#include "application_internal.h"

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) {
        // Explicit BOOT Wi-Fi setup owns the screen. A stale STA connected event
        // from the previous online session must not leave setup mode and render
        // ONLINE / "Connected" before the phone finishes provisioning. BluFi
        // success has its own path: it reports to the phone, stops BLE, then
        // schedules RefreshPendingTbotClaim().
        ESP_LOGI(TAG, "Network connected ignored because WiFi config mode is active");
        auto display = Board::GetInstance().GetDisplay();
        display->UpdateStatusBar(true);
        return;
    }

    if (state == kDeviceStateStarting) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        // Unclaimed + BLE advertising leaves ~7–8KB largest free internal block.
        // The normal activation worker needs 8KB stack and fails to create, so the
        // UI freezes on "Loading setup..." forever. Run only the protocol setup
        // needed for public lesson sync and leave claimed bootstrap to BLE.
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
        if (!IsDeviceClaimed()) {
            ESP_LOGW(TAG,
                     "Unclaimed device on Wi-Fi: run minimal activation transport "
                     "without claimed bootstrap worker");
            CompleteUnclaimedProtocolOnlyActivation();
            return;
        }
#endif
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        BaseType_t created = xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create activation task (heap exhausted?)");
            activation_task_handle_ = nullptr;
            if (!ota_) {
                ota_ = std::make_unique<Ota>();
                ota_->MarkCurrentVersionValid();
            }
            xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
        }
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    RequestLessonStorageAbandonment();
    backend_recovery_window_.Reset();
    // H2: network is gone -> stop the heartbeat (no live online session to report
    // and no point blocking the main task on an unreachable backend). It restarts
    // only from OnConnected.
    StopHeartbeat();

    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    auto display = Board::GetInstance().GetDisplay();
    if (chat_cleanup_enabled_ && chat_protocol_signals_ &&
        chat_protocol_signals_->SourceSelected() && !IsLessonVoiceRoute() &&
        (online_intent_.load() || passive_ws_intent_.load())) {
        // Radio loss must retain recovery intent; an intentional close cancels it.
        ConnectionSource source;
        if (chat_protocol_signals_->TrySource(source)) {
            ESP_LOGW(TAG, "chat_source_fault reason=wifi_disconnected");
            chat_protocol_signals_->PublishConnectionFault(
                source, chat_source_connect_generation_.load(), ChatProtocolSignals::Error);
        }
        PollChatProtocolSignals();
        display->UpdateStatusBar(true);
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        backend_offline_.store(true);
        if (chat_cleanup_enabled_) {
            if (chat_protocol_signals_)
                chat_protocol_signals_->Disable();
            tts_audio_accepting_.store(false);
            microphone_uplink_authorized_.store(false);
            RequestChatAudioCleanup(speaking_generation_.load(), true, false, false);
        } else
            audio_service_.ResetDecoder();
        CloseAudioChannelByIntent();
        if (lesson_runtime_active_.load()) {
            lesson_interactive_listen_generation_.fetch_add(1);
            lesson_interactive_listen_pending_.store(false);
            lesson_interactive_listening_active_.store(false);
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        } else {
            display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
            display->SetEmotion("thinking");
            if (chat_cleanup_enabled_)
                RequestChatCue(Lang::Sounds::OGG_EXCLAMATION);
            else
                audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    }

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::RearmClaimedIdleWakeWord() {
    if (IsWifiConfigEntryPending())
        return;
    if (chat_cleanup_enabled_) {
        ConnectionSource source;
        if (IsDeviceClaimed() && !lesson_runtime_active_.load() &&
            !lesson_asset_sync_quiet_.load() && GetDeviceState() == kDeviceStateIdle &&
            !connect_in_flight_.load() &&
            (!passive_ws_intent_.load() ||
             (chat_protocol_signals_ && chat_protocol_signals_->TrySource(source))))
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
        return;
    }
    if (!IsDeviceClaimed() || lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load() ||
        GetDeviceState() != kDeviceStateIdle || connect_in_flight_.load() ||
        (passive_ws_intent_.load() &&
         (protocol_ == nullptr || !protocol_->IsAudioChannelOpened()))) {
        return;
    }
    audio_service_.EnableWakeWordDetection(true);
    ESP_LOGI(TAG, "claimed_idle_wake_word_rearmed running=%d",
             audio_service_.IsWakeWordRunning() ? 1 : 0);
}

void Application::HandleActivationDoneEvent() {
    auto state = GetDeviceState();
    if (state == kDeviceStateWifiConfiguring) {
        ESP_LOGI(TAG, "Activation done ignored because WiFi config mode is active");
        return;
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Activation done ignored because runtime audio is active");
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "Activation done ignored because lesson runtime is active");
        return;
    }

    ESP_LOGI(TAG, "Activation done");

    // Migrate robots that received a heartbeat revocation on older firmware.
    // That handler cleared only device_secret, leaving device_id plus the
    // factory-test WebSocket marker to impersonate a claimed device forever.
    if (HasStaleRevokedClaimIdentity()) {
        ESP_LOGW(TAG, "Detected stale revoked claim identity; reopening WiFi setup");
        HandleHeartbeatAuthFailure(401);
        return;
    }

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);
    if (ShouldKeepManagementHeartbeat()) {
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    }
    RearmClaimedIdleWakeWord();

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    ota_.reset();
#endif
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    RefreshPendingTbotClaim();

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}


