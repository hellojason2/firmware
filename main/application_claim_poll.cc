#include "application_internal.h"

// ---------------------------------------------------------------------------
// Bounded claim poll (C4)
// ---------------------------------------------------------------------------

void Application::StartClaimPoll() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    if (online_intent_.load() && IsDeviceClaimed()) {
        return;  // Claimed + online; never re-arm the blocking claim backend poll.
    }
    // Fix 3: once the realtime WS is up (online_intent_) the device is fully
    // functional, so the claim poll is pure background — back it off to 60s so it
    // can never materially starve audio. Offline / mid-confirm keeps the 10s
    // cadence so a phone claim is discovered promptly. We never fully kill the
    // poll for an unclaimed device (must keep discovering it got claimed).
    const uint64_t desired_interval_us =
        online_intent_.load() ? kClaimPollIntervalIdleUs : kClaimPollIntervalUs;
    if (claim_poll_active_) {
        if (desired_interval_us == claim_poll_interval_us_ || claim_poll_timer_ == nullptr) {
            return;  // Already polling this window at the right cadence.
        }
        // Cadence changed (e.g. WS just came up) -> re-arm at the new interval
        // without resetting the 5-minute confirm-window start.
        esp_timer_stop(claim_poll_timer_);
        claim_poll_interval_us_ = desired_interval_us;
        esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_);
        ESP_LOGI(TAG, "Claim poll re-armed (every %lus)",
                 static_cast<unsigned long>(claim_poll_interval_us_ / 1000000ULL));
        return;
    }
    if (claim_poll_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    Application* app = static_cast<Application*>(arg);
                    // Never do network I/O in the timer task — post to Application.
                    app->Schedule([app]() { app->PollPendingTbotClaimTick(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "claim_poll",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &claim_poll_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create claim poll timer");
            claim_poll_timer_ = nullptr;
            return;
        }
    }
    claim_poll_started_ms_ = esp_timer_get_time() / 1000;
    claim_poll_active_ = true;
    claim_poll_interval_us_ = desired_interval_us;
    esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_);
    ESP_LOGI(TAG, "Claim poll started (every %lus, %lds cap)",
             static_cast<unsigned long>(claim_poll_interval_us_ / 1000000ULL),
             static_cast<long>(kClaimPollWindowMs / 1000));
#endif
}

void Application::StopClaimPoll() {
    if (claim_poll_timer_ != nullptr && claim_poll_active_) {
        esp_timer_stop(claim_poll_timer_);
    }
    claim_poll_active_ = false;
}

void Application::PollPendingTbotClaimTick() {
    if (!claim_poll_active_) {
        return;
    }

    // Respect the 5-minute window cap only for an active backend claim-confirm
    // window. Mere unclaimed standby is intentionally long-lived: the phone may
    // scan after the robot has been sitting ready for more than five minutes.
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - claim_poll_started_ms_ >= kClaimPollWindowMs) {
        if (!pending_tbot_claim_.active) {
            ESP_LOGI(TAG, "Claim standby poll window elapsed; continuing BLE advertising");
            claim_poll_started_ms_ = now_ms;
            RefreshPendingTbotClaim();
            return;
        }

        ESP_LOGW(TAG, "Claim confirm window elapsed -> CLAIM_CONFIRM_TIMEOUT");
        StopClaimPoll();
        HandleClaimConfirmTimeout();
        return;
    }

    // Re-fetch /device/config; RefreshPendingTbotClaim() handles the result
    // (promote to WaitingConfirm, stay in standby, or re-arm the poll).
    RefreshPendingTbotClaim();
}

// ---------------------------------------------------------------------------
// Local claim-expiry deadline (C4)
// ---------------------------------------------------------------------------

void Application::ArmClaimExpiryTimer() {
    CancelClaimExpiryTimer();

    time_t expires_epoch = 0;
    if (!ParseIso8601UtcToEpoch(pending_tbot_claim_.expires_at, expires_epoch)) {
        ESP_LOGW(TAG, "Pending claim has no parseable expires_at; relying on poll cap");
        return;
    }

    const time_t now = time(nullptr);
    // M3: arming a wall-clock deadline is only meaningful once the clock is real.
    // Without server_time (or before a 2024-01-01 sanity floor) time() can read
    // ~1970, which would arm the one-shot decades out. Skip arming and lean on
    // the bounded poll's 5-minute window cap, which is monotonic and correct.
    static constexpr time_t kClockSanityFloor = 1704067200;  // 2024-01-01T00:00:00Z
    if (!has_server_time_ && now < kClockSanityFloor) {
        ESP_LOGI(TAG,
                 "Clock unsynced; not arming wall-clock claim expiry, relying on poll-window cap");
        return;
    }
    int64_t remaining_s = static_cast<int64_t>(expires_epoch) - static_cast<int64_t>(now);
    if (remaining_s <= 0) {
        // Already expired -> surface immediately on the Application task.
        Schedule([this]() { HandleClaimConfirmTimeout(); });
        return;
    }

    esp_timer_create_args_t args = {
        .callback =
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->Schedule([app]() { app->HandleClaimConfirmTimeout(); });
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "claim_expiry",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &claim_expiry_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create claim expiry timer");
        claim_expiry_timer_ = nullptr;
        return;
    }
    esp_timer_start_once(claim_expiry_timer_, static_cast<uint64_t>(remaining_s) * 1000000ULL);
    ESP_LOGI(TAG, "Claim expiry armed in %lds", static_cast<long>(remaining_s));
}

void Application::CancelClaimExpiryTimer() {
    if (claim_expiry_timer_ != nullptr) {
        esp_timer_stop(claim_expiry_timer_);
        esp_timer_delete(claim_expiry_timer_);
        claim_expiry_timer_ = nullptr;
    }
}

void Application::HandleClaimConfirmTimeout() {
    CancelClaimExpiryTimer();
    StopClaimPoll();
    // Leaving claimable standby (window elapsed) -> stop advertising for pairing.
    StopBleAdvertising();
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
    claim_substate_ = TbotClaimSubstate::ConfirmTimeout;
    // "Setup expired" (CLAIM_CONFIRM_TIMEOUT) — no silent failure, no spinner.
    Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SETUP_EXPIRED, "triangle_exclamation",
          Lang::Sounds::OGG_EXCLAMATION);
}

bool Application::HasStaleRevokedClaimIdentity() const {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return false;
#else
    Settings claim_state("tbot_claim", false);
    Settings backend_settings("backend", false);
    const bool claim_confirmed = claim_state.GetInt("confirmed", 0) != 0;
    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    return !claim_confirmed && !device_id.empty() && device_secret.empty();
#endif
}

bool Application::IsDeviceClaimed() const {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return true;
#else
    // Primary claimed-signal: a DEDICATED flag written ONLY by a successful
    // physical-claim confirm (PersistTbotClaimConfirmationResponse). Recovery
    // signal: backend device_id + device_secret are also written only by that
    // successful confirm, so after a reboot/marker loss they are enough to keep
    // the robot on the claimed online path. We must NOT key off websocket
    // "token": OTA CheckVersion can write that realtime-WS token on every boot.
    Settings backend_settings("backend", false);
    if (backend_settings.GetInt("release_pending", 0) != 0) {
        // Credentials are retained only so the deferred ownership release can
        // authenticate. They must not start claimed runtime/audio while the
        // robot is rebooting into Wi-Fi provisioning.
        return false;
    }

    Settings claim_state("tbot_claim", false);
    const bool claim_confirmed = claim_state.GetInt("confirmed", 0) != 0;

    Settings websocket_settings("websocket", false);
    const std::string websocket_token = websocket_settings.GetString("token");
    const bool factory_test_claimed = claim_state.GetInt("factory_test", 0) != 0;
    if (factory_test_claimed && !websocket_token.empty()) {
        return true;
    }

    const std::string device_id = backend_settings.GetString("device_id");
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (claim_confirmed && (device_id.empty() || device_secret.empty())) {
        ESP_LOGW(TAG, "Ignoring stale claim marker without complete backend credentials");
    }
    return !device_id.empty() && !device_secret.empty();
#endif
}
