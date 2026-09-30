#include "application_internal.h"

void Application::SchedulePendingTbotClaimRefresh(uint32_t expected_setup_generation) {
    Schedule([this, expected_setup_generation]() {
        ClaimDeferredEffects deferred_effects;
        const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
            expected_setup_generation, [this, &deferred_effects]() {
                // Serialize setup promotion and snapshot/dispatch with BOOT re-entry.
                PromoteFromWifiConfigAfterProvisioning();
                if (GetDeviceState() != kDeviceStateWifiConfiguring) {
                    deferred_effects.dispatch_refresh = true;
                }
                // If activation remains in progress, HandleActivationDoneEvent
                // performs the normal refresh after reaching Idle.
            });
        if (applied) {
            ExecuteClaimDeferredEffects(deferred_effects, expected_setup_generation);
        }
    });
}

void Application::CompleteCardputerWifiProvisioning(uint64_t ui_generation) {
    uint64_t completed = cardputer_wifi_completion_generation_.load(std::memory_order_acquire);
    if (ui_generation <= completed || GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;
    }
    PromoteFromWifiConfigAfterProvisioning();
    if (GetDeviceState() == kDeviceStateWifiConfiguring) {
        return;
    }
    while (ui_generation > completed &&
           !cardputer_wifi_completion_generation_.compare_exchange_weak(
               completed, ui_generation, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    HandleNetworkConnectedEvent();
}

void Application::ExecuteClaimDeferredEffects(
    const ClaimDeferredEffects& effects, uint32_t expected_setup_generation,
    WakeWordLifecycleController::ProvisioningToken provisioning_token) {
    auto commit_dispatch = [this, &effects, expected_setup_generation]() {
        if (effects.dispatch_confirmation &&
            !DispatchPendingTbotClaimConfirmation(expected_setup_generation, true)) {
            StartClaimPoll();
        }
        if (effects.restore_standby_after_dispatch_failure) {
            claim_substate_ = TbotClaimSubstate::AvailableStandby;
            RenderClaimSubstate(claim_substate_);
            StartClaimPoll();
        }
    };
    bool lifecycle_ready = true;
    switch (effects.ble_intent) {
        case ClaimBleLifecycleIntent::kNone:
            lifecycle_ready =
                RunClaimDispatchForSetupGeneration(expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kEnsureAdvertising:
            lifecycle_ready = EnsureBleAdvertisingForStandbyForSetupGeneration(
                expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kStopAdvertising:
            lifecycle_ready =
                StopBleAdvertisingForSetupGeneration(expected_setup_generation, commit_dispatch);
            break;
        case ClaimBleLifecycleIntent::kCompleteSuccessfulTeardown:
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            lifecycle_ready =
                Blufi::GetInstance().CompleteSuccessfulProvisioningTeardownForGeneration(
                    "claim_confirmed", provisioning_token, expected_setup_generation,
                    commit_dispatch);
#else
            lifecycle_ready =
                StopBleAdvertisingForSetupGeneration(expected_setup_generation, commit_dispatch);
#endif
            break;
    }

    if (!lifecycle_ready) {
        return;
    }
    if (effects.dispatch_refresh) {
        DispatchPendingTbotClaimRefreshForSetupGeneration(expected_setup_generation);
    }
}

bool Application::RunClaimDispatchForSetupGeneration(uint32_t expected_setup_generation,
                                                     const std::function<void()>& action) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    return Blufi::GetInstance().RunWithSetupGenerationCurrent(expected_setup_generation, action);
#else
    (void)expected_setup_generation;
    action();
    return true;
#endif
}

void Application::DispatchPendingTbotClaimRefreshForSetupGeneration(
    uint32_t expected_setup_generation) {
    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    Settings websocket_settings("websocket", false);
    std::string token = websocket_settings.GetString("bootstrap_token");
    SecureStringScope token_scope(token);

    if (pending_tbot_claim_.active && !token.empty()) {
        ClaimDeferredEffects effects;
        const bool applied =
            Blufi::GetInstance().RunIfSetupGenerationCurrent(expected_setup_generation, [&]() {
                if (!api_url.empty()) {
                    pending_tbot_claim_api_url_ = api_url;
                }
                SecureClearString(pending_tbot_claim_token_);
                pending_tbot_claim_token_ = token;
                claim_substate_ = TbotClaimSubstate::WaitingConfirm;
                effects.ble_intent = ClaimBleLifecycleIntent::kStopAdvertising;
                effects.dispatch_confirmation = true;
            });
        if (applied) {
            ExecuteClaimDeferredEffects(effects, expected_setup_generation);
        }
        return;
    }

    bool dispatched = false;
    const bool current = RunClaimDispatchForSetupGeneration(expected_setup_generation, [&]() {
        dispatched =
            DispatchPendingTbotClaimFetch(api_url, token, true, expected_setup_generation, true);
    });
    if (!current) {
        return;
    }
    if (dispatched) {
        if (!token.empty() && passive_ws_intent_.load()) {
            ESP_LOGI(TAG, "Provisioning claim preempting passive lesson WebSocket");
            CloseAudioChannelByIntent();
        }
    }
    if (!dispatched) {
        ClaimDeferredEffects effects;
        effects.ble_intent = ClaimBleLifecycleIntent::kEnsureAdvertising;
        effects.restore_standby_after_dispatch_failure = true;
        ExecuteClaimDeferredEffects(effects, expected_setup_generation);
    }
}

void Application::PromoteFromWifiConfigAfterProvisioning() {
    // BluFi reported STA-connected success. Unlike a stale STA event, this is a
    // real provisioning completion, so leave WiFi-config mode and run the normal
    // activation->Idle path. RefreshPendingTbotClaim (which auto-confirms the
    // pending claim) only runs once we are OUT of kDeviceStateWifiConfiguring.
    if (GetDeviceState() != kDeviceStateWifiConfiguring) {
        return;  // Already promoted / not in setup -> let the normal path run.
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        return;  // Success report was stale; stay in setup.
    }
    if (!SetDeviceState(kDeviceStateActivating)) {
        return;  // FSM rejected the transition; nothing more to do here.
    }
    if (!IsDeviceClaimed()) {
        // Same heap constraint as HandleNetworkConnectedEvent: with BLE still
        // advertising for claim standby, the 8KB activation task often cannot be
        // created. Run only the protocol setup needed for public lesson sync.
        ESP_LOGW(TAG,
                 "Unclaimed after BluFi Wi-Fi success: run minimal activation "
                 "transport inline");
        CompleteUnclaimedProtocolOnlyActivation();
        return;
    }

    ESP_LOGI(TAG, "Claimed after BluFi Wi-Fi success: run lightweight activation");
    CompleteClaimedWifiReprovisionActivation();
}

void Application::PromoteCourseModeFromWifiConfigAfterProvisioning() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    PromoteFromWifiConfigAfterProvisioning();
#else
    return;
#endif
}

void Application::CompleteUnclaimedProtocolOnlyActivation() {
    if (!ota_) {
        ota_ = std::make_unique<Ota>();
    }
    ota_->MarkCurrentVersionValid();

    RequestInitializeProtocol(ProtocolActivation::kNormal);
}

void Application::CompleteClaimedWifiReprovisionActivation() {
    if (!ota_) {
        ota_ = std::make_unique<Ota>();
    }
    ota_->MarkCurrentVersionValid();

    // Normal Wi-Fi changes reuse the realtime token from the previous online
    // session. Recovery pairing can legitimately arrive here without one (for
    // example after an operator releases stale cloud ownership), so refresh the
    // signed runtime config before opening the WebSocket.
    Settings websocket_settings("websocket", false);
    if (websocket_settings.GetString("token").empty()) {
        const esp_err_t refresh_result = ota_->CheckVersion();
        if (refresh_result != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket config refresh after WiFi provisioning failed: 0x%x",
                     refresh_result);
        }
    }

    RequestInitializeProtocol(ProtocolActivation::kWifiReprovision);
}

bool Application::EnsureLocalAssetsAppliedForClaim() {
    auto& assets = Assets::GetInstance();
    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return true;
    }

    return assets.Apply(false);
}

bool Application::FinishClaimActivationAfterLocalAssetsReady() {
    if (!IsDeviceClaimed()) {
        return false;
    }
    if (!EnsureLocalAssetsAppliedForClaim()) {
        ESP_LOGE(TAG, "Claim confirmed but local assets/models are not ready; retrying locally");
        return false;
    }
    if (claim_assets_retry_timer_ != nullptr) {
        esp_timer_stop(claim_assets_retry_timer_);
    }

    claim_protocol_completion_pending_ = true;
    ReloadProtocolAfterClaimCredentials();
    return true;
}

void Application::CompleteClaimProtocolActivation() {
    // TBOT claim complete -> refresh the protocol with claimed credentials, then
    // return to explicit wake standby. InitializeProtocol opens only the passive
    // lesson/nudge WebSocket for claimed idle devices.
    if (!audio_service_.Start()) {
        ESP_LOGE(TAG, "Claim activation audio startup failed; retry remains pending");
        ScheduleClaimLocalAssetsRetry();
        return;
    }
    SetDeviceState(kDeviceStateIdle);
    if (!lesson_asset_sync_quiet_.load()) {
        audio_service_.EnableWakeWordDetection(true);
    }
    StartHeartbeat();
    DispatchDeviceHeartbeat();
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTED, "link", Lang::Sounds::OGG_SUCCESS);
}

void Application::ScheduleClaimLocalAssetsRetry() {
    if (claim_assets_retry_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() { self->HandleClaimLocalAssetsRetry(); });
        };
        args.arg = this;
        args.name = "claim_assets_retry";
        if (esp_timer_create(&args, &claim_assets_retry_timer_) != ESP_OK) {
            claim_assets_retry_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(claim_assets_retry_timer_);
    esp_timer_start_once(claim_assets_retry_timer_, 2000ULL * 1000ULL);
}

void Application::HandleClaimLocalAssetsRetry() {
    if (!IsDeviceClaimed()) {
        return;
    }
    if (FinishClaimActivationAfterLocalAssetsReady()) {
        return;
    }
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING,
          "triangle_exclamation", "");
    ScheduleClaimLocalAssetsRetry();
}

void Application::ReloadProtocolAfterClaimCredentials() {
    CloseAudioChannelByIntent();
    RequestInitializeProtocol();
}

void Application::RenderClaimSubstate(TbotClaimSubstate substate) {
    // The connect-state contract row is the source of truth for which state we
    // are in; the on-screen copy is the localized Lang::Strings equivalent so
    // the VI build shows translated text (English literals live in the table).
    const TbotConnectState state =
        TbotConnectMapper::ResolveState(GetDeviceState(), substate, GetBleSubstate());
    const char* copy = TbotConnectMapper::ScreenTextFor(state);
    switch (state) {
        case TbotConnectState::CLAIM_AVAILABLE:
            copy = Lang::Strings::READY_TO_CONNECT;
            break;
        case TbotConnectState::CLAIM_WAITING_CONFIRM:
            copy = Lang::Strings::PRESS_BUTTON_TO_CONFIRM;
            break;
        case TbotConnectState::CLAIM_CONFIRM_TIMEOUT:
            copy = Lang::Strings::SETUP_EXPIRED;
            break;
        default:
            break;  // fall back to the contract text
    }
    Alert(Lang::Strings::TBOT_CONNECT, copy, "link", "");
}

// ---------------------------------------------------------------------------
// Off-task claim-config fetch ("Hi ESP needs many tries" fix)
// ---------------------------------------------------------------------------

namespace {
// Heap-owned hand-off for the off-task claim fetch worker (mirrors ConnectContext).
struct ClaimFetchContext {
    Application* app;
    std::string api_url;
    std::string token;
    bool apply_when_poll_inactive;
    uint32_t expected_setup_generation;
    bool enforce_setup_generation;
};
}  // namespace

bool Application::DispatchPendingTbotClaimFetch(const std::string& api_url,
                                                const std::string& token,
                                                bool apply_when_poll_inactive,
                                                uint32_t expected_setup_generation,
                                                bool enforce_setup_generation) {
    // Runs on the Application task. Belt-and-suspenders gating (fix 2): never
    // kick off a blocking TLS handshake while live realtime audio is in flight —
    // a wake/connect/listen/speak must always win the radio + CPU. On skip we do
    // nothing and let the next periodic tick retry; we do NOT StopClaimPoll or
    // reset the window, so an unclaimed device keeps discovering a phone claim and
    // the 5-minute confirm cap (PollPendingTbotClaimTick) stays intact.
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || connect_in_flight_.load()) {
        ESP_LOGD(TAG, "Skipping claim fetch this tick (runtime audio active)");
        return false;
    }

    // Single-flight: a slow backend must never let the 10s timer stack workers.
    bool expected = false;
    if (!claim_poll_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Claim fetch already in flight; skipping this tick");
        return false;
    }

    auto* ctx = new (std::nothrow) ClaimFetchContext{this,
                                                     api_url,
                                                     token,
                                                     apply_when_poll_inactive,
                                                     expected_setup_generation,
                                                     enforce_setup_generation};
    if (ctx == nullptr) {
        ESP_LOGE(TAG, "claim_fetch context allocation failed; restoring standby");
        claim_poll_inflight_.store(false);
        return false;
    }
    // Low priority (tskIDLE_PRIORITY+1) and NOT pinned to core 0 so the worker
    // simply WAITS on the network at low priority while the wake-word AFE
    // fetch/feed pipeline keeps the CPU. The old design queued this blocking call
    // onto the priority-10 Application task, which is the starvation root cause.
    //
    // Stack MUST be internal DRAM — not SPIRAM. The worker opens NVS + does
    // TLS/HTTP; both disable the flash cache. A SPIRAM task stack is invalid
    // while the cache is off and panics with:
    //   esp_task_stack_is_sane_cache_disabled (spi_flash cache_utils).
    // Live crash after BluFi Wi-Fi success was exactly this path.
    if (xTaskCreateWithCaps(&Application::ClaimFetchTask, "claim_fetch", 6144, ctx,
                            tskIDLE_PRIORITY + 1, nullptr,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "claim_fetch task create failed; retrying next tick");
        SecureClearString(ctx->token);
        delete ctx;
        claim_poll_inflight_.store(false);
        return false;
    }
    return true;
}

void Application::ClaimFetchTask(void* arg) {
    {
        auto* ctx = static_cast<ClaimFetchContext*>(arg);
        Application* self = ctx->app;
        std::string api_url = ctx->api_url;
        std::string token = ctx->token;
        const bool apply_when_poll_inactive = ctx->apply_when_poll_inactive;
        const uint32_t expected_setup_generation = ctx->expected_setup_generation;
        const bool enforce_setup_generation = ctx->enforce_setup_generation;
        SecureClearString(ctx->token);
        delete ctx;

        // The ONLY work on this worker: the blocking ~3s HTTP/TLS fetch. No shared
        // state is touched here.
        if (api_url.empty()) {
            api_url = FetchBackendApiUrlFromBootstrap(token, false);
        }
        PendingTbotClaim pending_claim;
        int device_config_status = 0;
        const bool fetched =
            !api_url.empty() && FetchPendingTbotClaimFromDeviceConfig(api_url, token, pending_claim,
                                                                      &device_config_status);

        // Marshal result-application back onto the Application task (OQ1): all
        // claim_substate_/pending_tbot_claim_*/BLE/SetDeviceState mutation stays on
        // the one task that owns them. Clear the single-flight guard there so the
        // next tick can dispatch again.
        self->Schedule([self, api_url, token, pending_claim, fetched, device_config_status,
                        apply_when_poll_inactive, expected_setup_generation,
                        enforce_setup_generation]() mutable {
            self->claim_poll_inflight_.store(false);
            if (!self->claim_poll_active_ && token.empty() && !apply_when_poll_inactive) {
                SecureClearString(token);
                return;
            }
            ClaimDeferredEffects deferred_effects;
            auto apply_result = [&]() {
                self->ApplyPendingTbotClaimFetchResult(
                    api_url, token, pending_claim, fetched, device_config_status,
                    enforce_setup_generation, expected_setup_generation, &deferred_effects);
            };
            if (enforce_setup_generation) {
                const bool applied = Blufi::GetInstance().RunIfSetupGenerationCurrent(
                    expected_setup_generation, apply_result);
                if (applied) {
                    self->ExecuteClaimDeferredEffects(deferred_effects, expected_setup_generation);
                }
            } else {
                self->ApplyPendingTbotClaimFetchResult(api_url, token, pending_claim, fetched,
                                                       device_config_status, false,
                                                       expected_setup_generation, nullptr);
            }
            SecureClearString(token);
        });
        SecureClearString(token);
    }
    vTaskDeleteWithCaps(nullptr);
}
