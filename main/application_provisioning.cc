#include "application_internal.h"

bool Application::SetDeviceState(DeviceState state) {
    if (state != kDeviceStateIdle)
        lesson_asset_sync_wake_invalidated_.store(true);
    if (state != kDeviceStateSpeaking)
        speaking_arm_dispatch_.Cancel();
    return state_machine_.TransitionTo(state);
}

bool Application::PrepareWifiConfigEntry(WifiConfigEntryPreparation& preparation) {
    preparation = {};
    if (wifi_config_preparation_.valid) {
        PollChatAudioCleanup();
        PollChatProtocolCleanup();
        if (lesson_runtime_active_.load() || reset_pending_.load() ||
            chat_protocol_state_.load(std::memory_order_acquire) != 0 ||
            protocol_work_lifetime_.Pending() || protocol_work_lifetime_.Busy() ||
            chat_audio_state_.load(std::memory_order_acquire) != 0 ||
            chat_audio_completed_revoked_ != wifi_config_audio_revoked_ || chat_audio_fault_) {
            return false;
        }
        preparation = wifi_config_preparation_;
        wifi_config_preparation_ = {};
        ESP_LOGI(TAG, "WiFi config audio and protocol cleanup complete");
        return true;
    }
    const DeviceState state = GetDeviceState();
    if (!WifiConfigEntryPolicy::CanPrepare(state, lesson_runtime_active_.load(),
                                           connect_in_flight_.load(), reset_pending_.load())) {
        ESP_LOGW(TAG, "WiFi config preparation rejected: state=%d connect=%d reset=%d",
                 static_cast<int>(state), connect_in_flight_.load() ? 1 : 0,
                 reset_pending_.load() ? 1 : 0);
        return false;
    }

    preparation.original_state = state;
    preparation.resume_mode = state == kDeviceStateConnecting ? reconnect_mode_ : listening_mode_;
    preparation.resume_realtime = state == kDeviceStateConnecting ||
                                  state == kDeviceStateListening || state == kDeviceStateSpeaking;
    preparation.resume_listening =
        state != kDeviceStateConnecting || reconnect_resume_listening_.load();
    preparation.valid = true;
    CancelChatRecovery();

    ++connect_generation_;
    CancelConnectWatchdog();
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    connect_attempt_active_.store(false);
    passive_ws_intent_.store(false);
    reconnect_passive_.store(false);

    if (chat_cleanup_enabled_) {
        if (chat_protocol_signals_)
            chat_protocol_signals_->Disable();
        tts_audio_accepting_.store(false);
        microphone_uplink_authorized_.store(false);
        speaking_arm_dispatch_.Cancel();
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        wifi_config_audio_revoked_ =
            RequestChatAudioCleanup(speaking_generation_.load(), true, false, false);
        CloseAudioChannelByIntent();
        if (state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring &&
            state != kDeviceStateIdle && !SetDeviceState(kDeviceStateIdle)) {
            preparation.valid = false;
            return false;
        }
        wifi_config_preparation_ = preparation;
        preparation.valid = false;
        return false;
    }

    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (GetDeviceState() == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
    }
    CloseAudioChannelByIntent();
    audio_service_.ResetDecoder();
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);

    const DeviceState settled_state = GetDeviceState();
    if (settled_state != kDeviceStateStarting && settled_state != kDeviceStateWifiConfiguring &&
        settled_state != kDeviceStateIdle) {
        if (!SetDeviceState(kDeviceStateIdle)) {
            ESP_LOGE(TAG, "WiFi config preparation could not settle state=%d",
                     static_cast<int>(settled_state));
            if (!RollbackWifiConfigEntry(preparation)) {
                ESP_LOGE(TAG, "WiFi config preparation rollback failed");
            }
            preparation.valid = false;
            return false;
        }
    }
    return true;
}

bool Application::PublishWifiConfigEntry(const WifiConfigEntryPreparation& preparation) {
    if (!preparation.valid) {
        return false;
    }
    if (!SetDeviceState(kDeviceStateWifiConfiguring)) {
        ESP_LOGE(TAG, "WiFi config state publication rejected from state=%d",
                 static_cast<int>(GetDeviceState()));
        return false;
    }
    return true;
}

bool Application::RollbackWifiConfigEntry(const WifiConfigEntryPreparation& preparation) {
    if (!preparation.valid) {
        return false;
    }
    if (preparation.original_state == kDeviceStateStarting ||
        preparation.original_state == kDeviceStateWifiConfiguring) {
        if (GetDeviceState() != preparation.original_state) {
            return false;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    if (preparation.original_state == kDeviceStateActivating) {
        return SetDeviceState(kDeviceStateActivating);
    }
    if (!preparation.resume_realtime) {
        if (!SetDeviceState(kDeviceStateIdle)) {
            return false;
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    if (!SetDeviceState(kDeviceStateIdle)) {
        return false;
    }
    if (protocol_ == nullptr) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return true;
    }
    reconnect_resume_listening_.store(preparation.resume_listening);
    if (!SetDeviceState(kDeviceStateConnecting)) {
        return false;
    }
    Schedule([this, mode = preparation.resume_mode]() { ContinueOpenAudioChannel(mode); });
    return true;
}

void Application::EnsureBleAdvertisingForStandby() {
    EnsureBleAdvertisingForStandbyImpl(std::nullopt);
}

bool Application::EnsureBleAdvertisingForStandbyForSetupGeneration(
    uint32_t expected_generation, const std::function<void()>& on_current) {
    return EnsureBleAdvertisingForStandbyImpl(expected_generation, on_current);
}

bool Application::EnsureBleAdvertisingForStandbyImpl(std::optional<uint32_t> expected_generation,
                                                     const std::function<void()>& on_current) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();

    if (IsDeviceClaimed()) {
        return expected_generation.has_value()
                   ? StopBleAdvertisingForSetupGeneration(expected_generation.value(), on_current)
                   : StopBleAdvertisingImpl(std::nullopt);
    }

    auto provisioning_token = blufi.CaptureProvisioningSession();
    auto prepare = [&]() -> esp_err_t {
        if (provisioning_token.valid()) {
            return ESP_OK;
        }
        auto provisioning_reservation = blufi.TryReserveProvisioningSession();
        if (!provisioning_reservation) {
            ESP_LOGW(TAG, "Claim standby BLE start deferred: provisioning completion active");
            return ESP_ERR_INVALID_STATE;
        }
        const auto begin_result = audio_service_.BeginWifiProvisioning();
        if (!begin_result) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: audio lifecycle did not quiesce");
            return ESP_FAIL;
        }
        provisioning_token = begin_result.token;
        if (!provisioning_reservation.Commit(provisioning_token)) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: could not bind provisioning token");
            audio_service_.EndWifiProvisioningAndRearm(provisioning_token);
            provisioning_token = {};
            return ESP_FAIL;
        }
        return ESP_OK;
    };
    if (expected_generation.has_value()) {
        const bool ensured = blufi.EnsureAdvertisingForSetupGeneration(
            expected_generation.value(), CONFIG_BLE_SETUP_TIMEOUT_SEC, &provisioning_token, prepare,
            on_current);
        if (!ensured) {
            ESP_LOGE(TAG, "Claim standby BLE ensure failed or became stale");
        }
        return ensured;
    }

    if (blufi.GetBleState() == Blufi::BleState::kOff) {
        ESP_LOGI(TAG, "Claim standby: starting BLE advertising (TBOT-<MAC>)");
        const esp_err_t init_error = prepare() == ESP_OK ? blufi.init() : ESP_FAIL;
        if (init_error != ESP_OK) {
            ESP_LOGE(TAG, "Claim standby BLE start failed: BLUFI init failed");
            blufi.AbortProvisioningSetup(provisioning_token);
            return false;
        }
    }

    blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);
#else
    if (expected_generation.has_value()) {
        (void)expected_generation;
        if (on_current) {
            on_current();
        }
    }
#endif
    return true;
}

void Application::EnsureBleAdvertisingForUnclaimedSavedWifi() {
    if (IsDeviceClaimed()) {
        return;
    }

    ESP_LOGI(TAG,
             "Stored WiFi exists but device is unclaimed; keeping BLE advertising open for setup");
    EnsureBleAdvertisingForStandby();
}

void Application::StopBleAdvertising() { StopBleAdvertisingImpl(std::nullopt); }

bool Application::StopBleAdvertisingForSetupGeneration(uint32_t expected_generation,
                                                       const std::function<void()>& on_current) {
    return StopBleAdvertisingImpl(expected_generation, on_current);
}

bool Application::StopBleAdvertisingImpl(std::optional<uint32_t> expected_generation,
                                         const std::function<void()>& on_current) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    if (expected_generation.has_value()) {
        const bool was_active = blufi.GetBleState() != Blufi::BleState::kOff;
        const esp_err_t result =
            blufi.DeinitForSetupGeneration(expected_generation.value(), on_current);
        if (result != ESP_OK) {
            return false;
        }
        if (was_active) {
            ESP_LOGI(TAG, "Leaving claimable standby: stopping BLE advertising");
        }
        return true;
    }
    blufi.CancelBleSetupTimeout();
    if (blufi.GetBleState() != Blufi::BleState::kOff) {
        ESP_LOGI(TAG, "Leaving claimable standby: stopping BLE advertising");
        return blufi.deinit() == ESP_OK;
    }
#else
    if (expected_generation.has_value()) {
        (void)expected_generation;
        if (on_current) {
            on_current();
        }
    }
#endif
    return true;
}

TbotBleSubstate Application::GetBleSubstate() const {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    switch (Blufi::GetInstance().GetBleState()) {
        case Blufi::BleState::kAdvertising:
        case Blufi::BleState::kConnected:
            return TbotBleSubstate::Advertising;
        case Blufi::BleState::kTimeout:
            return TbotBleSubstate::Timeout;
        case Blufi::BleState::kOff:
        default:
            return TbotBleSubstate::Off;
    }
#else
    return TbotBleSubstate::Off;
#endif
}

void Application::ActivationTask() {
    ota_ = std::make_unique<Ota>();
    SystemInfo::PrintHeapCheckpoint("activation.start");

    ota_->MarkCurrentVersionValid();

#if !CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    if (!IsDeviceClaimed()) {
        ESP_LOGW(TAG,
                 "Unclaimed device: skip OTA/bootstrap HTTPS while BLE stays up "
                 "(claim path remains via BLE; public lesson sync uses raw WS)");
        CheckAssetsVersion();
    } else {
        CheckAssetsVersion();

        SystemInfo::StartHeapPhaseMonitor();
        CheckNewVersion();
        SystemInfo::PrintHeapCheckpoint("ota_check.complete");
        SystemInfo::StopHeapPhaseMonitor();

        SystemInfo::StartHeapPhaseMonitor();
        RefreshWebsocketUrlFromConfigFetch();
        SystemInfo::PrintHeapCheckpoint("config_fetch.complete");
        SystemInfo::StopHeapPhaseMonitor();
    }
#else
    SystemInfo::StartHeapPhaseMonitor();
    CheckNewVersion();
    SystemInfo::PrintHeapCheckpoint("course_mode_local_config.complete");
    SystemInfo::StopHeapPhaseMonitor();
#endif

    RequestInitializeProtocol(ProtocolActivation::kNormal);
}
