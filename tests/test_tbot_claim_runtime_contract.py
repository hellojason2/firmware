from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace:index]
    raise AssertionError(f"unterminated function {signature}")


def test_application_polls_pending_claim_after_activation():
    header = read("main/application.h")
    source = read("main/application.cc")
    activation_done = function_body(source, "void Application::HandleActivationDoneEvent")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert '#include "claim_confirmation_reporter.h"' in header
    assert "PendingTbotClaim pending_tbot_claim_" in header
    assert "std::string pending_tbot_claim_api_url_" in header
    assert "std::string pending_tbot_claim_token_" in header
    assert "RefreshPendingTbotClaim();" in activation_done
    assert 'Settings backend_settings("backend", false);' in refresh_body
    assert 'backend_settings.GetString("api_url")' in refresh_body
    assert "DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);" in refresh_body
    assert "FetchPendingTbotClaimFromDeviceConfig" in function_body(
        source, "void Application::ClaimFetchTask"
    )


def test_activation_done_does_not_leave_wifi_config_mode_after_boot_reprovisioning():
    source = read("main/application.cc")
    activation_done = function_body(source, "void Application::HandleActivationDoneEvent")

    assert "auto state = GetDeviceState();" in activation_done
    assert "state == kDeviceStateWifiConfiguring" in activation_done
    assert "Activation done ignored because WiFi config mode is active" in activation_done
    assert activation_done.index("kDeviceStateWifiConfiguring") < activation_done.index("SetDeviceState(kDeviceStateIdle)")


def test_application_button_confirms_pending_claim_before_chat_toggle():
    source = read("main/application.cc")
    toggle_body = function_body(source, "void Application::HandleToggleChatEvent")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")
    apply_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )

    assert "ConfirmPendingTbotClaim()" in toggle_body
    assert "ClaimConfirmationReporter::Confirm(" in confirm_body
    assert "pending_tbot_claim_" in confirm_body
    assert "pending_tbot_claim_api_url_" in confirm_body
    assert "pending_tbot_claim_token_" in confirm_body
    assert "CloseAudioChannelByIntent();" in apply_body
    assert "pending_tbot_claim_ = PendingTbotClaim{}" in apply_body

    close_body = function_body(source, "void Application::CloseAudioChannelByIntent")
    assert "online_intent_.store(false)" in close_body
    assert "protocol_->CloseAudioChannel" in close_body

def test_blufi_persists_claim_bootstrap_token_without_fetching_while_ble_is_active():
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")

    assert 'Settings websocket_settings("websocket", true);' in custom_data_body
    assert '"bootstrap_token", self->bootstrap_token_' in custom_data_body
    token_branch = custom_data_body[
        custom_data_body.index("if (has_token)"):
        custom_data_body.index("if (applied && snapshot.device_id_len > 0)")
    ]
    assert "WifiManager::GetInstance().IsConnected()" in token_branch
    assert "ScheduleClaimRefreshAfterTokenHandoff();" in token_branch
    assert "self->deinit();" not in token_branch
    assert "SchedulePendingTbotClaimRefresh();" not in token_branch
    assert "void ScheduleClaimRefreshAfterTokenHandoff();" in header
    assert "tag == 0x01" in custom_data_body


def test_claim_flow_does_not_spend_bootstrap_token_on_legacy_provisioning_report():
    source = read("main/boards/common/blufi.cpp")
    report_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")

    # The backend bootstrap token is single-use. In the mobile claim flow the
    # robot must spend it on POST /claim/confirm, not on the legacy
    # provisioning-status report that would consume it first and leave mobile
    # stuck at WAITING_PHYSICAL_CONFIRM.
    claim_guard_idx = report_body.index('websocket_settings.GetString("claim_device_id")')
    first_report_start_idx = report_body.index('ProvisioningStatusReporter::Report')
    claim_guard = report_body[:first_report_start_idx]

    assert 'Settings websocket_settings("websocket", false);' in claim_guard
    assert '!websocket_settings.GetString("claim_device_id").empty()' in claim_guard
    assert 'provisioning_code_.empty()' in claim_guard
    assert "Skipping provisioning authenticated report during claim flow" in claim_guard
    assert "return;" in claim_guard[claim_guard_idx:]
    assert claim_guard_idx < first_report_start_idx


def test_new_ble_session_cannot_reuse_a_stale_provisioning_code():
    source = read("main/boards/common/blufi.cpp")
    connect_body = function_body(source, "case ESP_BLUFI_EVENT_BLE_CONNECT:")

    # Credential-only Wi-Fi changes intentionally send no claim code/token. A
    # failed prior claim report must not make that new session look code-based.
    assert "ClearProvisioningSecrets(false);" in connect_body
    assert connect_body.index("ClearProvisioningSecrets(false);") < connect_body.index(
        "_security_init();"
    )

    header = read("main/boards/common/blufi.h")
    clear_body = function_body(source, "void Blufi::ClearProvisioningSecrets")
    assert "void ClearProvisioningSecrets(bool preserve_claim_token = true);" in header
    assert "preserve_claim_token &&" in clear_body


def test_custom_claim_data_cannot_teardown_ble_before_new_wifi_is_provisioned():
    source = read("main/boards/common/blufi.cpp")
    report_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")

    provisioned_gate = report_body.index("!m_provisioned")
    ble_state_read = report_body.index("const auto ble_state = GetBleState()")
    teardown = report_body.index("CompleteSuccessfulProvisioningTeardown")

    assert ble_state_read < provisioned_gate < teardown
    assert "wifi_provisioned=%d" in report_body


def test_code_based_provisioning_reports_device_authenticated_even_with_device_id():
    source = read("main/boards/common/blufi.cpp")
    report_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")

    first_report_start_idx = report_body.index("ProvisioningStatusReporter::Report")
    claim_guard = report_body[:first_report_start_idx]
    # A device id is included in both zero-code claim and code-based provisioning.
    # Only the absence of a provisioning code identifies the claim-confirm path.
    assert 'const bool zero_code_claim_flow =' in claim_guard
    assert '!websocket_settings.GetString("claim_device_id").empty()' in claim_guard
    assert 'provisioning_code_.empty()' in claim_guard
    assert 'if (zero_code_claim_flow)' in claim_guard

def test_claim_custom_data_persists_device_id_before_token_side_effects():
    source = read("main/boards/common/blufi.cpp")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")

    gate_idx = custom_data_body.index("RunIfSetupGenerationCurrent(")
    device_store_idx = custom_data_body.index('"claim_device_id"', gate_idx)
    token_store_idx = custom_data_body.index('"bootstrap_token"', device_store_idx)
    first_report_idx = custom_data_body.index("TryReportProvisioningAuthenticated", token_store_idx)

    assert gate_idx < device_store_idx < token_store_idx < first_report_idx
    callback_prefix = custom_data_body[:custom_data_body.index("Application::GetInstance().Schedule(")]
    assert "Settings " not in callback_prefix

def test_claim_flow_preserves_nvs_bootstrap_token_until_confirm():
    source = read("main/boards/common/blufi.cpp")
    clear_body = function_body(source, "void Blufi::ClearProvisioningSecrets")

    claim_guard_idx = clear_body.index('zero_code_claim_flow')
    erase_idx = clear_body.index('websocket_settings.EraseKey("bootstrap_token")')
    preserve_idx = clear_body.index("Claim flow active; preserving NVS bootstrap token")

    assert claim_guard_idx < erase_idx < preserve_idx
    assert '!websocket_settings.GetString("claim_device_id").empty()' in clear_body
    assert 'provisioning_code_.empty()' in clear_body
    assert clear_body.index("const bool zero_code_claim_flow =") < clear_body.index(
        "provisioning_code_.clear();"
    )


def test_blufi_defers_connected_wifi_claim_refresh_until_after_possible_wifi_frames():
    source = read("main/boards/common/blufi.cpp")
    helper_body = function_body(source, "void Blufi::ScheduleClaimRefreshAfterTokenHandoff")

    assert "kClaimRefreshAfterTokenHandoffDelayMs" in source
    assert "vTaskDelay(pdMS_TO_TICKS(kClaimRefreshAfterTokenHandoffDelayMs));" in helper_body
    assert "m_sta_is_connecting" in helper_body
    connecting_branch = helper_body[helper_body.index("m_sta_is_connecting"):]
    teardown_idx = connecting_branch.index("CompleteSuccessfulProvisioningTeardown")
    assert "return;" in connecting_branch[:teardown_idx]
    assert '"connected_wifi_token_handoff"' in helper_body
    assert "SchedulePendingTbotClaimRefresh(generation);" in helper_body

def test_one_shot_claim_fetch_after_wifi_success_applies_even_without_poll_timer():
    source = read("main/application.cc")
    task_body = function_body(source, "void Application::ClaimFetchTask")

    # SchedulePendingTbotClaimRefresh() can dispatch a one-shot device/config
    # fetch immediately after Wi-Fi provisioning, before StartClaimPoll() has
    # armed claim_poll_active_. Results with a bootstrap token must still apply:
    #  - active claim -> /claim/confirm (mobile "Đang chờ Robot xác thực")
    #  - claim_present=0 -> EnsureBleAdvertisingForStandby (else phone BLE_SCAN_TIMEOUT)
    stale_guard_start = task_body.index("if (!self->claim_poll_active_")
    apply_idx = task_body.index("ApplyPendingTbotClaimFetchResult")
    stale_guard = task_body[stale_guard_start:apply_idx]

    # Drop only when poll is off AND there is no bootstrap token (pure poll tick).
    assert "token.empty()" in stale_guard
    assert "return;" in stale_guard
    # Must not require pending_claim.active — claim_present=0 still needs apply.
    assert "pending_claim.active" not in stale_guard


def test_successful_no_claim_fetch_keeps_token_and_retries_delayed_claim_visibility():
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")

    # The app and backend can expose the new claim a few seconds after the BLE
    # handoff. A first HTTP 200 with claim_present=0 is therefore not proof that
    # the bootstrap token is abandoned. Preserve it and use the bounded poll;
    # an explicit 401/403 or an exhausted visibility window may clear it.
    assert "if (fetched && !pending_claim.active && !token.empty())" not in apply_body
    no_claim_start = apply_body.index("if (!fetched || !pending_claim.active)")
    active_claim_start = apply_body.index("// Fetch succeeded with an active claim")
    no_claim_branch = apply_body[no_claim_start:active_claim_start]
    no_claim_retry_body = no_claim_branch[:no_claim_branch.index("const int64_t now_ms")]
    assert "pending_tbot_claim_token_ = token;" in no_claim_retry_body
    assert "StartClaimPoll();" in no_claim_branch
    assert 'websocket_settings.SetString("bootstrap_token", "");' not in no_claim_retry_body

    # The active-claim result path still tears BLE down once immediately before
    # confirmation.
    active_claim = apply_body[apply_body.index("// Fetch succeeded with an active claim") :]
    assert active_claim.index("ClaimBleLifecycleIntent::kStopAdvertising") < active_claim.index(
        "ConfirmPendingTbotClaim"
    )

def test_token_backed_no_claim_retry_does_not_reinitialize_ble_each_poll():
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    no_claim_start = apply_body.index("if (!fetched || !pending_claim.active)")
    active_claim_start = apply_body.index("// Fetch succeeded with an active claim")
    no_claim_branch = apply_body[no_claim_start:active_claim_start]
    no_token_start = no_claim_branch.index("if (token.empty())")
    before_token_split = no_claim_branch[:no_token_start]
    no_token_branch = no_claim_branch[
        no_token_start:no_claim_branch.index("return;", no_token_start) + len("return;")
    ]
    token_branch = no_claim_branch[no_claim_branch.index("return;", no_token_start) + len("return;") :]

    # Once the phone has delivered a bootstrap token, BLE has already served
    # its handoff purpose. Keep it off while backend claim visibility catches
    # up; repeated deinit/init cycles exhaust ESP32-S3 internal heap.
    assert "EnsureBleAdvertisingForStandby" not in before_token_split
    assert "ClaimBleLifecycleIntent::kEnsureAdvertising" in no_token_branch
    assert "StartClaimPoll();" in token_branch
    assert "EnsureBleAdvertisingForStandby" not in token_branch

def test_token_backed_no_claim_retry_expires_into_stable_ble_standby():
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    no_claim_start = apply_body.index("if (!fetched || !pending_claim.active)")
    active_claim_start = apply_body.index("// Fetch succeeded with an active claim")
    no_claim_branch = apply_body[no_claim_start:active_claim_start]
    token_branch = no_claim_branch[no_claim_branch.index("return;", no_claim_branch.index("if (token.empty())")) + len("return;") :]

    assert "kClaimVisibilityRetryWindowMs" in source
    assert "kClaimVisibilityRetryWindowMs = 20LL * 1000LL" in source
    assert "claim_poll_started_ms_" in token_branch
    assert 'websocket_settings.SetString("bootstrap_token", "");' in token_branch
    assert "ClearProvisioningSecrets();" in token_branch
    assert "ClaimBleLifecycleIntent::kEnsureAdvertising" in token_branch
    assert token_branch.index("StopClaimPoll();") < token_branch.index(
        "ClaimBleLifecycleIntent::kEnsureAdvertising"
    )


def test_blufi_waits_for_full_wifi_board_connect_timeout_before_reporting_failure():
    source = read("main/boards/common/blufi.cpp")
    branch_start = source.index("void Blufi::StartStationConnectFromCredentials")
    failure_log_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", branch_start)
    connect_body = source[branch_start:failure_log_idx]

    assert "constexpr int kConnectTimeoutMs = 60000;" in connect_body

def test_application_exposes_scheduled_pending_claim_refresh_for_late_ble_token():
    header = read("main/application.h")
    source = read("main/application.cc")
    schedule_body = function_body(source, "void Application::SchedulePendingTbotClaimRefresh")

    assert "void SchedulePendingTbotClaimRefresh(uint32_t expected_setup_generation);" in header
    assert "Schedule([this, expected_setup_generation]()" in schedule_body
    run_idx = schedule_body.index("RunIfSetupGenerationCurrent(")
    assert "expected_setup_generation" in schedule_body[run_idx:run_idx + 180]
    assert "deferred_effects.dispatch_refresh = true" in schedule_body
    assert "ExecuteClaimDeferredEffects" in schedule_body
    assert "RefreshPendingTbotClaim();" not in schedule_body


def test_application_runs_a_bounded_claim_poll_not_a_single_shot():
    # C4: the one-shot /device/config fetch becomes a BOUNDED poll (cadence +
    # 5-minute window cap), with an "if unowned -> claimable standby" trigger.
    header = read("main/application.h")
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    start_body = function_body(source, "void Application::StartClaimPoll")
    tick_body = function_body(source, "void Application::PollPendingTbotClaimTick")

    # Periodic timer + window cap constants exist (no tight loop).
    assert "esp_timer_handle_t claim_poll_timer_" in header
    assert "kClaimPollIntervalUs" in source
    assert "kClaimPollWindowMs" in source
    assert "claim_poll_interval_us_ = desired_interval_us;" in start_body
    assert "esp_timer_start_periodic(claim_poll_timer_, claim_poll_interval_us_)" in start_body

    # Unowned -> claimable standby trigger lives in the refresh/apply path.
    assert "TbotClaimSubstate::AvailableStandby" in refresh_body
    assert "StartClaimPoll();" in function_body(
        source, "void Application::ApplyPendingTbotClaimFetchResult"
    )

    # The window cap stops the poll and surfaces a timeout (no infinite poll).
    assert "claim_poll_started_ms_" in tick_body
    assert "kClaimPollWindowMs" in tick_body
    assert "HandleClaimConfirmTimeout();" in tick_body

def test_unclaimed_no_token_standby_waits_on_ble_without_periodic_reinit_cycle():
    # Physical ESP32-S3 evidence: repeatedly deinit/init Bluedroid around a
    # no-token liveness poll eventually asserts in vQueueDelete. After one fetch
    # refreshes backend liveness, leave BLE advertising continuously; custom-data
    # token handoff schedules the next claim refresh directly.
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")

    no_claim_start = apply_body.index("if (!fetched || !pending_claim.active)")
    pending_start = apply_body.index("Fetch succeeded with an active claim", no_claim_start)
    no_claim_branch = apply_body[no_claim_start:pending_start]
    no_token_start = no_claim_branch.index("if (token.empty())")
    no_token_branch = no_claim_branch[no_token_start:no_claim_branch.index("return;", no_token_start) + len("return;")]

    assert "ClaimBleLifecycleIntent::kEnsureAdvertising" in no_token_branch
    assert "StopClaimPoll();" in no_token_branch
    assert "StartClaimPoll();" not in no_token_branch

def test_claimed_devices_leave_claim_fsm_before_standby_ble_advertising():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    ensure_body = function_body(source, "bool Application::EnsureBleAdvertisingForStandbyImpl")
    stop_ble_body = function_body(source, "bool Application::StopBleAdvertisingImpl")

    assert "IsDeviceClaimed()" in refresh_body
    assert "StopClaimPoll();" in refresh_body
    assert "StopBleAdvertising();" in refresh_body
    assert "return;" in refresh_body[refresh_body.index("if (IsDeviceClaimed())") :]
    assert "IsDeviceClaimed()" in ensure_body
    assert "StopBleAdvertisingForSetupGeneration" in ensure_body
    assert "StopBleAdvertisingImpl(std::nullopt)" in ensure_body
    assert "blufi.deinit() == ESP_OK" in stop_ble_body

    claimed_idx = refresh_body.index("if (IsDeviceClaimed())")
    standby_idx = refresh_body.index("Blufi::GetInstance().GetBleState()")
    claimed_branch = refresh_body[claimed_idx:refresh_body.index("return;", claimed_idx)]

    assert claimed_idx < standby_idx
    assert "StopClaimPoll();" in claimed_branch
    assert "StopBleAdvertising();" in claimed_branch
    assert "EnsureBleAdvertisingForStandby();" not in claimed_branch
    assert "if (IsDeviceClaimed())" in ensure_body
    claimed_ensure = ensure_body[:ensure_body.index("if (blufi.GetBleState()")]
    assert "StopBleAdvertisingForSetupGeneration" in claimed_ensure
    assert "StopBleAdvertisingImpl(std::nullopt)" in claimed_ensure
    assert "CancelBleSetupTimeout();" in stop_ble_body
    assert "StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);" in ensure_body


def test_factory_test_claimed_is_ota_controlled_and_requires_ws_token():
    ota = read("main/ota.cc")
    app = read("main/application.cc")
    ota_body = function_body(ota, "esp_err_t Ota::CheckVersion")
    claimed_body = function_body(app, "bool Application::IsDeviceClaimed")

    assert 'factory_test_claimed' in ota_body
    assert 'Settings claim_state("tbot_claim", true);' in ota_body
    assert 'claim_state.SetInt("factory_test",' in ota_body
    assert 'claim_state.SetInt("factory_test", 0);' in ota_body
    assert 'websocket_settings.GetString("token")' in claimed_body
    assert 'claim_state.GetInt("factory_test", 0) != 0' in claimed_body
    assert 'factory_test_claimed && !websocket_token.empty()' in claimed_body

def test_factory_test_claimed_nvs_key_fits_esp_idf_limit():
    ota = read("main/ota.cc")
    app = read("main/application.cc")
    ota_body = function_body(ota, "esp_err_t Ota::CheckVersion")
    claimed_body = function_body(app, "bool Application::IsDeviceClaimed")
    websocket_section = ota_body[
        ota_body.index('cJSON *websocket') : ota_body.index('cJSON *api_url')
    ]

    # ESP-IDF NVS keys are limited to 15 chars. The OTA JSON field can keep the
    # descriptive protocol name, but the persisted key must stay short or the
    # device aborts during OTA parsing before it can enter the claimed path.
    assert 'SetInt("factory_test_claimed"' not in ota_body
    assert 'GetInt("factory_test_claimed"' not in claimed_body
    assert 'settings.SetInt(item->string, item->valueint);' not in websocket_section
    assert 'std::strcmp(item->string, "factory_test_claimed") == 0' in websocket_section
    assert 'claim_state.SetInt("factory_test",' in websocket_section
    assert len("factory_test") <= 15


def test_pending_cloud_release_cannot_start_claimed_runtime_or_audio():
    source = read("main/application.cc")
    claimed_body = function_body(source, "bool Application::IsDeviceClaimed")

    assert 'backend_settings.GetInt("release_pending", 0)' in claimed_body
    pending_check = claimed_body.index('backend_settings.GetInt("release_pending", 0)')
    credential_return = claimed_body.index(
        "return !device_id.empty() && !device_secret.empty();"
    )
    assert pending_check < credential_return
    assert "return false;" in claimed_body[pending_check:credential_return]


def test_saved_wifi_recovery_public_entry_keeps_claim_gate_inside_application():
    header = read("main/application.h")
    source = read("main/application.cc")
    recovery_body = function_body(source, "void Application::EnsureBleAdvertisingForUnclaimedSavedWifi")

    assert "void EnsureBleAdvertisingForUnclaimedSavedWifi();" in header
    assert "if (IsDeviceClaimed())" in recovery_body
    claimed_branch = recovery_body[recovery_body.index("if (IsDeviceClaimed())"):]
    assert "return;" in claimed_branch[:claimed_branch.index("EnsureBleAdvertisingForStandby();")]
    assert "Stored WiFi exists but device is unclaimed" in recovery_body
    assert "EnsureBleAdvertisingForStandby();" in recovery_body

def test_application_enforces_local_claim_expiry_against_a_clock():
    # C4: expires_at must be compared to a clock and drive CLAIM_CONFIRM_TIMEOUT.
    source = read("main/application.cc")
    arm_body = function_body(source, "void Application::ArmClaimExpiryTimer")
    timeout_body = function_body(source, "void Application::HandleClaimConfirmTimeout")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    assert "ParseIso8601UtcToEpoch(pending_tbot_claim_.expires_at" in arm_body
    assert "time(nullptr)" in arm_body
    assert "esp_timer_start_once(claim_expiry_timer_" in arm_body
    # Tapping an already-expired window must not blind-confirm.
    assert "IsPendingTbotClaimExpired(pending_tbot_claim_, time(nullptr))" in confirm_body
    # Timeout renders the spec copy "Setup expired".
    assert "TbotClaimSubstate::ConfirmTimeout" in timeout_body
    assert "Lang::Strings::SETUP_EXPIRED" in timeout_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in timeout_body
    assert "SecureClearString(pending_tbot_claim_token_);" in timeout_body
    assert "Blufi::GetInstance().ClearProvisioningSecrets();" in timeout_body
    assert 'websocket_settings.EraseKey("claim_device_id");' in timeout_body


def test_successful_claim_confirm_zeroizes_blufi_ram_secrets():
    source = read("main/application.cc")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    success_start = confirm_body.index(
        "CancelClaimExpiryTimer();",
        confirm_body.index("ClaimConfirmationResult::TerminalFailure") + 1,
    )
    success_body = confirm_body[success_start:]

    nvs_clear = success_body.index('websocket_settings.SetString("bootstrap_token", "");')
    ram_clear = success_body.index("Blufi::GetInstance().ClearProvisioningSecrets();")
    assert nvs_clear < ram_clear
    assert 'websocket_settings.EraseKey("claim_device_id");' in success_body

def test_blufi_token_only_claim_path_does_not_report_device_authenticated_before_button_press():
    source = read("main/boards/common/blufi.cpp")
    success_start = source.index("if (credentials_committed)")
    success_end = source.index("Failed to connect to WiFi via esp-wifi-connect", success_start)
    success_body = source[success_start:success_end]
    helper_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")
    skip_idx = helper_body.index("token_empty || code_empty")
    report_idx = helper_body.index("ProvisioningStatusReporter::Status::DeviceAuthenticated")

    assert "ProvisioningStatusReporter::Status::DeviceAuthenticated" not in success_body
    assert '"wifi_success_after_ble_teardown", generation' in success_body
    assert skip_idx < report_idx
    assert "Reporting provisioning authenticated skipped" in helper_body
    assert "ClearProvisioningSecrets();" in helper_body

def test_blufi_wifi_success_tears_down_ble_before_claim_refresh():
    # Real ESP32-S3 hardware can connect TCP but fail mbedTLS AES allocation if
    # the claim HTTPS poll runs while BluFi/BLE is still active. After the phone
    # has received the Wi-Fi success report, tear BLE down before refreshing the
    # pending claim.
    source = read("main/boards/common/blufi.cpp")
    branch_start = source.index("if (credentials_committed)")
    helper_start = source.index("void Blufi::StartStationConnectFromCredentials")
    release_idx = source.index("ReleaseBleForStationAssociation", helper_start)
    station_idx = source.index("wifi.StartStationWithCredentialsIfScanIdle(", release_idx)
    connected_log_idx = source.index('"connected to WiFi"', station_idx)
    failure_log_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", connected_log_idx)
    success_body = source[branch_start:failure_log_idx]

    assert "ReleaseBleForStationAssociation" in source[helper_start:station_idx]
    exact_start = source[station_idx:branch_start]
    assert "candidate_ssid_string" in exact_start
    assert "candidate_password" in exact_start
    assert "WifiManager::StationStartResult::kStartedNow" in exact_start
    assert "CompleteSuccessfulProvisioningTeardown" in success_body
    assert '"wifi_credentials_connected"' in success_body
    assert "const bool code_based_provisioning" in success_body
    assert "if (code_based_provisioning)" in success_body
    assert "SchedulePendingTbotClaimRefresh(generation);" in success_body
    code_branch = success_body[
        success_body.index("if (code_based_provisioning)"):
        success_body.index("SchedulePendingTbotClaimRefresh(generation);")
    ]
    assert "TryReportProvisioningAuthenticated" in code_branch
    assert "return;" in code_branch
    teardown_idx = source.index("CompleteSuccessfulProvisioningTeardown", connected_log_idx)
    assert release_idx < station_idx < connected_log_idx < teardown_idx
    assert teardown_idx < source.index(
        "SchedulePendingTbotClaimRefresh(generation);", connected_log_idx
    )


def test_code_based_report_success_promotes_robot_out_of_wifi_setup():
    source = read("main/boards/common/blufi.cpp")
    report = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")
    callback_start = report.index(
        "Application::GetInstance().Schedule([self, ok, expected_generation]()"
    )
    callback = report[callback_start:]
    success_start = callback.index("if (ok)")
    failure_start = callback.index("} else {", success_start)
    success = callback[success_start:failure_start]
    failure = callback[failure_start:]

    clear_idx = success.index("self->ClearProvisioningSecrets();")
    refresh_idx = success.index("SchedulePendingTbotClaimRefresh(", clear_idx)
    assert "expected_generation" in success[refresh_idx:refresh_idx + 100]
    assert clear_idx < refresh_idx
    assert "SchedulePendingTbotClaimRefresh" not in failure


def test_code_based_custom_data_does_not_schedule_claim_refresh_after_token_handoff():
    source = read("main/boards/common/blufi.cpp")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")
    applied_body = custom_data_body[
        custom_data_body.index("const bool applied ="):
        custom_data_body.index("if (applied && snapshot.device_id_len > 0)")
    ]

    assert "self->provisioning_code_.assign" in applied_body
    schedule_idx = applied_body.index("self->ScheduleClaimRefreshAfterTokenHandoff();")
    assign_idx = applied_body.index("self->provisioning_code_.assign")
    assert assign_idx < schedule_idx
    schedule_guard = applied_body[assign_idx:schedule_idx]
    assert "self->provisioning_code_.empty()" in schedule_guard

def test_blufi_reports_device_authenticated_only_after_ble_teardown():
    # ESP32-S3 cannot reliably run provisioning HTTPS while the BluFi/BLE stack
    # is still up; BLE+TLS can fail internal AES allocation and leave mobile
    # polling forever for the robot's device_authenticated callback.
    source = read("main/boards/common/blufi.cpp")
    header = read("main/boards/common/blufi.h")
    success_start = source.index("if (credentials_committed)")
    helper_start = source.index("void Blufi::StartStationConnectFromCredentials")
    release_idx = source.index("ReleaseBleForStationAssociation", helper_start)
    station_idx = source.index("wifi.StartStationWithCredentialsIfScanIdle(", release_idx)
    failure_idx = source.index("Failed to connect to WiFi via esp-wifi-connect", station_idx)
    success_body = source[success_start:failure_idx]
    helper_body = function_body(source, "void Blufi::TryReportProvisioningAuthenticated")
    custom_data_body = function_body(source, "case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:")

    assert "void TryReportProvisioningAuthenticated(const char* reason, uint32_t expected_generation);" in header
    assert '"wifi_success_after_ble_teardown", generation' in success_body
    assert release_idx < station_idx
    assert station_idx < success_start
    assert success_body.index("CompleteSuccessfulProvisioningTeardown") < success_body.index(
        '"wifi_success_after_ble_teardown", generation'
    )
    assert "ProvisioningStatusReporter::Status::DeviceAuthenticated" in helper_body
    assert "ClearProvisioningSecrets();" in helper_body
    assert "Reporting provisioning authenticated skipped" in helper_body
    assert custom_data_body.count("TryReportProvisioningAuthenticated") >= 2

def test_claim_confirm_uses_only_ble_bootstrap_token_not_realtime_ws_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert 'websocket_settings.GetString("bootstrap_token")' in refresh_body
    # websocket.token is the realtime/OTA token. It is not a claim bootstrap
    # token and can be stale/consumed, so the claim-confirm path must never fall
    # back to it.
    assert 'websocket_settings.GetString("token")' not in refresh_body

def test_pending_claim_without_bootstrap_token_keeps_ble_open_and_does_not_confirm():
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    pending_start = apply_body.index("Fetch succeeded with an active claim")
    pending_body = apply_body[pending_start:]

    assert "if (token.empty())" in pending_body
    branch_start = pending_body.index("if (token.empty())")
    empty_token_branch = pending_body[branch_start:pending_body.index("StopClaimPoll();", branch_start)]
    assert "ClaimBleLifecycleIntent::kEnsureAdvertising" in empty_token_branch
    assert "StartClaimPoll();" in empty_token_branch
    assert "return;" in empty_token_branch
    assert "StopBleAdvertising();" not in empty_token_branch
    assert "ConfirmPendingTbotClaim" not in empty_token_branch

def test_auto_confirm_continuations_delegate_retry_policy_without_clearing_token():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")

    assert "Auto-confirm POST did not land" not in refresh_body
    assert "Auto-confirm POST did not land" not in apply_body
    assert "ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);" in refresh_body
    assert "ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);" in apply_body

def test_successful_claim_confirm_clears_consumed_bootstrap_token_before_reboot():
    # The BluFi bootstrap token is attempt-scoped. After /claim/confirm succeeds
    # the device persists device_secret/ws_url, so leaving the consumed bootstrap
    # token in NVS makes the next reboot poll /device/config with stale auth and
    # can show a false backend retry state before WS comes online.
    source = read("main/application.cc")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )

    success_start = confirm_body.index("CancelClaimExpiryTimer();")
    success_end = confirm_body.index("pending_tbot_claim_ = PendingTbotClaim{}", success_start)
    success_body = confirm_body[success_start:success_end]

    assert 'Settings websocket_settings("websocket", true);' in success_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in success_body
    assert "SecureClearString(pending_tbot_claim_token_);" in confirm_body[success_start:]

def test_button_confirm_without_bootstrap_token_waits_for_ble_instead_of_failing():
    source = read("main/application.cc")
    confirm_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")

    assert "pending_tbot_claim_token_.empty()" in confirm_body
    wait_branch = confirm_body[confirm_body.index("pending_tbot_claim_token_.empty()"):confirm_body.index(
        "ClaimConfirmationReporter::Confirm"
    )]
    assert "EnsureBleAdvertisingForStandby();" in wait_branch
    assert "StartClaimPoll();" in wait_branch
    assert "RenderClaimSubstate(claim_substate_);" in wait_branch
    assert "Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::CONNECTION_CONFIRM_FAILED" not in wait_branch

def test_retryable_claim_confirm_preserves_token_claim_and_ble_teardown():
    source = read("main/application.cc")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    retry_start = confirm_body.index("ClaimConfirmationResult::RetryableFailure")
    terminal_start = confirm_body.index("ClaimConfirmationResult::TerminalFailure", retry_start)
    retry_body = confirm_body[retry_start:terminal_start]

    assert 'SetString("bootstrap_token", "")' not in retry_body
    assert "pending_tbot_claim_token_.clear()" not in retry_body
    assert "pending_tbot_claim_ = PendingTbotClaim{}" not in retry_body
    assert "EnsureBleAdvertisingForStandby" not in retry_body
    assert "ClearProvisioningSecrets" not in retry_body
    assert "StartClaimPoll();" in retry_body
    assert "claim_substate_ = TbotClaimSubstate::WaitingConfirm;" in retry_body


def test_ambiguous_claim_success_stops_retries_without_clearing_or_reopening_ble():
    source = read("main/application.cc")
    header = read("main/application.h")
    confirm_guard_body = function_body(source, "bool Application::ConfirmPendingTbotClaim")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    ambiguous_start = confirm_body.index("ClaimConfirmationResult::AmbiguousSuccess")
    terminal_start = confirm_body.index("ClaimConfirmationResult::TerminalFailure", ambiguous_start)
    ambiguous_body = confirm_body[ambiguous_start:terminal_start]

    assert "claim_confirmation_ambiguous_" in header
    assert "claim_confirmation_ambiguous_ = true;" in ambiguous_body
    assert 'SetInt("claim_ambiguous", 1);' in ambiguous_body
    assert "StopClaimPoll();" in ambiguous_body
    assert "StartClaimPoll();" not in ambiguous_body
    assert 'SetString("bootstrap_token", "")' not in ambiguous_body
    assert "SecureClearString(pending_tbot_claim_token_)" not in ambiguous_body
    assert "ClearProvisioningSecrets" not in ambiguous_body
    assert "EnsureBleAdvertisingForStandby" not in ambiguous_body
    assert "TbotClaimSubstate::Confirmed" not in ambiguous_body
    assert "Lang::Strings::CLAIM_CONFIRM_SUPPORT_REQUIRED" in ambiguous_body

    guard_start = confirm_guard_body.index("if (claim_confirmation_ambiguous_)")
    report_start = confirm_guard_body.index("ClaimConfirmationReporter::Confirm", guard_start)
    guard_body = confirm_guard_body[guard_start:report_start]
    assert "StopClaimPoll();" in guard_body
    assert "return true;" in guard_body

    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    persisted_guard = refresh_body[refresh_body.index('GetInt("claim_ambiguous", 0)') :]
    dispatch = persisted_guard.index("DispatchPendingTbotClaimFetch")
    persisted_guard = persisted_guard[:dispatch]
    assert "claim_confirmation_ambiguous_ = true;" in persisted_guard
    assert "StopClaimPoll();" in persisted_guard
    assert "StopBleAdvertising();" in persisted_guard
    assert "return;" in persisted_guard

    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    assert apply_body.index("if (claim_confirmation_ambiguous_)") < apply_body.index(
        "device_config_status == 401"
    )

    repair_body = function_body(source, "void Application::EnterRepairPairingMode")
    assert 'SetInt("claim_ambiguous", 0);' in repair_body


def test_terminal_claim_confirm_clears_token_and_reopens_ble():
    source = read("main/application.cc")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    terminal_start = confirm_body.index("ClaimConfirmationResult::TerminalFailure")
    terminal_end = confirm_body.index("return true;", terminal_start)
    terminal_body = confirm_body[terminal_start:terminal_end]

    assert 'Settings websocket_settings("websocket", true);' in terminal_body
    assert 'websocket_settings.SetString("bootstrap_token", "");' in terminal_body
    assert "SecureClearString(pending_tbot_claim_token_);" in terminal_body
    assert "pending_tbot_claim_ = PendingTbotClaim{}" in terminal_body
    assert "Blufi::GetInstance().ClearProvisioningSecrets();" in terminal_body
    assert 'websocket_settings.EraseKey("claim_device_id");' in terminal_body
    assert "EnsureBleAdvertisingForStandby();" in terminal_body
    assert "StartClaimPoll();" not in terminal_body

def test_cached_pending_claim_with_late_ble_token_confirms_without_refetching_config():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    claim_fetch_body = function_body(source, "void Application::ClaimFetchTask")

    assert "pending_tbot_claim_.active && !token.empty()" in refresh_body
    cached_start = refresh_body.index("pending_tbot_claim_.active && !token.empty()")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);")
    assert cached_start < dispatch_idx
    cached_branch = refresh_body[cached_start:dispatch_idx]

    # Once BluFi has delivered a fresh bootstrap token for an already-cached
    # backend claim, do not run another TLS /device/config fetch while BLE is
    # still active. Stop BLE first, then confirm the cached claim directly.
    assert "StopBleAdvertising();" in cached_branch
    assert "ConfirmPendingTbotClaim(/*trust_backend_expiry=*/true);" in cached_branch
    assert "FetchPendingTbotClaimFromDeviceConfig" not in cached_branch
    assert "FetchPendingTbotClaimFromDeviceConfig" in claim_fetch_body

def test_unclaimed_standby_without_bootstrap_token_never_polls_backend_config_even_when_ble_is_off():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fallback_idx = refresh_body.index("FetchBackendApiUrlFromBootstrap(token)")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);")
    pre_network = refresh_body[token_read:min(fallback_idx, dispatch_idx)]

    # If BLE init failed or was already off, GetBleState() no longer protects the
    # no-token path. Standby must still reach backend /device/config without an
    # Authorization header so ClaimService.refreshSetupSignalIfUnclaimed keeps the
    # robot visible to the mobile claim list. The low-priority ClaimFetchTask owns
    # the blocking TLS fetch, so this no longer wedges the Application task.
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_network
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_network

def test_unclaimed_standby_polls_device_config_without_bootstrap_token_to_refresh_mobile_discovery():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);")
    pre_fetch = refresh_body[token_read:dispatch_idx]

    ble_state_idx = pre_fetch.index("Blufi::GetInstance().GetBleState()")

    # There must be no generic early-return for an empty bootstrap token before
    # the backend config fetch is dispatched. FetchPendingTbotClaimFromDeviceConfig
    # omits the Authorization header when token is empty, and the backend accepts
    # that as an unclaimed setup-liveness poll.
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_fetch[:ble_state_idx]
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_fetch
    assert "Blufi::GetInstance().GetBleState()" in pre_fetch
    assert "DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);" in refresh_body

def test_unclaimed_standby_with_bootstrap_token_waits_for_connected_ble_handoff():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    token_read = refresh_body.index('std::string token = websocket_settings.GetString("bootstrap_token");')
    fallback_idx = refresh_body.index("FetchBackendApiUrlFromBootstrap(token)")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);")
    pre_network = refresh_body[token_read:min(fallback_idx, dispatch_idx)]

    # A token can arrive before SSID/password. A live GATT connection therefore
    # always owns the handoff and must not be torn down by the periodic poll.
    # Advertising (no connected phone) may still be stopped before TLS.
    assert "Blufi::GetInstance().GetBleState()" in pre_network
    assert "pending_tbot_claim_.active" in pre_network
    assert "Skipping claim config fetch until BluFi token handoff" not in pre_network
    assert "if (!pending_tbot_claim_.active && token.empty())" not in pre_network
    ble_branch_start = pre_network.index("Blufi::GetInstance().GetBleState()")
    ble_branch = pre_network[ble_branch_start:]
    assert "ble_state == Blufi::BleState::kConnected" in ble_branch
    connected_branch = ble_branch[ble_branch.index("ble_state == Blufi::BleState::kConnected"):]
    connected_branch = connected_branch[:connected_branch.index("BleState::kAdvertising")]
    assert "return;" in connected_branch
    assert "StopBleAdvertising();" not in connected_branch

    token_present_branch = ble_branch[ble_branch.index("Bootstrap token present but BLE still active"):]

    assert "CancelBleSetupTimeout();" in token_present_branch
    assert "StopBleAdvertising();" in token_present_branch
    assert "return;" not in token_present_branch[:token_present_branch.index("StopBleAdvertising();")]
    assert "Retrying delayed claim visibility while keeping BLE session stable" not in pre_network


def test_unclaimed_standby_without_token_pauses_advertising_before_tls_fetch():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    dispatch_idx = refresh_body.index("DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);")
    pre_fetch = refresh_body[:dispatch_idx]

    # Physical ESP32-S3 evidence: Wi-Fi + BluFi advertising leaves too little
    # internal SRAM for DNS/TLS (getaddrinfo EAI_MEMORY). Preserve a live BLE
    # connection so custom-data can arrive, but pause advertising before the
    # no-token setup-liveness fetch. ApplyPendingTbotClaimFetchResult reopens BLE.
    assert "ble_state == Blufi::BleState::kConnected" in pre_fetch
    connected_branch = pre_fetch[pre_fetch.index("ble_state == Blufi::BleState::kConnected"):]
    assert "return;" in connected_branch

    assert "ble_state == Blufi::BleState::kAdvertising && token.empty()" in pre_fetch
    advertising_branch = refresh_body[refresh_body.index("ble_state == Blufi::BleState::kAdvertising && token.empty()"):]
    assert "CancelBleSetupTimeout();" in advertising_branch
    assert "StopBleAdvertising();" in advertising_branch
    assert advertising_branch.index("StopBleAdvertising();") < advertising_branch.index(
        "DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);"
    )


def test_no_token_fetch_reopens_ble_even_when_periodic_poll_was_not_active():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    dispatch_body = function_body(source, "bool Application::DispatchPendingTbotClaimFetch")
    worker_body = function_body(source, "void Application::ClaimFetchTask")

    assert "bool apply_when_poll_inactive" in source
    assert "paused_ble_for_fetch = true;" in refresh_body
    assert "DispatchPendingTbotClaimFetch(api_url, token, paused_ble_for_fetch);" in refresh_body
    assert "apply_when_poll_inactive" in worker_body
    assert "!self->claim_poll_active_ && token.empty() && !apply_when_poll_inactive" in worker_body


def test_paused_no_token_fetch_restores_ble_and_retries_when_dispatch_is_not_accepted():
    source = read("main/application.cc")
    header = read("main/application.h")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")
    dispatch_body = function_body(source, "bool Application::DispatchPendingTbotClaimFetch")

    assert "bool DispatchPendingTbotClaimFetch" in header
    assert "const bool claim_fetch_dispatched =" in refresh_body
    assert "if (paused_ble_for_fetch && !claim_fetch_dispatched)" in refresh_body
    rollback = refresh_body[
        refresh_body.index("if (paused_ble_for_fetch && !claim_fetch_dispatched)") :
    ]
    assert "claim_substate_ = TbotClaimSubstate::AvailableStandby;" in rollback
    assert "EnsureBleAdvertisingForStandby();" in rollback
    assert "StartClaimPoll();" in rollback
    assert "StopClaimPoll();" not in rollback

    assert dispatch_body.count("return false;") >= 3
    assert "new (std::nothrow) ClaimFetchContext" in dispatch_body
    allocation_failure = dispatch_body[dispatch_body.index("new (std::nothrow) ClaimFetchContext") :]
    assert "if (ctx == nullptr)" in allocation_failure
    assert "claim_poll_inflight_.store(false);" in allocation_failure[
        : allocation_failure.index("xTaskCreateWithCaps")
    ]
    task_failure = dispatch_body[dispatch_body.index("xTaskCreateWithCaps") :]
    assert "return false;" in task_failure
    assert task_failure.rfind("return true;") > task_failure.index("return false;")


def test_audio_service_start_is_idempotent_while_workers_are_running():
    header = read("main/audio/audio_service.h")
    source = read("main/audio/audio_service.cc")
    start_workers = function_body(source, "bool AudioService::StartWorkers")

    assert "std::atomic<bool> start_in_progress_{false};" in header
    assert "std::atomic<bool> service_running_{false};" in header
    claim = start_workers.index(
        "if (!start_in_progress_.compare_exchange_strong("
    )
    claim_failure = function_body(
        start_workers, "if (!start_in_progress_.compare_exchange_strong("
    )
    assert "start_in_progress_.compare_exchange_strong(" in claim_failure
    assert "return false;" in claim_failure
    running = start_workers.index("if (IsRunning())", claim)
    assert start_workers.index("return false;", claim) < running
    first_creation = start_workers.index(
        "AudioWorkerStartTransaction::StartOnce", running
    )
    assert claim < running < first_creation
    duplicate = start_workers[running:first_creation]
    assert "start_in_progress_.store(false" in duplicate
    assert "return true;" in duplicate
    assert "std::memory_order_acq_rel" in start_workers

def test_auth_rejected_device_config_clears_stale_bootstrap_token_without_server_unavailable_copy():
    source = read("main/application.cc")
    reporter_header = read("main/provisioning/claim_confirmation_reporter.h")
    claim_fetch_body = function_body(source, "void Application::ClaimFetchTask")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")

    fetch_idx = claim_fetch_body.index("FetchPendingTbotClaimFromDeviceConfig")
    failure_copy_idx = apply_body.index("Lang::Strings::SERVER_UNAVAILABLE_RETRYING")
    auth_branch_start = apply_body.index("device_config_status == 401")
    auth_branch = apply_body[auth_branch_start:failure_copy_idx]

    # A consumed/stale bootstrap token makes the backend return 401/403. That is
    # not a server outage and must not increment into the Vietnamese
    # "Máy chủ không khả dụng" copy. Clear the stale token, reopen claimable BLE,
    # and wait for the phone to deliver a fresh attempt token.
    assert "int device_config_status" in claim_fetch_body[:fetch_idx]
    assert "int* http_status_code" in reporter_header
    assert "device_config_status == 401" in auth_branch
    assert "device_config_status == 403" in auth_branch
    assert "Device config rejected bootstrap token" in auth_branch
    assert 'websocket_settings.SetString("bootstrap_token", "");' in auth_branch
    assert "SecureClearString(pending_tbot_claim_token_);" in auth_branch
    assert 'websocket_settings.EraseKey("claim_device_id");' in auth_branch
    assert "pending_tbot_claim_ = PendingTbotClaim{};" in auth_branch
    assert "claim_fetch_failures_ = 0;" in auth_branch
    assert "claim_substate_ = TbotClaimSubstate::AvailableStandby;" in auth_branch
    nvs_clear = auth_branch.index('websocket_settings.SetString("bootstrap_token", "");')
    ram_clear = auth_branch.index("Blufi::GetInstance().ClearProvisioningSecrets();")
    reopen_ble = auth_branch.index("ClaimBleLifecycleIntent::kEnsureAdvertising")
    assert nvs_clear < ram_clear < reopen_ble
    assert "ClaimBleLifecycleIntent::kEnsureAdvertising" in auth_branch
    assert "StopClaimPoll();" in auth_branch
    assert "StartClaimPoll();" not in auth_branch
    assert "return;" in auth_branch
    assert auth_branch_start < failure_copy_idx


def test_retryable_device_config_fetch_failure_retains_blufi_ram_secrets():
    source = read("main/application.cc")
    apply_body = function_body(source, "void Application::ApplyPendingTbotClaimFetchResult")
    retry_start = apply_body.index("if (!fetched || !pending_claim.active)")
    active_claim_start = apply_body.index("// Fetch succeeded with an active claim", retry_start)
    retry_body = apply_body[retry_start:active_claim_start]
    failure_counter_body = retry_body[
        retry_body.index("if (!fetched)"):retry_body.index("static constexpr int kClaimFetchFailureCopyThreshold")
    ]

    assert "claim_fetch_failures_" in retry_body
    assert "StartClaimPoll();" in retry_body
    assert "ClearProvisioningSecrets" not in failure_counter_body
    assert 'EraseKey("claim_device_id")' not in failure_counter_body


def test_claim_token_copies_are_zeroized_across_fetch_worker_handoffs():
    source = read("main/application.cc")
    dispatch_body = function_body(source, "bool Application::DispatchPendingTbotClaimFetch")
    worker_body = function_body(source, "void Application::ClaimFetchTask")
    helper_sig = "inline void SecureClearString" if "inline void SecureClearString" in source else "static void SecureClearString"
    helper = function_body(source, helper_sig)
    assert "volatile char*" in helper
    assert "value.clear();" in helper
    assert "SecureClearString(ctx->token);" in dispatch_body
    assert "SecureClearString(ctx->token);" in worker_body
    assert "mutable" in worker_body
    assert worker_body.count("SecureClearString(token);") >= 3
    early_return = worker_body[worker_body.index("if (!self->claim_poll_active_") :]
    assert early_return.index("SecureClearString(token);") < early_return.index("return;")
    assert "pending_tbot_claim_token_.clear();" not in source
    for assignment in (
        "pending_tbot_claim_token_ = token;",
    ):
        search_from = 0
        while True:
            assign_at = source.find(assignment, search_from)
            if assign_at == -1:
                break
            prefix = source[max(0, assign_at - 100):assign_at]
            assert "SecureClearString(pending_tbot_claim_token_);" in prefix
            search_from = assign_at + len(assignment)


def test_refresh_pending_claim_zeroizes_its_local_bootstrap_token_on_every_exit():
    source = read("main/application.cc")
    refresh_body = function_body(source, "void Application::RefreshPendingTbotClaim")

    assert "class SecureStringScope" in source
    assert "SecureStringScope token_scope(token);" in refresh_body
    assert "~SecureStringScope()" in source
    assert "SecureClearString(value_);" in source

def test_retryable_auto_confirm_keeps_cached_pending_claim_and_token():
    source = read("main/application.cc")
    confirm_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    retry_start = confirm_body.index("ClaimConfirmationResult::RetryableFailure")
    terminal_start = confirm_body.index("ClaimConfirmationResult::TerminalFailure", retry_start)
    retry_body = confirm_body[retry_start:terminal_start]

    assert "pending_tbot_claim_token_.clear()" not in retry_body
    assert 'SetString("bootstrap_token", "")' not in retry_body
    assert "StartClaimPoll();" in retry_body


def claim_activation_flow(source):
    requested = function_body(source, "bool Application::FinishClaimActivationAfterLocalAssetsReady")
    completion = function_body(source, "void Application::CompleteProtocolActivation")
    assert requested.index("claim_protocol_completion_pending_ = true;") < requested.index("ReloadProtocolAfterClaimCredentials();")
    assert "if (claim_protocol_completion_pending_)" in completion
    assert "CompleteClaimProtocolActivation();" in completion
    return requested + function_body(source, "void Application::CompleteClaimProtocolActivation")


def test_same_session_claim_applies_local_assets_before_starting_audio_or_wake():
    header = read("main/application.h")
    source = read("main/application.cc")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = claim_activation_flow(source)

    assert "bool EnsureLocalAssetsAppliedForClaim();" in header
    assert "if (!FinishClaimActivationAfterLocalAssetsReady())" in result_body
    ready_idx = finish_body.index("if (!EnsureLocalAssetsAppliedForClaim())")
    start_idx = finish_body.index("if (!audio_service_.Start())")
    idle_idx = finish_body.index("SetDeviceState(kDeviceStateIdle);")
    wake_idx = finish_body.index("audio_service_.EnableWakeWordDetection(true);")

    assert ready_idx < start_idx < idle_idx < wake_idx

    start_failure = function_body(finish_body, "if (!audio_service_.Start())")
    assert "ScheduleClaimLocalAssetsRetry();" in start_failure
    assert "return;" in start_failure
    gated_body = finish_body[start_idx:finish_body.index("StartHeartbeat();", start_idx)]
    assert "SetDeviceState(kDeviceStateIdle);" in gated_body
    assert "if (!audio_service_.Start())" in gated_body
    assert "audio_service_.EnableWakeWordDetection(true);" in gated_body

def test_same_session_claim_asset_failure_is_recoverable_and_does_not_show_success():
    header = read("main/application.h")
    source = read("main/application.cc")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = claim_activation_flow(source)
    retry_body = function_body(source, "void Application::ScheduleClaimLocalAssetsRetry")
    retry_tick = function_body(source, "void Application::HandleClaimLocalAssetsRetry")

    assert "esp_timer_handle_t claim_assets_retry_timer_" in header
    assert "void ScheduleClaimLocalAssetsRetry();" in header
    assert "bool FinishClaimActivationAfterLocalAssetsReady();" in header
    assert "void HandleClaimLocalAssetsRetry();" in header
    assert "if (!FinishClaimActivationAfterLocalAssetsReady())" in result_body
    failure_start = result_body.index("if (!FinishClaimActivationAfterLocalAssetsReady())")
    failure_body = result_body[failure_start:result_body.index("return true;", failure_start)]
    assert "ScheduleClaimLocalAssetsRetry();" in failure_body
    assert "Alert(Lang::Strings::TBOT_CONNECT, Lang::Strings::SERVER_UNAVAILABLE_RETRYING" in failure_body
    assert "Lang::Strings::CONNECTED" not in failure_body
    assert "ConfirmPendingTbotClaim" not in retry_body + retry_tick
    assert "ClaimConfirmationReporter::Confirm" not in retry_body + retry_tick
    assert "FinishClaimActivationAfterLocalAssetsReady()" in retry_tick
    assert "ScheduleClaimLocalAssetsRetry();" in retry_tick
    assert "Lang::Strings::CONNECTED" in finish_body
    assert finish_body.index("SetDeviceState(kDeviceStateIdle);") < finish_body.index(
        "audio_service_.EnableWakeWordDetection(true);"
    )

def test_claim_confirm_defers_protocol_reload_until_local_assets_are_ready():
    header = read("main/application.h")
    source = read("main/application.cc")
    websocket = read("main/protocols/websocket_protocol.cc")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = claim_activation_flow(source)
    reload_body = function_body(source, "void Application::ReloadProtocolAfterClaimCredentials")
    open_body = function_body(websocket, "bool WebsocketProtocol::OpenAudioChannel")

    assert "void ReloadProtocolAfterClaimCredentials();" in header
    assert "ReloadProtocolAfterClaimCredentials();" not in result_body
    assert "ReloadProtocolAfterClaimCredentials();" in finish_body
    clear_idx = result_body.index('websocket_settings.SetString("bootstrap_token", "")')
    finish_idx = result_body.index("if (!FinishClaimActivationAfterLocalAssetsReady())")
    assert clear_idx < finish_idx
    assets_idx = finish_body.index("if (!EnsureLocalAssetsAppliedForClaim())")
    reload_idx = finish_body.index("ReloadProtocolAfterClaimCredentials();")
    idle_idx = finish_body.index("SetDeviceState(kDeviceStateIdle);")
    assert assets_idx < reload_idx < idle_idx
    assert "CloseAudioChannelByIntent();" in reload_body
    assert "RequestInitializeProtocol();" in reload_body
    request = function_body(source, "void Application::RequestInitializeProtocol")
    assert "reset_pending_.store(true);" in request
    assert "CompletePendingProtocolWork();" in request
    assert "RefreshSettings();" in open_body
    assert open_body.index("RefreshSettings();") < open_body.index("std::string url = url_;")

def test_claim_local_asset_failure_does_not_reload_or_start_claimed_passive_until_retry_success():
    source = read("main/application.cc")
    result_body = function_body(
        source, "bool Application::ApplyPendingTbotClaimConfirmationResult"
    )
    finish_body = claim_activation_flow(source)
    retry_tick = function_body(source, "void Application::HandleClaimLocalAssetsRetry")
    initialize = function_body(source, "void Application::InitializeProtocol")

    failure_start = result_body.index("if (!FinishClaimActivationAfterLocalAssetsReady())")
    failure_body = result_body[failure_start:result_body.index("return true;", failure_start)]
    assert "ReloadProtocolAfterClaimCredentials();" not in failure_body
    assert "StartPassiveLessonWebsocket();" not in failure_body
    assert "audio_service_.EnableWakeWordDetection(true);" not in failure_body

    assets_idx = finish_body.index("if (!EnsureLocalAssetsAppliedForClaim())")
    failure_return_idx = finish_body.index("return false;", assets_idx)
    reload_idx = finish_body.index("ReloadProtocolAfterClaimCredentials();", failure_return_idx)
    audio_idx = finish_body.index("if (!audio_service_.Start())", reload_idx)
    idle_idx = finish_body.index("SetDeviceState(kDeviceStateIdle);", audio_idx)
    wake_idx = finish_body.index("audio_service_.EnableWakeWordDetection(true);", idle_idx)
    success_idx = finish_body.index("Lang::Strings::CONNECTED", wake_idx)

    assert assets_idx < failure_return_idx < reload_idx < audio_idx < idle_idx < wake_idx < success_idx
    assert "FinishClaimActivationAfterLocalAssetsReady()" in retry_tick
    assert "ReloadProtocolAfterClaimCredentials();" not in retry_tick
    assert "if (IsDeviceClaimed())" in initialize
    claimed_passive = initialize[initialize.index("if (IsDeviceClaimed())") :]
    assert "StartPassiveLessonWebsocket();" in claimed_passive

def test_same_session_claim_asset_apply_is_local_only_and_does_not_download():
    source = read("main/application.cc")
    helper = function_body(source, "bool Application::EnsureLocalAssetsAppliedForClaim")

    assert "Assets::GetInstance()" in helper
    assert "assets.partition_valid()" in helper
    assert "assets.Apply(false)" in helper

    for forbidden in (
        "CheckAssetsVersion",
        "CheckNewVersion",
        "Download",
        "download_url",
        "StartHeartbeat",
        "DispatchDeviceHeartbeat",
        "audio_service_.Start",
        "EnableWakeWordDetection",
    ):
        assert forbidden not in helper


def test_claimed_robot_prefers_provisioned_websocket_over_stale_ota_mqtt():
    source = read("main/application.cc")
    initialize = function_body(source, "void Application::InitializeProtocol")

    claimed_websocket = initialize.index("const bool prefer_claimed_websocket =")
    mqtt_fallback = initialize.index("ota_->HasMqttConfig()")
    websocket_build = initialize.index("std::make_unique<WebsocketProtocol>()")

    # Provisioning persists the management ws_url while the OTA object can still
    # carry a legacy MQTT section. Remote-unpair requires the claimed robot to
    # select the provisioned WebSocket before considering that stale fallback.
    assert claimed_websocket < mqtt_fallback < websocket_build
    preference = initialize[claimed_websocket:mqtt_fallback]
    assert "IsDeviceClaimed()" in preference
    assert "has_configured_websocket_url" in preference
    mqtt_branch = initialize[mqtt_fallback:websocket_build]
    assert "!prefer_claimed_websocket" in mqtt_branch


def test_mqtt_is_not_displaced_by_compile_time_websocket_fallback():
    source = read("main/application.cc")
    initialize = function_body(source, "void Application::InitializeProtocol")

    # Only an explicitly persisted management URL proves provisioning/config
    # fetch selected WebSocket. CONFIG_WEBSOCKET_URL remains a construction
    # fallback and must not override a valid OTA MQTT section by itself.
    assert 'websocket_settings.GetString("url", "")' in initialize
    preference_end = initialize.index("const bool prefer_claimed_websocket")
    preference_setup = initialize[:preference_end]
    assert 'GetString("url", CONFIG_WEBSOCKET_URL)' not in preference_setup

def test_app_startup_registers_allocation_failure_diagnostics_without_allocating_in_hook():
    source = read("main/main.cc")
    hook = function_body(source, "static void AllocationFailureHook")
    normal_main = 'extern "C" void app_main(void)' + source.rsplit(
        'extern "C" void app_main(void)', 1
    )[1]
    app_main = function_body(normal_main, 'extern "C" void app_main(void)')

    assert "#include <esp_heap_caps.h>" in source
    assert "#include <esp_rom_sys.h>" in source
    assert "size_t requested_size" in source
    assert "uint32_t caps" in source
    assert "const char* function_name" in source
    assert "esp_rom_printf" in hook
    assert "requested_size" in hook
    assert "caps" in hook
    assert "function_name" in hook
    for forbidden in ("malloc(", "calloc(", "realloc(", "new ", "std::", "ESP_LOG"):
        assert forbidden not in hook
    for secret in ("ssid", "password", "token", "serial", "mac"):
        assert secret not in hook.lower()

    register_idx = app_main.index("heap_caps_register_failed_alloc_callback(AllocationFailureHook)")
    nvs_idx = app_main.index("nvs_flash_init()")
    assert register_idx < nvs_idx
