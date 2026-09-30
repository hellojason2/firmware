#include "application_internal.h"

namespace {
}  // namespace

void Application::StartPassiveLessonWebsocket() {
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    if (protocol_->IsAudioChannelOpened() || connect_in_flight_.load()) {
        return;
    }
    passive_ws_intent_.store(true);
    online_intent_.store(false);
    uint32_t gen = ++connect_generation_;
    connect_in_flight_.store(true);
    ArmConnectWatchdog();
    auto* ctx = new ConnectContext{this, GetDefaultListeningMode(), gen, std::string(), false, true};
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        connect_in_flight_.store(false);
        passive_ws_intent_.store(false);
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "lesson_ws worker unavailable");
        SchedulePassiveLessonReconnect();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }
    if (protocol_ == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        SetDeviceState(kDeviceStateIdle);  // SM-3: don't wedge CONNECTING
        return;
    }
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() || lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson open channel ignored state=%d", static_cast<int>(GetDeviceState()));
        online_intent_.store(false);
        connect_attempt_active_.store(false);
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        SetListeningMode(mode);
        return;
    }
    if (connect_in_flight_.load()) {
        ESP_LOGW(TAG, "connect already in flight, ignoring duplicate request");
        return;
    }
    // SM-1/WSS-1: OpenAudioChannel() blocks (TCP+TLS handshake + server hello,
    // up to ~20s). Queue it on the persistent worker so reconnects do not depend
    // on finding a fresh contiguous 8KB internal heap block. connect_generation_
    // invalidates a stale result; the
    // connect watchdog (SM-3) recovers a wedged/black-hole connect.
    reconnect_mode_ = mode;
    online_intent_.store(true);  // we want an open channel -> reconnect on unexpected drop
    uint32_t gen = ++connect_generation_;
    connect_in_flight_.store(true);
    connect_attempt_active_.store(true);  // WSS-8: suppress per-attempt error banner until terminal
    ArmConnectWatchdog();
    passive_ws_intent_.store(false);
    auto* ctx = new ConnectContext{this, mode, gen, std::string(), false, false};
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);  // WSS-8: no worker -> cycle ended
        CancelConnectWatchdog();
        ESP_LOGE(TAG, "ws_open worker unavailable -> idle");
        SetDeviceState(kDeviceStateIdle);
    }
}

bool Application::StartOpenChannelWorker(void* context) {
    RetireChatOutbound();
    if (chat_protocol_infrastructure_fault_)
        return false;
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        return false;
    }
    auto* ctx = static_cast<ConnectContext*>(context);
    if (!protocol_ || protocol_work_lifetime_.Pending() || protocol_work_lifetime_.Busy())
        return false;
    ctx->reservation = protocol_work_lifetime_.Reserve();
    if (!ctx->reservation)
        return false;
    ctx->protocol = protocol_.get();
    ctx->protocol_generation = protocol_generation_.load();
    const NetworkWorkItem work{NetworkWorkKind::kOpenChannel, context};
    if (xQueueSend(open_channel_queue, &work, 0) == pdTRUE)
        return true;
    protocol_work_lifetime_.Release(ctx->reservation);
    ctx->reservation = 0;
    return false;
}

void Application::StartProtocolWorker() {
    auto* ctx = new ConnectContext{this, GetDefaultListeningMode(), connect_generation_.load(),
                                   std::string()};
    ctx->start_protocol = true;
    if (!StartOpenChannelWorker(ctx)) {
        delete ctx;
        protocol_start_pending_generation_ = protocol_generation_.load();
        return;
    }
    protocol_start_pending_generation_ = 0;
}

void Application::OpenChannelTask(void* arg) {
    auto* self = static_cast<Application*>(arg);
    for (;;) {
        NetworkWorkItem work{};
        if (xQueueReceive(open_channel_queue, &work, portMAX_DELAY) != pdTRUE ||
            work.context == nullptr) {
            continue;
        }
        if (work.kind == NetworkWorkKind::kHeartbeat) {
            Application::HeartbeatTask(work.context);
            continue;
        }
        if (work.kind == NetworkWorkKind::kProtocolCleanup) {
            self->RunChatProtocolCleanup();
            continue;
        }
        auto* ctx = static_cast<ConnectContext*>(work.context);
        ListeningMode mode = ctx->mode;
        uint32_t gen = ctx->generation;
        std::string wake_word = ctx->wake_word;
        bool wake_word_invoke = ctx->wake_word_invoke;
        bool passive_preconnect = ctx->passive_preconnect;
        Protocol* worker_protocol = ctx->protocol;
        const uint64_t worker_protocol_generation = ctx->protocol_generation;
        const uint64_t reservation = ctx->reservation;
        const bool start_protocol = ctx->start_protocol;
        delete ctx;
        self->protocol_callback_connect_generation_.store(gen);

        if (self->protocol_work_lifetime_.Pending() ||
            (!start_protocol && gen != self->connect_generation_.load())) {
            self->Schedule([self, reservation, start_protocol, worker_protocol_generation, gen]() {
                if (!self->protocol_work_lifetime_.Release(reservation))
                    return;
                if (!start_protocol)
                    self->CompleteChatRecoveryOpen(gen, false);
                if (start_protocol &&
                    worker_protocol_generation == self->protocol_generation_.load()) {
                    // A close may cancel queued audio work, but control startup is
                    // still owed unless the drain resets/replaces this protocol.
                    self->protocol_start_pending_generation_ = worker_protocol_generation;
                }
                self->CompletePendingProtocolWork();
            });
            continue;
        }

        if (start_protocol) {
            worker_protocol->Start();
            self->Schedule([self, reservation, worker_protocol_generation]() {
                if (!self->protocol_work_lifetime_.Release(reservation))
                    return;
                if (self->CompletePendingProtocolWork())
                    return;
                if (worker_protocol_generation == self->protocol_generation_.load()) {
                    self->CompleteProtocolActivation();
                }
            });
            continue;
        }

        // The ONLY blocking call, now off the app task.
        bool ok = false;
        int max_attempts = wake_word_invoke ? kWakeWordAudioChannelOpenMaxAttempts : 1;
        for (int attempt = 1;
             !self->protocol_work_lifetime_.Pending() && gen == self->connect_generation_.load() &&
             !worker_protocol->IsAudioChannelOpened() && attempt <= max_attempts;
             ++attempt) {
            worker_protocol->SetIncomingJsonTransportEpoch(
                self->lesson_transport_epoch_gate_.PublishedEpoch());
            ok = worker_protocol->OpenAudioChannel();
            if (ok) {
                break;
            }
            if (wake_word_invoke) {
                ESP_LOGW(TAG, "wake_audio_channel_open_failed attempt=%d max=%d", attempt,
                         kWakeWordAudioChannelOpenMaxAttempts);
            }
            if (wake_word_invoke && attempt < max_attempts) {
                vTaskDelay(pdMS_TO_TICKS(kWakeWordAudioChannelRetryDelayMs));
            }
        }
        if (worker_protocol->IsAudioChannelOpened()) {
            ok = true;
        }
        self->Schedule([self, ok, mode, gen, wake_word, wake_word_invoke, passive_preconnect,
                        reservation, worker_protocol_generation]() {
            if (!self->protocol_work_lifetime_.Release(reservation))
                return;
            if (gen == self->connect_generation_.load()) {
                self->connect_in_flight_.store(false);
                self->CancelConnectWatchdog();
            }
            if (self->protocol_work_lifetime_.Pending())
                self->CompleteChatRecoveryOpen(gen, false);
            if (self->CompletePendingProtocolWork())
                return;
            if (gen != self->connect_generation_.load() ||
                worker_protocol_generation != self->protocol_generation_.load()) {
                return;  // superseded by a newer connect or the watchdog
            }
            if (self->CompleteChatRecoveryOpen(gen, ok))
                return;
            if (!passive_preconnect) {
                const DeviceState state = self->GetDeviceState();
                if (wake_word_invoke) {
                    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
                        return;
                    }
                } else if (state != kDeviceStateConnecting) {
                    return;
                }
            }
            if (ok) {
                self->backend_offline_.store(false);
                self->reconnect_attempt_ = 0;
                self->connect_attempt_active_.store(
                    false);  // WSS-8: connect cycle resolved (success)
                if (passive_preconnect) {
                    self->passive_reconnect_attempt_ = 0;
                    self->reconnect_passive_.store(false);
                    ESP_LOGI(TAG, "passive_lesson_websocket_opened");
                    const bool lesson_answer_turn =
                        self->lesson_interactive_listen_pending_.load() ||
                        self->lesson_interactive_listening_active_.load();
                    if (self->lesson_runtime_active_.load() && lesson_answer_turn) {
                        self->passive_ws_intent_.store(false);
                        self->StartHeartbeat();
                        self->DispatchDeviceHeartbeat();
                        self->SetListeningMode(kListeningModeManualStop);
                    } else if (self->IsDeviceClaimed() && !self->lesson_runtime_active_.load()) {
                        if (!self->lesson_asset_sync_quiet_.load()) {
                            const std::string deferred_wake_word = self->deferred_wake_word_;
                            self->deferred_wake_word_.clear();
                            if (!deferred_wake_word.empty()) {
                                ESP_LOGI(TAG, "passive_lesson_deferred_wake_resumed");
                                self->FinishWakeWordInvoke(deferred_wake_word);
                            } else {
                                // Give the server's initial asset burst time to
                                // enter quiet mode before allocating the AFE.
                                self->ScheduleLessonAssetSyncWakeRearm(5000ULL * 1000ULL);
                            }
                        }
                    }
                } else if (wake_word_invoke) {
                    self->FinishWakeWordInvoke(wake_word);
                } else {
                    const bool lesson_answer_turn =
                        self->lesson_interactive_listen_pending_.load() ||
                        self->lesson_interactive_listening_active_.load();
                    if (self->lesson_runtime_active_.load() && !lesson_answer_turn) {
                        ESP_LOGI(TAG, "lesson open worker ignored state=%d",
                                 static_cast<int>(self->GetDeviceState()));
                        self->online_intent_.store(false);
                        return;
                    }
                    if (self->reconnect_resume_listening_.exchange(true)) {
                        self->SetListeningMode(mode);
                    } else {
                        self->SetDeviceState(kDeviceStateIdle);
                    }
                }
            } else {
                const bool lesson_answer_turn = self->lesson_interactive_listen_pending_.load() ||
                                                self->lesson_interactive_listening_active_.load();
                if (self->lesson_runtime_active_.load()) {
                    if (lesson_answer_turn || (!passive_preconnect && !wake_word_invoke)) {
                        ESP_LOGW(TAG, "lesson open_audio_channel_failed -> wait");
                        self->backend_offline_.store(true);
                        self->passive_ws_intent_.store(false);
                        self->online_intent_.store(false);
                        self->connect_attempt_active_.store(false);
                        if (!lesson_answer_turn) {
                            self->lesson_interactive_listen_generation_.fetch_add(1);
                            self->lesson_interactive_listen_pending_.store(false);
                            self->lesson_interactive_listening_active_.store(false);
                        }
                        self->lesson_idle_repaint_suppressed_.store(true);
                        if (self->GetDeviceState() == kDeviceStateConnecting) {
                            self->SetDeviceState(kDeviceStateIdle);
                        }
                        auto display = Board::GetInstance().GetDisplay();
                        display->SetStatus(Lang::Strings::PLEASE_WAIT);
                        if (lesson_answer_turn) {
                            self->SchedulePassiveLessonReconnect();
                        }
                        return;
                    }
                }
                if (passive_preconnect) {
                    ESP_LOGW(TAG, "passive_lesson_websocket_failed");
                    self->deferred_wake_word_.clear();
                    self->passive_ws_intent_.store(false);
                    if (self->GetDeviceState() == kDeviceStateConnecting) {
                        self->SetDeviceState(kDeviceStateIdle);
                    }
                    if (self->ShouldKeepManagementHeartbeat()) {
                        self->StartHeartbeat();
                        self->DispatchDeviceHeartbeat();
                    }
                    self->RearmClaimedIdleWakeWord();
                    self->SchedulePassiveLessonReconnect();
                } else if (wake_word_invoke) {
                    ESP_LOGW(TAG, "wake_audio_channel_open_failed -> idle");
                } else {
                    ESP_LOGW(TAG, "open_audio_channel_failed -> idle + backoff");
                }
                self->backend_offline_.store(true);
                if (wake_word_invoke) {
                    self->audio_service_.EnableWakeWordDetection(true);
                }
                if (self->GetDeviceState() == kDeviceStateConnecting) {
                    self->SetDeviceState(kDeviceStateIdle);
                }
                if (!wake_word_invoke && !passive_preconnect) {
                    self->ScheduleReconnect(
                        mode,
                        self->reconnect_resume_listening_.load());  // WSS-4: long-horizon retry
                } else if (wake_word_invoke) {
                    // WSS-8: the wake open-loop exhausted all attempts -> terminal.
                    // Per-attempt errors were suppressed (connect_attempt_active_),
                    // so surface the offline banner exactly once now.
                    self->connect_attempt_active_.store(false);
                    xEventGroupSetBits(self->event_group_, MAIN_EVENT_ERROR);
                }
            }
        });
    }
}
