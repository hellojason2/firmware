#include "application_internal.h"

void Application::RefreshPendingTbotClaim() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#endif
    MaybeDispatchDeferredCloudRelease();

    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        StopClaimPoll();
        claim_fetch_failures_ = 0;
        return;
    }

    if (IsDeviceClaimed()) {
        StopClaimPoll();
        StopBleAdvertising();
        return;
    }
    CancelClaimExpiryTimer();

    Settings backend_settings("backend", false);
    std::string api_url = backend_settings.GetString("api_url");

    Settings websocket_settings("websocket", false);
    std::string token = websocket_settings.GetString("bootstrap_token");
    SecureStringScope token_scope(token);
    bool paused_ble_for_fetch = false;

    if (!token.empty() && websocket_settings.GetInt("claim_ambiguous", 0) != 0) {
        claim_confirmation_ambiguous_ = true;
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StopClaimPoll();
        StopBleAdvertising();
        ESP_LOGE(TAG, "Claim confirmation remains ambiguous; automatic backend retry suppressed");
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return;
    }

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    {
        const auto ble_state = Blufi::GetInstance().GetBleState();
        if (ble_state == Blufi::BleState::kConnected) {
            ESP_LOGI(TAG, "BLE connected; waiting for provisioning handoff to finish");
            return;
        }
        if (!pending_tbot_claim_.active && ble_state == Blufi::BleState::kAdvertising && token.empty()) {
            ESP_LOGI(TAG, "Pausing BLE advertising before no-token claim config fetch");
            Blufi::GetInstance().CancelBleSetupTimeout();
            StopBleAdvertising();
            paused_ble_for_fetch = true;
        } else if (!pending_tbot_claim_.active && !token.empty() &&
                   ble_state == Blufi::BleState::kAdvertising) {
            ESP_LOGW(TAG,
                     "Bootstrap token present but BLE still active; stopping BLE to proceed with "
                     "claim fetch/confirm");
            Blufi::GetInstance().CancelBleSetupTimeout();
            StopBleAdvertising();
            paused_ble_for_fetch = true;
        }
    }
#endif

    if (api_url.empty()) {
        api_url = FetchBackendApiUrlFromBootstrap(token);
    }

    if (api_url.empty()) {
        ESP_LOGW(TAG, "No backend api_url (OTA and bootstrap fallback both empty); "
                 "claim/heartbeat feature is inert");
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_NOT_FOUND, "triangle_exclamation",
              "");
        StopClaimPoll();
        StopBleAdvertising();
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::None;
        return;
    }

    if (pending_tbot_claim_.active && !token.empty()) {
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        pending_tbot_claim_token_ = token;
        StopBleAdvertising();
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        ESP_LOGI(TAG, "Cached pending claim has BLE bootstrap token -> auto-confirming");
        ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);
        return;
    }

    if (pending_tbot_claim_.active && token.empty()) {
        ESP_LOGW(TAG,
                 "Pending claim cached but no BLE bootstrap token yet; keeping BLE advertising");
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StopClaimPoll();
        return;
    }

    if (!token.empty() && passive_ws_intent_.load()) {
        ESP_LOGI(TAG, "Bootstrap claim preempting passive lesson WebSocket");
        CloseAudioChannelByIntent();
    }
    const bool claim_fetch_dispatched =
        DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);
    if (paused_ble_for_fetch && !claim_fetch_dispatched) {
        ESP_LOGW(TAG, "Claim fetch was not dispatched; restoring BLE claim standby");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
    }
}

void Application::ApplyPendingTbotClaimFetchResult(
    const std::string& api_url, const std::string& token, const PendingTbotClaim& pending_claim,
    bool fetched, int device_config_status, bool defer_confirmation,
    uint32_t expected_setup_generation, ClaimDeferredEffects* deferred_effects) {
    auto request_ble = [this, deferred_effects](ClaimBleLifecycleIntent intent) {
        if (deferred_effects != nullptr) {
            deferred_effects->ble_intent = intent;
        } else if (intent == ClaimBleLifecycleIntent::kEnsureAdvertising) {
            EnsureBleAdvertisingForStandby();
        } else if (intent == ClaimBleLifecycleIntent::kStopAdvertising) {
            StopBleAdvertising();
        }
    };
    if (!api_url.empty()) {
        Settings backend_settings("backend", true);
        if (backend_settings.GetString("api_url") != api_url) {
            backend_settings.SetString("api_url", api_url);
        }
    }
    if (claim_confirmation_ambiguous_) {
        ESP_LOGW(TAG, "Ignoring claim fetch result while confirmation outcome is ambiguous");
        StopClaimPoll();
        return;
    }
    if (!fetched && !token.empty() &&
        (device_config_status == 401 || device_config_status == 403)) {
        ESP_LOGW(TAG,
                 "Device config rejected bootstrap token (HTTP %d); clearing stale claim token",
                 device_config_status);
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        Blufi::GetInstance().ClearProvisioningSecrets();
#endif
        websocket_settings.EraseKey("claim_device_id");
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_fetch_failures_ = 0;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
        StopClaimPoll();
        return;
    }
    if (!fetched || !pending_claim.active) {
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_ = api_url;
        SecureClearString(pending_tbot_claim_token_);
        pending_tbot_claim_token_ = token;

        const bool had_claim_fetch_failures = claim_fetch_failures_ > 0;
        if (!fetched) {
            ++claim_fetch_failures_;
        } else {
            claim_fetch_failures_ = 0;
        }
        static constexpr int kClaimFetchFailureCopyThreshold = 2;

        if (!fetched && claim_fetch_failures_ == kClaimFetchFailureCopyThreshold) {
            if (claim_substate_ != TbotClaimSubstate::WaitingConfirm) {
                claim_substate_ = TbotClaimSubstate::AvailableStandby;
                Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
                      "triangle_exclamation", "");
            }
        } else if (claim_substate_ != TbotClaimSubstate::WaitingConfirm &&
                   (claim_substate_ != TbotClaimSubstate::AvailableStandby ||
                    had_claim_fetch_failures)) {
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
        }
        if (token.empty()) {
            request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
            StopClaimPoll();
            return;
        }
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms - claim_poll_started_ms_ >= kClaimVisibilityRetryWindowMs) {
            ESP_LOGW(TAG,
                     "Claim visibility retry window elapsed; clearing stale token and restoring "
                     "BLE standby");
            Settings websocket_settings("websocket", true);
            websocket_settings.SetString("bootstrap_token", "");
            websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            Blufi::GetInstance().ClearProvisioningSecrets();
#endif
            websocket_settings.EraseKey("claim_device_id");
            SecureClearString(pending_tbot_claim_token_);
            pending_tbot_claim_ = PendingTbotClaim{};
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
            StopClaimPoll();
            request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
            return;
        }
        if (claim_fetch_failures_ >= 4 && IsDeviceClaimed()) {
            ESP_LOGW(TAG, "claim_poll_giveup failures=%d (claimed; stop hammering backend)",
                     claim_fetch_failures_);
            StopClaimPoll();
        } else {
            StartClaimPoll();
        }
        return;
    }

    // Fetch succeeded with an active claim -> reset the failure streak.
    claim_fetch_failures_ = 0;
    pending_tbot_claim_ = pending_claim;
    pending_tbot_claim_api_url_ = api_url;
    SecureClearString(pending_tbot_claim_token_);
    pending_tbot_claim_token_ = token;

    if (token.empty()) {
        ESP_LOGW(TAG,
                 "Pending claim detected but no BLE bootstrap token yet; keeping BLE advertising");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        request_ble(ClaimBleLifecycleIntent::kEnsureAdvertising);
        StartClaimPoll();
        return;
    }

    StopClaimPoll();
    request_ble(ClaimBleLifecycleIntent::kStopAdvertising);
    claim_substate_ = TbotClaimSubstate::WaitingConfirm;

    ESP_LOGI(
        TAG,
        "Pending claim detected -> auto-confirming (press-to-allow skipped by product decision)");
    if (defer_confirmation) {
        if (deferred_effects != nullptr) {
            deferred_effects->dispatch_confirmation = true;
        } else if (!DispatchPendingTbotClaimConfirmation(expected_setup_generation, true)) {
            StartClaimPoll();
        }
    } else {
        ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);
    }
}

bool Application::ConfirmPendingTbotClaim(bool trust_backend_expiry) {
    if (!pending_tbot_claim_.active) {
        return false;
    }

    if (claim_confirmation_ambiguous_) {
        ESP_LOGW(TAG, "Claim confirmation outcome ambiguous; automatic retry suppressed");
        StopClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    if (pending_tbot_claim_token_.empty()) {
        ESP_LOGW(TAG, "Claim confirm deferred: missing BLE bootstrap token");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        EnsureBleAdvertisingForStandby();
        StartClaimPoll();
        return true;
    }

    if (!trust_backend_expiry && IsPendingTbotClaimExpired(pending_tbot_claim_, time(nullptr))) {
        ESP_LOGW(TAG, "Claim confirm ignored: window expired");
        HandleClaimConfirmTimeout();
        return true;
    }

    WakeWordLifecycleController::ProvisioningToken provisioning_token{};
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    provisioning_token = Blufi::GetInstance().CaptureProvisioningSession();
#endif
    const ClaimConfirmationResult confirmation_result = ClaimConfirmationReporter::Confirm(
        pending_tbot_claim_, pending_tbot_claim_api_url_, pending_tbot_claim_token_);
    return ApplyPendingTbotClaimConfirmationResult(confirmation_result, provisioning_token);
}

bool Application::ApplyPendingTbotClaimConfirmationResult(
    ClaimConfirmationResult confirmation_result,
    WakeWordLifecycleController::ProvisioningToken provisioning_token,
    bool defer_successful_teardown, ClaimDeferredEffects* deferred_effects) {
    if (confirmation_result == ClaimConfirmationResult::RetryableFailure) {
        ESP_LOGW(TAG, "Claim confirmation retryable; retaining claim token for bounded retry");
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StartClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
              "triangle_exclamation", "");
        return true;
    }

    if (confirmation_result == ClaimConfirmationResult::AmbiguousSuccess) {
        ESP_LOGE(TAG,
                 "Claim confirmation 2xx response unusable; freezing attempt for support/reset");
        Settings websocket_settings("websocket", true);
        websocket_settings.SetInt("claim_ambiguous", 1);
        claim_confirmation_ambiguous_ = true;
        claim_substate_ = TbotClaimSubstate::WaitingConfirm;
        StopClaimPoll();
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    if (confirmation_result == ClaimConfirmationResult::TerminalFailure) {
        CancelClaimExpiryTimer();
        StopClaimPoll();
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        // The Application owns the terminal NVS clear; BluFi owns zeroization of
        // the in-RAM token/code copies received from the phone.
        Blufi::GetInstance().ClearProvisioningSecrets();
#endif
        websocket_settings.EraseKey("claim_device_id");
        pending_tbot_claim_ = PendingTbotClaim{};
        pending_tbot_claim_api_url_.clear();
        SecureClearString(pending_tbot_claim_token_);
        claim_confirmation_ambiguous_ = false;
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        if (deferred_effects != nullptr) {
            deferred_effects->ble_intent = ClaimBleLifecycleIntent::kEnsureAdvertising;
        } else {
            EnsureBleAdvertisingForStandby();
        }
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTION_CONFIRM_FAILED,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        return true;
    }

    CancelClaimExpiryTimer();
    StopClaimPoll();
    // Claim confirmed -> the device is becoming claimed. Stop advertising for
    // pairing; an owned robot must not be BLE-discoverable for a new claim.
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    if (defer_successful_teardown && deferred_effects != nullptr) {
        deferred_effects->ble_intent = ClaimBleLifecycleIntent::kCompleteSuccessfulTeardown;
    } else if (!defer_successful_teardown) {
        Blufi::GetInstance().CompleteSuccessfulProvisioningTeardown("claim_confirmed", provisioning_token);
    }
#else
    if (defer_successful_teardown && deferred_effects != nullptr) {
        deferred_effects->ble_intent = ClaimBleLifecycleIntent::kStopAdvertising;
    } else if (!defer_successful_teardown) {
        StopBleAdvertising();
    }
#endif
    if (protocol_) {
        CloseAudioChannelByIntent();
    }
    // The bootstrap token is single-attempt claim auth. Device credentials are
    // already persisted by the reporter; clear the consumed token so a reboot
    // never polls /device/config with stale Authorization before WS comes up.
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.SetString("bootstrap_token", "");
        websocket_settings.SetInt("claim_ambiguous", 0);
    }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    Blufi::GetInstance().ClearProvisioningSecrets();
#endif
    {
        Settings websocket_settings("websocket", true);
        websocket_settings.EraseKey("claim_device_id");
    }
    pending_tbot_claim_ = PendingTbotClaim{};
    pending_tbot_claim_api_url_.clear();
    SecureClearString(pending_tbot_claim_token_);
    claim_confirmation_ambiguous_ = false;
    claim_substate_ = TbotClaimSubstate::Confirmed;

    if (!FinishClaimActivationAfterLocalAssetsReady()) {
        Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
              "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
        ScheduleClaimLocalAssetsRetry();
        return true;
    }

    return true;
}

namespace {
struct ClaimConfirmationContext {
    Application* app;
    PendingTbotClaim claim;
    std::string api_url;
    std::string token;
    WakeWordLifecycleController::ProvisioningToken provisioning_token;
    uint32_t expected_setup_generation;
    bool enforce_setup_generation;
};
}  // namespace

bool Application::DispatchPendingTbotClaimConfirmation(uint32_t expected_setup_generation,
                                                       bool enforce_setup_generation) {
    if (!pending_tbot_claim_.active || pending_tbot_claim_token_.empty()) {
        return false;
    }
    bool expected = false;
    if (!claim_confirm_inflight_.compare_exchange_strong(expected, true)) {
        return false;
    }
    WakeWordLifecycleController::ProvisioningToken provisioning_token{};
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    provisioning_token = Blufi::GetInstance().CaptureProvisioningSession();
#endif
    auto* ctx = new (std::nothrow) ClaimConfirmationContext{this,
                                                            pending_tbot_claim_,
                                                            pending_tbot_claim_api_url_,
                                                            pending_tbot_claim_token_,
                                                            provisioning_token,
                                                            expected_setup_generation,
                                                            enforce_setup_generation};
    if (ctx == nullptr) {
        claim_confirm_inflight_.store(false);
        return false;
    }
    if (xTaskCreateWithCaps(&Application::ClaimConfirmationTask, "claim_confirm", 8192, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        SecureClearString(ctx->token);
        delete ctx;
        claim_confirm_inflight_.store(false);
        return false;
    }
    return true;
}

void Application::ClaimConfirmationTask(void* arg) {
    {
        auto* ctx = static_cast<ClaimConfirmationContext*>(arg);
        Application* self = ctx->app;
        PendingTbotClaim claim = ctx->claim;
        std::string api_url = std::move(ctx->api_url);
        std::string token = std::move(ctx->token);
        const auto provisioning_token = ctx->provisioning_token;
        const uint32_t expected_setup_generation = ctx->expected_setup_generation;
        const bool enforce_setup_generation = ctx->enforce_setup_generation;
        delete ctx;

        std::string success_response;
        const ClaimConfirmationResult result =
            ClaimConfirmationReporter::Confirm(claim, api_url, token, &success_response);
        self->Schedule([self, result, token = std::move(token), provisioning_token,
                        success_response = std::move(success_response), expected_setup_generation,
                        enforce_setup_generation]() mutable {
            self->claim_confirm_inflight_.store(false);
            ClaimDeferredEffects deferred_effects;
            auto apply_result = [&](bool defer_successful_teardown, ClaimDeferredEffects* effects) {
                ClaimConfirmationResult effective_result = result;
                if (effective_result == ClaimConfirmationResult::Confirmed &&
                    !PersistTbotClaimConfirmationResponse(success_response)) {
                    effective_result = ClaimConfirmationResult::AmbiguousSuccess;
                }
                self->ApplyPendingTbotClaimConfirmationResult(effective_result, provisioning_token, defer_successful_teardown, effects);
                return effective_result == ClaimConfirmationResult::Confirmed;
            };
            if (enforce_setup_generation) {
                const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
                    expected_setup_generation, [&]() { apply_result(true, &deferred_effects); });
                if (applied) {
                    self->ExecuteClaimDeferredEffects(deferred_effects, expected_setup_generation,
                                                      provisioning_token);
                }
            } else {
                apply_result(false, nullptr);
            }
            SecureClearString(token);
            SecureClearString(success_response);
        });
        SecureClearString(token);
        SecureClearString(success_response);
    }
    vTaskDeleteWithCaps(nullptr);
}
