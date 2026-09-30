#include "application_internal.h"

void Application::RequestInitializeProtocol(ProtocolActivation activation) {
    auto request = [this, activation]() {
        if (activation != ProtocolActivation::kNone) {
            protocol_activation_pending_ = activation;
        }
        ++connect_generation_;
        reset_pending_.store(true);
        protocol_reinit_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReinitialize);
        CompletePendingProtocolWork();
    };
    if (xTaskGetCurrentTaskHandle() == application_task_)
        request();
    else
        Schedule(std::move(request));
}

void Application::CompleteProtocolActivation() {
    if (protocol_heap_monitor_pending_) {
        SystemInfo::PrintHeapCheckpoint("protocol_init.complete");
        SystemInfo::StopHeapPhaseMonitor();
        protocol_heap_monitor_pending_ = false;
    }
    const auto activation = protocol_activation_pending_;
    protocol_activation_pending_ = ProtocolActivation::kNone;
    if (claim_protocol_completion_pending_) {
        claim_protocol_completion_pending_ = false;
        CompleteClaimProtocolActivation();
    }
    if (activation == ProtocolActivation::kNone)
        return;
    SystemInfo::PrintHeapCheckpoint(activation == ProtocolActivation::kNormal
                                        ? "activation.complete"
                                        : "wifi_reprovision_activation.complete");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::InitializeProtocol() {
    if (protocol_activation_pending_ == ProtocolActivation::kNormal) {
        SystemInfo::StartHeapPhaseMonitor();
        protocol_heap_monitor_pending_ = true;
    }
    backend_recovery_window_.Reset();
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    bool is_websocket_protocol = false;
    std::string transient_evidence_journey_id = ota_->TakeTransientEvidenceJourneyId();
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    Settings websocket_settings("websocket", false);
    const bool has_configured_websocket_url = !websocket_settings.GetString("url", "").empty();
    const bool prefer_claimed_websocket = IsDeviceClaimed() && has_configured_websocket_url;
    const bool has_available_websocket_url =
        !websocket_settings.GetString("url", CONFIG_WEBSOCKET_URL).empty();
    if (ota_->HasMqttConfig() && !prefer_claimed_websocket) {
        SecureClearString(transient_evidence_journey_id);
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig() || has_available_websocket_url) {
        auto websocket_protocol = std::make_unique<WebsocketProtocol>();
        websocket_protocol->SetTransientConfig(ota_->GetTransientWebsocketUrl(),
                                               ota_->GetTransientWebsocketToken(),
                                               std::move(transient_evidence_journey_id));
        websocket_protocol->SetUnclaimedPublicLessonOnly(!IsDeviceClaimed());
        protocol_ = std::move(websocket_protocol);
        is_websocket_protocol = true;
    } else {
        SecureClearString(transient_evidence_journey_id);
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#else
    auto websocket_protocol = std::make_unique<WebsocketProtocol>();
    websocket_protocol->SetTransientConfig(ota_->GetTransientWebsocketUrl(),
                                           ota_->GetTransientWebsocketToken(),
                                           std::move(transient_evidence_journey_id));
    websocket_protocol->SetUnclaimedPublicLessonOnly(false);
    protocol_ = std::move(websocket_protocol);
    is_websocket_protocol = true;
#endif
    SecureClearString(transient_evidence_journey_id);
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);

    Protocol* callback_protocol = protocol_.get();
    const uint64_t callback_protocol_generation =
        protocol_generation_.load(std::memory_order_acquire);
    try {
        std::atomic_store(&chat_protocol_signals_, std::make_shared<ChatProtocolSignals>());
        chat_source_open_handled_ = 0;
        chat_source_failure_handled_ = 0;
        chat_passive_ping_id_ = 0;
    } catch (...) {
        std::atomic_store(&chat_protocol_signals_, std::shared_ptr<ChatProtocolSignals>());
        chat_protocol_fault_ = true;
        chat_protocol_infrastructure_fault_ = true;
    }
    const auto callback_signals = chat_protocol_signals_;
    protocol_->OnConnected([this, callback_protocol_generation, callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        Schedule([this, callback_protocol_generation, callback_signals, callback_era]() {
            if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era))
                return;
            if (callback_protocol_generation != protocol_generation_.load() ||
                chat_protocol_owned_.load(std::memory_order_acquire))
                return;
            backend_recovery_window_.Reset();
            if (IsConnectSuccessPublicationSuppressed()) {
                ESP_LOGI(TAG, "connect success publication suppressed");
                online_intent_.store(false);
                StopHeartbeat();
                return;
            }
            backend_offline_.store(false);
            DismissAlert();
            const bool lesson_answer_turn = lesson_interactive_listen_pending_.load() ||
                                            lesson_interactive_listening_active_.load();
            if (lesson_runtime_active_.load() && !lesson_answer_turn) {
                ESP_LOGI(TAG, "lesson protocol connected without heartbeat");
                StopHeartbeat();
                return;
            }
            StartHeartbeat();
            DispatchDeviceHeartbeat();
        });
    });

    protocol_->OnNetworkError(
        [this, callback_protocol_generation, callback_signals](const std::string& message) {
            const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
            if (callback_signals && callback_signals->Deferred()) {
                if (callback_signals->Publish(callback_era, ChatProtocolSignals::Error)) {
                    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                }
                return;
            }
            try {
                Schedule([this, callback_protocol_generation, callback_signals, callback_era,
                          message]() {
                    if (callback_signals &&
                        (!callback_era || callback_signals->Capture() != callback_era))
                        return;
                    if (callback_protocol_generation != protocol_generation_.load() ||
                        chat_protocol_owned_.load(std::memory_order_acquire))
                        return;
                    backend_offline_.store(true);
                    if (ShouldKeepManagementHeartbeat()) {
                        StartHeartbeat();
                        DispatchDeviceHeartbeat();
                    } else {
                        StopHeartbeat();
                    }
                    last_error_message_ = message;
                    xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
                });
            } catch (...) {
                if (callback_signals)
                    callback_signals->Publish(callback_era, ChatProtocolSignals::Error);
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            }
        });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (!lesson_audio_playout_.AllowsAudio(speaking_generation_.load()))
            return;
        if (GetDeviceState() == kDeviceStateSpeaking || tts_audio_accepting_.load()) {
            last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
            packet->generation = speaking_generation_.load();
            packet->conversation_audio = true;
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board, callback_protocol,
                                     callback_protocol_generation, callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        const auto callback_connect_generation = protocol_callback_connect_generation_.load();
        const int callback_sample_rate = callback_protocol->server_sample_rate();
        Schedule([this, codec, &board, callback_protocol_generation, callback_connect_generation,
                  callback_signals, callback_era, callback_sample_rate]() {
            if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era))
                return;
            if (callback_protocol_generation != protocol_generation_.load() ||
                callback_connect_generation != connect_generation_.load() ||
                chat_protocol_owned_.load(std::memory_order_acquire))
                return;
            backend_recovery_window_.Reset();
            if (IsConnectSuccessPublicationSuppressed()) {
                ESP_LOGI(TAG, "audio channel success publication suppressed");
                online_intent_.store(false);
                StopHeartbeat();
                return;
            }
            if (passive_ws_intent_.load()) {
                if (IsDeviceClaimed() && !lesson_runtime_active_.load()) {
                    ESP_LOGI(TAG,
                             "claimed passive lesson websocket opened with management heartbeat");
                    StartHeartbeat();
                    DispatchDeviceHeartbeat();
                }
                if (!IsDeviceClaimed() || lesson_runtime_active_.load()) {
                    ESP_LOGI(TAG, "unclaimed passive lesson websocket opened without heartbeat");
                    StopHeartbeat();
                }
            } else {
                const bool lesson_answer_turn = lesson_interactive_listen_pending_.load() ||
                                                lesson_interactive_listening_active_.load();
                if (lesson_runtime_active_.load() && !lesson_answer_turn) {
                    ESP_LOGI(TAG, "lesson audio channel opened ignored");
                    online_intent_.store(false);
                    StopHeartbeat();
                    return;
                }
                online_intent_.store(true);
                StartHeartbeat();
                DispatchDeviceHeartbeat();
            }
            backend_offline_.store(false);
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            if (IsDeviceClaimed()) {
                Schedule([this, callback_protocol_generation, callback_connect_generation,
                          callback_signals, callback_era]() {
                    if (callback_signals &&
                        (!callback_era || callback_signals->Capture() != callback_era))
                        return;
                    if (callback_protocol_generation != protocol_generation_.load() ||
                        callback_connect_generation != connect_generation_.load() ||
                        chat_protocol_owned_.load(std::memory_order_acquire))
                        return;
                    StopClaimPoll();
                });
            }
            if (callback_sample_rate != codec->output_sample_rate()) {
                ESP_LOGW(TAG,
                         "Server sample rate %d does not match device output sample rate %d, "
                         "resampling may cause distortion",
                         callback_sample_rate, codec->output_sample_rate());
            }
        });
    });

    protocol_->OnAudioChannelClosed([this, callback_protocol, callback_protocol_generation,
                                     callback_signals]() {
        const auto callback_era = callback_signals ? callback_signals->Capture() : 0;
        if (callback_signals && callback_signals->Deferred()) {
            if (callback_signals->Publish(callback_era, ChatProtocolSignals::Closed)) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            }
            return;
        }
        Schedule([this, callback_protocol, callback_protocol_generation, callback_signals,
                  callback_era]() {
            if (callback_signals && (!callback_era || callback_signals->Capture() != callback_era))
                return;
            if (chat_protocol_owned_.load(std::memory_order_acquire))
                return;
            if (!ProtocolLifetimeMatches(protocol_.get(), callback_protocol,
                                         protocol_generation_.load(std::memory_order_acquire),
                                         callback_protocol_generation)) {
                return;
            }
            speaking_arm_dispatch_.Cancel();
            tts_audio_accepting_.store(false);
            if (ShouldKeepManagementHeartbeat()) {
                StartHeartbeat();
                DispatchDeviceHeartbeat();
            } else {
                StopHeartbeat();
            }
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            RequestLessonStorageAbandonment();
            auto display = Board::GetInstance().GetDisplay();
            if (!lesson_runtime_active_.load()) {
                display->SetChatMessage("system", "");
            }
            if (GetDeviceState() == kDeviceStateWifiConfiguring ||
                GetDeviceState() == kDeviceStateAudioTesting) {
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {
                }
                return;
            }
            if (lesson_runtime_active_.load() && passive_ws_intent_.load()) {
                while (audio_service_.PopPacketFromSendQueue() != nullptr) {}
                RequestLessonStorageAbandonment();
                if (PassiveReconnectHasOwner(reconnect_passive_.load(),
                                             connect_in_flight_.load())) {
                    ESP_LOGI(TAG, "lesson passive_liveness_reconnect_pending");
                    return;
                }
                ESP_LOGW(TAG, "lesson passive ws dropped -> passive reconnect");
                SchedulePassiveLessonReconnect();
                return;
            }
            if (connect_in_flight_.load()) {
                ESP_LOGW(TAG, "ws_close_ignored_during_connect");
                return;
            }
            SetDeviceState(kDeviceStateIdle);
            while (audio_service_.PopPacketFromSendQueue() != nullptr) {
            }
            if (passive_ws_intent_.load()) {
                if (reconnect_passive_.load()) {
                    ESP_LOGI(TAG, "passive_liveness_reconnect_pending");
                    return;
                }
                ESP_LOGW(TAG, "passive_lesson_ws_dropped_unexpected -> passive reconnect");
                SchedulePassiveLessonReconnect();
                return;
            }
            if (online_intent_.load()) {
                if (lesson_runtime_active_.load()) {
                    ESP_LOGW(TAG, "lesson ws dropped unexpected -> suppress generic reconnect");
                    RequestLessonStorageAbandonment();
                    online_intent_.store(false);
                    lesson_interactive_listen_generation_.fetch_add(1);
                    lesson_interactive_listen_pending_.store(false);
                    lesson_interactive_listening_active_.store(false);
                    backend_offline_.store(true);
                    audio_service_.ResetDecoder();
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                    return;
                }
                ESP_LOGW(TAG, "ws_dropped_unexpected -> auto-reconnect (online_intent)");
                backend_offline_.store(true);
                audio_service_.ResetDecoder();
                display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
                display->SetEmotion("thinking");
                audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
                ScheduleReconnect(GetDefaultListeningMode(), false);
                return;
            }
            if (lesson_runtime_active_.load()) {
                RequestLessonStorageAbandonment();
            }
        });
    });

    protocol_->OnIncomingJson([this, is_websocket_protocol](const cJSON* root, uint64_t epoch) {
        DispatchIncomingJson(root, epoch, is_websocket_protocol);
    });

    if (!InitializeChatSourceRoute(is_websocket_protocol)) {
        backend_offline_.store(true);
        online_intent_.store(false);
        microphone_uplink_authorized_.store(false);
        display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
        return;
    }

    // WebSocket Start() opens the realtime audio channel.
    if (is_websocket_protocol) {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
        ESP_LOGI(TAG, "Course-mode local endpoint: opening private-LAN WebSocket");
#endif
        if (IsDeviceClaimed()) {
            ESP_LOGI(TAG, "Claimed device: opening passive WebSocket for lesson/nudge");
            StartPassiveLessonWebsocket();
        } else {
            ESP_LOGI(TAG, "Unclaimed device: opening passive WebSocket for public lesson sync");
            StartPassiveLessonWebsocket();
        }
    } else {
        StartProtocolWorker();
    }
    if (is_websocket_protocol)
        CompleteProtocolActivation();
}
