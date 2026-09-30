#include "application_internal.h"

void Application::ArmConnectWatchdog() {
    if (connect_watchdog_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            uint32_t gen = self->connect_generation_.load();
            self->Schedule([self, gen]() { self->HandleConnectWatchdog(gen); });
        };
        args.arg = this;
        args.name = "connect_wdt";
        if (esp_timer_create(&args, &connect_watchdog_timer_) != ESP_OK) {
            connect_watchdog_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(connect_watchdog_timer_);
    // OpenAudioChannel can spend up to 10s waiting for server hello after the
    // socket connect. Wake-word invokes may retry; the watchdog must outlive that
    // budget or the first valid "Hi ESP" is reset to Idle before success returns.
    esp_timer_start_once(connect_watchdog_timer_, kConnectWatchdogTimeoutUs);
}

void Application::CancelConnectWatchdog() {
    if (connect_watchdog_timer_ != nullptr) {
        esp_timer_stop(connect_watchdog_timer_);
    }
}

void Application::HandleConnectWatchdog(uint32_t generation) {
    if (generation != connect_generation_.load()) {
        return;  // connect already resolved
    }
    const bool recovery_watchdog =
        chat_recovery_.opening && chat_recovery_.connect_generation == generation &&
        chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.lesson_generation == lesson_runtime_generation_.load();
    // Invalidate the still-running worker's eventual result, recover to Idle and
    // schedule a backoff retry. WebsocketProtocol keeps each open candidate
    // private until it has connected and received hello, so a timed-out passive
    // worker can no longer own reconnect forever.
    ++connect_generation_;
    connect_in_flight_.store(false);  // Attempt expired; reservation still owns the worker.
    if (chat_recovery_.opening && chat_recovery_.connect_generation == generation &&
        chat_recovery_.protocol_generation == protocol_generation_.load()) {
        chat_recovery_.connect_generation = connect_generation_.load();
        chat_recovery_.opening = chat_recovery_.adopted = false;
    }
    if (passive_ws_intent_.load()) {
        deferred_wake_word_.clear();
        const bool lesson_answer_turn = lesson_interactive_listen_pending_.load() ||
                                        lesson_interactive_listening_active_.load();
        if (lesson_runtime_active_.load() && lesson_answer_turn) {
            ESP_LOGW(TAG, "lesson passive connect watchdog timeout -> wait");
            backend_offline_.store(true);
            passive_ws_intent_.store(false);
            online_intent_.store(false);
            connect_attempt_active_.store(false);
            connect_in_flight_.store(false);
            auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateConnecting) {
                SetDeviceState(kDeviceStateIdle);
            }
            SchedulePassiveLessonReconnect();
            return;
        }
        ESP_LOGW(TAG, "passive_lesson_connect_watchdog_timeout -> passive backoff");
        backend_offline_.store(true);
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);
        if (GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        passive_ws_intent_.store(false);
        RearmClaimedIdleWakeWord();
        SchedulePassiveLessonReconnect();
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGW(TAG, "lesson connect watchdog timeout -> suppress generic reconnect");
        RequestLessonStorageAbandonment();
        backend_offline_.store(true);
        online_intent_.store(false);
        connect_attempt_active_.store(false);
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }
    // Explicit recovery expiry already made the UI Idle while the worker still
    // owned its reservation. Its watchdog must retain the background retry.
    if (recovery_watchdog && GetDeviceState() == kDeviceStateIdle && online_intent_.load() &&
        !reset_pending_.load() && !protocol_reinit_pending_.load() && !reboot_pending_.load()) {
        backend_offline_.store(true);
        ScheduleReconnect(reconnect_mode_, false);
        RearmClaimedIdleWakeWord();
        return;
    }
    if (GetDeviceState() == kDeviceStateConnecting) {
        ESP_LOGW(TAG, "connect_watchdog_timeout -> idle + backoff");
        backend_offline_.store(true);
        SetDeviceState(kDeviceStateIdle);
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());
    }
}

void Application::ScheduleReconnect(ListeningMode mode, bool resume_listening) {
    static constexpr int kFastReconnectAttempts = 6;
    static constexpr uint32_t kSlowReconnectRetryMs = 30000;
    reconnect_mode_ = mode;
    reconnect_resume_listening_.store(resume_listening);
    reconnect_passive_.store(false);
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleReconnectTick(); });
        };
        args.arg = this;
        args.name = "reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            return;
        }
    }
    uint32_t delay_ms = 0;
    if (reconnect_attempt_ < kFastReconnectAttempts) {
        // Fast recovery window: exponential backoff 0.5s -> 8s cap, plus
        // 0..50% jitter (anti fleet-sync).
        uint32_t base_ms = 500u << reconnect_attempt_;
        if (base_ms > 8000u) {
            base_ms = 8000u;
        }
        uint32_t jitter_ms = esp_random() % (base_ms / 2 + 1);
        delay_ms = base_ms + jitter_ms;
        reconnect_attempt_++;
        ESP_LOGW(TAG, "reconnect_scheduled attempt=%d phase=fast delay_ms=%lu", reconnect_attempt_,
                 (unsigned long)delay_ms);
    } else {
        // Long-horizon recovery: keep retrying slowly so a recovered endpoint
        // reconnects without another wake word or button press.
        uint32_t jitter_ms = esp_random() % (kSlowReconnectRetryMs / 4 + 1);
        delay_ms = kSlowReconnectRetryMs + jitter_ms;
        ESP_LOGW(TAG, "reconnect_slow_retry_scheduled attempt=%d phase=slow delay_ms=%lu",
                 reconnect_attempt_ + 1, (unsigned long)delay_ms);
        reconnect_attempt_ = kFastReconnectAttempts;
    }
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);  // OBS-2
    if (chat_cleanup_enabled_ && !lesson_runtime_active_.load() && online_intent_.load() &&
        RetainChatRecovery(ChatRecoveryIntent::Kind::Background, mode)) {
        chat_recovery_.ready = false;
        const auto now = static_cast<uint64_t>(esp_timer_get_time());
        const uint64_t delay = static_cast<uint64_t>(delay_ms) * 1000ULL;
        chat_recovery_.retry_at_us = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::SchedulePassiveLessonReconnect() {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    if (IsDeviceClaimed() && !lesson_runtime_active_.load() &&
        WifiManager::GetInstance().IsConnected() &&
        backend_recovery_window_.ShouldEnterWifiConfig(
            static_cast<uint64_t>(esp_timer_get_time() / 1000))) {
        ESP_LOGW(TAG, "passive_backend_timeout_entering_wifi_config");
        reconnect_passive_.store(false);
        passive_ws_intent_.store(false);
        static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
        return;
    }
#endif
    if (reconnect_passive_.load()) {
        ESP_LOGD(TAG, "passive_lesson_reconnect_already_pending");
        return;
    }
    if (reconnect_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleReconnectTick(); });
        };
        args.arg = this;
        args.name = "reconnect";
        if (esp_timer_create(&args, &reconnect_timer_) != ESP_OK) {
            reconnect_timer_ = nullptr;
            reconnect_passive_.store(false);
            return;
        }
    }
    uint32_t capped_attempt = passive_reconnect_attempt_ > 4 ? 4 : passive_reconnect_attempt_;
    uint32_t base_ms = 500u << capped_attempt;
    uint32_t jitter_ms = esp_random() % (base_ms / 2 + 1);
    uint32_t delay_ms = base_ms + jitter_ms;
    passive_reconnect_attempt_++;
    reconnect_passive_.store(true);
    ESP_LOGW(TAG, "passive_lesson_reconnect_scheduled attempt=%d delay_ms=%lu",
             passive_reconnect_attempt_, (unsigned long)delay_ms);
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, (uint64_t)delay_ms * 1000ULL);
}

void Application::HandleReconnectTick() {
    if (chat_cleanup_enabled_ && chat_protocol_signals_ &&
        chat_protocol_signals_->SourceSelected() && !reconnect_passive_.load() &&
        !lesson_runtime_active_.load()) {
        if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None ||
            static_cast<uint64_t>(esp_timer_get_time()) < chat_recovery_.retry_at_us)
            return;
        chat_recovery_.ready = true;
        PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
        return;
    }
    if (protocol_work_lifetime_.Pending()) {
        reconnect_passive_.store(false);
        return;
    }
    if (protocol_ == nullptr) {
        reconnect_attempt_ = 0;
        passive_reconnect_attempt_ = 0;
        reconnect_passive_.store(false);
        connect_attempt_active_.store(false);
        return;
    }
    if (reconnect_passive_.exchange(false)) {
        if (lesson_asset_sync_quiet_.load()) {
            ESP_LOGI(TAG, "lesson asset sync quiet deferred passive reconnect");
            SchedulePassiveLessonReconnect();
            return;
        }
        if (protocol_->IsAudioChannelOpened()) {
            passive_reconnect_attempt_ = 0;
            return;
        }
        if (connect_in_flight_.load() || protocol_work_lifetime_.Busy()) {
            SchedulePassiveLessonReconnect();
            return;
        }
        auto state = GetDeviceState();
        if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) {
            passive_reconnect_attempt_ = 0;
            return;
        }
        if (state != kDeviceStateIdle) {
            const bool lesson_answer_turn =
                lesson_runtime_active_.load() && (lesson_interactive_listen_pending_.load() ||
                                                  lesson_interactive_listening_active_.load());
            if (lesson_answer_turn &&
                (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
                 state == kDeviceStateConnecting)) {
                ESP_LOGI(TAG, "passive_lesson_reconnect_tick answer_turn state=%d",
                         static_cast<int>(state));
                StartPassiveLessonWebsocket();
                return;
            }
            SchedulePassiveLessonReconnect();
            return;
        }
        ESP_LOGI(TAG, "passive_lesson_reconnect_tick attempt=%d", passive_reconnect_attempt_);
        StartPassiveLessonWebsocket();
        return;
    }
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet deferred voice reconnect");
        ScheduleReconnect(reconnect_mode_, reconnect_resume_listening_.load());
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson reconnect ignored");
        reconnect_attempt_ = 0;
        connect_attempt_active_.store(false);
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        reconnect_attempt_ = 0;  // user moved on; abandon the retry chain
        connect_attempt_active_.store(false);
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        reconnect_attempt_ = 0;
        connect_attempt_active_.store(false);
        return;
    }
    if (connect_in_flight_.load() || protocol_work_lifetime_.Busy()) {
        ScheduleReconnect(
            reconnect_mode_,
            reconnect_resume_listening_.load());  // previous worker still finishing; retry later
        return;
    }
    ESP_LOGI(TAG, "reconnect_tick attempt=%d", reconnect_attempt_);
    SetDeviceState(kDeviceStateConnecting);
    ContinueOpenAudioChannel(reconnect_mode_);
}

void Application::HandleStartListeningEvent() {
    if (IsWifiConfigEntryPending())
        return;
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !lesson_runtime_active_.load() &&
        (chat_protocol_owned_.load() ||
         chat_source_connect_generation_.load() != connect_generation_.load())) {
        BeginChatListen(kListeningModeManualStop, ChatListenOrigin::User);
        return;
    }
    if (IsSelectedNormalChatRoute()) {
        if (GetDeviceState() == kDeviceStateSpeaking &&
            !(chat_rearm_phase_ == ChatRearmPhase::Pending &&
              chat_rearm_mode_ == kListeningModeManualStop)) {
            ConnectionSource source;
            if (!protocol_ || lesson_asset_sync_quiet_.load() ||
                !chat_protocol_signals_->TrySource(source) ||
                protocol_->CurrentConnectionEpoch() != source.connection_epoch)
                return;
            if (!HandleChatAbort(kAbortReasonNone, false) || chat_playout_recovery_)
                return;
        }
        BeginChatListen(kListeningModeManualStop, ChatListenOrigin::User);
        return;
    }
    auto state = GetDeviceState();
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored start listening state=%d",
                 static_cast<int>(state));
        return;
    }
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() || lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson start listening ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        if (!audio_service_.IsRunning()) {
            ESP_LOGI(TAG, "Audio test unavailable while provisioning workers are deferred");
            return;
        }
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        if (lesson_interactive_listen_pending_.load()) {
            ESP_LOGI(TAG, "lesson prompt still speaking; defer listening");
            listening_mode_ = kListeningModeManualStop;
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearChatMessages();
                display->SetStatus("Sắp đến lượt con...");
            }
            return;
        }
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateListening) {
        ESP_LOGI(TAG, "lesson/manual listening rearm");
        listening_mode_ = kListeningModeManualStop;
        if (lesson_interactive_listening_active_.load() && !lesson_interactive_listen_pending_.load()) {
            ESP_LOGI(TAG, "lesson listening already active; duplicate start ignored");
            return;
        }
        if (lesson_interactive_listen_pending_.exchange(false)) {
            lesson_interactive_listening_active_.store(true);
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearChatMessages();
                display->SetStatus("Con nói nhé...");
                display->SetChatMessage("system", "Con nói nhé.");
            }
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        {
            int64_t now_ms = esp_timer_get_time() / 1000;
            listening_started_ms_.store(now_ms);
            last_listening_activity_ms_.store(now_ms);
        }
        protocol_->SendStartListening(kListeningModeManualStop);
        if (!RequestChatLessonCapture())
            audio_service_.EnableVoiceProcessing(true);
    }
}
