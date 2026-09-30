#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "app_manager.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <new>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif
#ifdef CONFIG_TBOT_PROVISIONING_REPORT_ENABLED
#include "provisioning_status_reporter.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &connect_timer_));

    esp_timer_create_args_t retry_timer_args = {
        .callback = OnWifiConfigIntentRetry,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_config_retry",
        .skip_unhandled_events = true
    };
    ESP_ERROR_CHECK(esp_timer_create(
        &retry_timer_args, &wifi_config_retry_timer_));
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
    if (wifi_config_retry_timer_) {
        esp_timer_stop(wifi_config_retry_timer_);
        esp_timer_delete(wifi_config_retry_timer_);
    }
    if (ap_setup_timer_) {
        esp_timer_stop(ap_setup_timer_);
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "TBot";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    if (in_config_mode_) {
        ESP_LOGI(TAG, "StartNetwork skipped auto-connect because config mode is already active");
        return;
    }

    // Try to connect or enter config mode
    // --- Hardcoded WiFi (dev only) ---
    {
        auto result = SsidManager::GetInstance().AddSsid("TP-Link_5BC8", "33714592");
        if (result != SsidMutationResult::kApplied) {
            ESP_LOGW(TAG, "Hardcoded SSID not applied (%d)", static_cast<int>(result));
        }
    }

    TryWifiConnect();
}

void WifiBoard::EnsureWifiRecoveryTimeout() {
    wifi_recovery_timer_gate_.RunArmTransaction(
        [this]() {
            if (in_config_mode_ || WifiManager::GetInstance().IsConnected()) {
                return false;
            }
            if (esp_timer_is_active(connect_timer_)) {
                ESP_LOGI(TAG, "WiFi recovery timeout already armed; retaining deadline");
                return false;
            }
            return true;
        },
        [this]() {
            const int64_t deadline_us =
                esp_timer_get_time() + CONNECT_TIMEOUT_SEC * 1000000LL;
            wifi_recovery_deadline_us_.store(deadline_us, std::memory_order_release);
            const esp_err_t timer_error =
                esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
            if (timer_error != ESP_OK) {
                wifi_recovery_deadline_us_.store(0, std::memory_order_release);
                ESP_LOGE(TAG, "Failed to arm WiFi recovery timeout: %s",
                         esp_err_to_name(timer_error));
                return;
            }
            ESP_LOGI(TAG, "WiFi recovery timeout armed for %ds", CONNECT_TIMEOUT_SEC);
        });
}

WifiStationStartResult WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        auto& app = Application::GetInstance();
        app.EnsureBleAdvertisingForUnclaimedSavedWifi();
        EnsureWifiRecoveryTimeout();

        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        const auto start_result =
            WifiManager::GetInstance().StartStationIfScanIdle();
        if (start_result == WifiStationStartResult::kBusyOrFailed) {
            ESP_LOGW(TAG, "WiFi station start busy; recovery deadline remains armed");
        } else if (start_result == WifiStationStartResult::kAlreadyActive) {
            ESP_LOGI(TAG, "WiFi station already active; recovery deadline remains armed");
        }
        return start_result;
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        RequestWifiConfigMode();
        return WifiStationStartResult::kStartedNow;
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    std::lock_guard<std::recursive_mutex> event_lock(network_event_mutex_);
    const uint64_t event_sequence = ++network_event_sequence_;
    switch (event) {
        case NetworkEvent::Connected: {
            // Invalidate both the timer and any conditional entry it already queued.
            bool ignore_connected = false;
            wifi_recovery_timer_gate_.RunInvalidationTransaction(
                [this, &data, event_sequence, &ignore_connected]() {
                if (wifi_config_station_teardown_pending_) {
                    wifi_config_deferred_connected_ = true;
                    wifi_config_deferred_connected_data_ = data;
                    wifi_config_deferred_connected_sequence_ = event_sequence;
                    ignore_connected = true;
                    return;
                }
                wifi_recovery_deadline_us_.store(0, std::memory_order_release);
                wifi_recovery_generation_.fetch_add(1, std::memory_order_acq_rel);
                esp_timer_stop(connect_timer_);
                });
            if (ignore_connected) {
                ESP_LOGI(TAG, "Stale station connection ignored during config entry");
                return;
            }
            // Provisioning succeeded → AP (if it was open) must stop. Cancel the
            // AP hard-timeout so a stale callback cannot post a redundant
            // StopConfigAp after we have already moved on.
            CancelApSetupTimeout();
            // BluFi success workers carry their originating provisioning token
            // and exclusively own BLE teardown/rearm. A generic Connected event
            // cannot safely identify which provisioning attempt produced it.
            in_config_mode_ = false;
            ESP_LOGI(TAG, "WiFi connected");
            break;
        }
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connection attempt started");
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            EnsureWifiRecoveryTimeout();
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
            // A deferred lifecycle transition is not an open AP yet. Start the
            // hard timeout only after the manager confirms the AP is active.
            StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);
#endif
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Leaving config mode (credentials received) is a successful exit of
            // AP setup — cancel the hard-timeout before attempting connection.
            CancelApSetupTimeout();
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");
    const uint32_t recovery_generation =
        board->wifi_recovery_generation_.load(std::memory_order_acquire);
    const int64_t recovery_deadline_us =
        board->wifi_recovery_deadline_us_.load(std::memory_order_acquire);
    if (recovery_deadline_us <= 0 || esp_timer_get_time() < recovery_deadline_us) {
        ESP_LOGI(TAG, "WiFi recovery timeout ignored because its deadline is stale");
        return;
    }
    if (board->in_config_mode_) {
        ESP_LOGI(TAG, "WiFi connection timeout ignored because config mode is active");
        return;
    }
    if (WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "WiFi recovery timeout ignored because station recovered");
        return;
    }
    if (Application::GetInstance().IsLessonRuntimeActive()) {
        ESP_LOGI(TAG, "WiFi connection timeout deferred during lesson");
        board->ArmWifiRecoveryRetry(recovery_generation);
        return;
    }

    board->RequestWifiConfigMode(false, true, recovery_generation);
}

// ---------------------------------------------------------------------------
// AP-setup hard-timeout safety gate (mirrors the BLE gate in blufi.cpp).
// SoftAP must not run forever: §"AP must not run forever" / "AP setup timeout
// default: 10 minutes". The timer callback runs in the esp_timer task; tearing
// down the AP there would race the Wi-Fi/event tasks, so we only latch the flag
// and post StopConfigAp() to the Application task.
// ---------------------------------------------------------------------------

void WifiBoard::OnApSetupTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "AP setup TIMEOUT -> StopConfigAp posted to Application task");
    board->ap_timed_out_ = true;

    Application::GetInstance().Schedule([board]() {
        ESP_LOGW(TAG, "AP setup TIMEOUT teardown executing on Application task");
        if (Application::GetInstance().IsLessonRuntimeActive()) {
            ESP_LOGI(TAG, "AP setup TIMEOUT teardown deferred during lesson");
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
            board->StartApSetupTimeout(CONFIG_AP_SETUP_TIMEOUT_SEC);
#endif
            return;
        }
        WifiManager::GetInstance().StopConfigAp();
        board->in_config_mode_ = false;
    });
}

void WifiBoard::StartApSetupTimeout(int seconds) {
    if (ap_setup_timer_ != nullptr) {
        // Already armed — stop and delete so we can re-create cleanly.
        esp_timer_stop(ap_setup_timer_);
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
    }
    ap_timed_out_ = false;

    esp_timer_create_args_t args = {
        .callback = OnApSetupTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ap_setup_timer",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &ap_setup_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create AP setup timer: %s", esp_err_to_name(err));
        ap_setup_timer_ = nullptr;
        return;
    }
    err = esp_timer_start_once(ap_setup_timer_, static_cast<uint64_t>(seconds) * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP setup timer: %s", esp_err_to_name(err));
        esp_timer_delete(ap_setup_timer_);
        ap_setup_timer_ = nullptr;
        return;
    }
    ESP_LOGI(TAG, "AP setup timer armed %ds", seconds);
}

void WifiBoard::CancelApSetupTimeout() {
    if (ap_setup_timer_ == nullptr) {
        return;  // Never armed or already cancelled — no-op.
    }
    esp_timer_stop(ap_setup_timer_);
    esp_timer_delete(ap_setup_timer_);
    ap_setup_timer_ = nullptr;
    ESP_LOGI(TAG, "AP setup cancelled (provisioned)");
}

const char* WifiBoard::GetApStateString() const {
    // AP timeout tears the SoftAP down, so the backend-safe radio state is off.
    // The explicit AP_SETUP_TIMEOUT connection state carries the timeout detail.
    if (in_config_mode_ && WifiManager::GetInstance().IsConfigMode()) {
        return "active";
    }
    return "off";
}

void WifiBoard::RequestWifiConfigMode(bool show_notification,
                                      bool require_disconnected,
                                      uint32_t recovery_generation) {
    uint32_t requested_flags = require_disconnected
        ? kWifiConfigIntentConditional
        : kWifiConfigIntentExplicit;
    if (show_notification) {
        requested_flags |= kWifiConfigIntentNotify;
    }
    wifi_config_entry_intent_.Request(requested_flags, recovery_generation);
    ScheduleWifiConfigIntentDrain();
}

void WifiBoard::ScheduleWifiConfigIntentDrain() {
    bool expected = false;
    if (!wifi_config_entry_pending_.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "WiFi config request coalesced while entry is pending");
        return;
    }
    Application::GetInstance().Schedule([this]() {
        const auto snapshot = wifi_config_entry_intent_.Snapshot();
        const uint32_t request_generation = snapshot.request_generation;
        const uint32_t flags = snapshot.flags;
        const bool explicit_entry = (flags & kWifiConfigIntentExplicit) != 0;
        const bool conditional_entry =
            !explicit_entry && (flags & kWifiConfigIntentConditional) != 0;
        const uint32_t recovery_generation = snapshot.recovery_generation;
        WifiConfigEntryResult result = WifiConfigEntryResult::kCancelled;
        if (flags != 0) {
            result = StartWifiConfigMode(
                (flags & kWifiConfigIntentNotify) != 0,
                conditional_entry,
                recovery_generation);
        }

        if (result == WifiConfigEntryResult::kStarted) {
            esp_timer_stop(wifi_config_retry_timer_);
            wifi_config_entry_intent_.ConsumeAll();
        } else if (result == WifiConfigEntryResult::kCancelled) {
            esp_timer_stop(wifi_config_retry_timer_);
            wifi_config_entry_intent_.ConsumeIfCurrent(
                request_generation, kWifiConfigIntentConditional);
        } else {
            ArmWifiConfigIntentRetry();
        }
        wifi_config_entry_pending_.store(false);

        const auto current = wifi_config_entry_intent_.Snapshot();
        const uint32_t remaining = current.flags;
        const bool newer_request =
            current.request_generation != request_generation;
        if (remaining != 0 &&
            (result != WifiConfigEntryResult::kRetry || newer_request)) {
            ScheduleWifiConfigIntentDrain();
        }
    });
}

void WifiBoard::ArmWifiRecoveryRetry(uint32_t recovery_generation) {
    wifi_recovery_timer_gate_.RunArmTransaction(
        [this, recovery_generation]() {
            if (in_config_mode_ || esp_timer_is_active(connect_timer_)) {
                return false;
            }
            if (recovery_generation != wifi_recovery_generation_.load(
                    std::memory_order_acquire) ||
                WifiManager::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "WiFi recovery retry cancelled because station reconnected");
                return false;
            }
            return true;
        },
        [this]() {
            const int64_t deadline_us =
                esp_timer_get_time() + CONNECT_TIMEOUT_SEC * 1000000LL;
            wifi_recovery_deadline_us_.store(deadline_us, std::memory_order_release);
            const esp_err_t timer_error = esp_timer_start_once(
                connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
            if (timer_error != ESP_OK) {
                wifi_recovery_deadline_us_.store(0, std::memory_order_release);
                ESP_LOGE(TAG, "Failed to arm WiFi config intent retry: %s",
                         esp_err_to_name(timer_error));
            }
        });
}

void WifiBoard::OnWifiConfigIntentRetry(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    board->ScheduleWifiConfigIntentDrain();
}

void WifiBoard::ArmWifiConfigIntentRetry() {
    if (in_config_mode_ || esp_timer_is_active(wifi_config_retry_timer_)) {
        return;
    }
    const esp_err_t timer_error = esp_timer_start_once(
        wifi_config_retry_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
    if (timer_error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to arm WiFi config intent retry: %s",
                 esp_err_to_name(timer_error));
    }
}

WifiBoard::WifiConfigEntryResult WifiBoard::StartWifiConfigMode(
        bool show_notification,
        bool require_disconnected,
        uint32_t recovery_generation) {
    if (in_config_mode_ && WifiManager::GetInstance().IsConfigMode()) {
        return WifiConfigEntryResult::kStarted;
    }
    if (require_disconnected &&
        (recovery_generation != wifi_recovery_generation_.load(std::memory_order_acquire) ||
         WifiManager::GetInstance().IsConnected()) &&
        !Application::GetInstance().IsWifiConfigEntryPending()) {
        ESP_LOGI(TAG, "WiFi recovery cancelled because station reconnected");
        return WifiConfigEntryResult::kCancelled;
    }
    if (Application::GetInstance().IsLessonRuntimeActive()) {
        ESP_LOGI(TAG, "StartWifiConfigMode deferred during lesson");
        ArmWifiConfigIntentRetry();
        return WifiConfigEntryResult::kRetry;
    }
    auto& app = Application::GetInstance();
    Application::WifiConfigEntryPreparation preparation;
    if (!app.PrepareWifiConfigEntry(preparation)) {
        ESP_LOGE(TAG, "WiFi config aborted: realtime preparation failed");
        return WifiConfigEntryResult::kRetry;
    }
    if (require_disconnected &&
        (recovery_generation != wifi_recovery_generation_.load(std::memory_order_acquire) ||
         WifiManager::GetInstance().IsConnected())) {
        app.RollbackWifiConfigEntry(preparation);
        return WifiConfigEntryResult::kCancelled;
    }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    auto& audio_service = app.GetAudioService();
    auto provisioning_reservation = blufi.TryReserveProvisioningSession();
    if (!provisioning_reservation) {
        ESP_LOGE(TAG, "WiFi config aborted: prior provisioning completion still active");
        app.RollbackWifiConfigEntry(preparation);
        return WifiConfigEntryResult::kRetry;
    }
    const auto begin_result = audio_service.BeginWifiProvisioning();
    if (!begin_result) {
        ESP_LOGE(TAG, "WiFi config aborted: wake-word shutdown did not quiesce");
        if (begin_result.rollback_complete) {
            app.RollbackWifiConfigEntry(preparation);
        }
        return WifiConfigEntryResult::kRetry;
    }
    const auto provisioning_token = begin_result.token;
    if (!provisioning_reservation.Commit(provisioning_token)) {
        ESP_LOGE(TAG, "WiFi config aborted: failed to bind provisioning token");
        if (!audio_service.EndWifiProvisioningAndRearm(provisioning_token)) {
            ESP_LOGE(TAG, "WiFi config abort could not rearm provisioning generation");
        } else {
            app.RollbackWifiConfigEntry(preparation);
        }
        return WifiConfigEntryResult::kRetry;
    }
    const esp_err_t blufi_restart_error = blufi.RestartForSetup();
    if (blufi_restart_error != ESP_OK) {
        ESP_LOGE(TAG, "WiFi config aborted: BLUFI restart failed: %s",
                 esp_err_to_name(blufi_restart_error));
        if (!blufi.AbortProvisioningSetup(provisioning_token)) {
            ESP_LOGE(TAG, "BLUFI restart rollback incomplete; provisioning remains fail-closed");
        } else if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "BLUFI restart rollback could not restore application state");
        }
        return WifiConfigEntryResult::kRetry;
    }
#endif

    if (require_disconnected &&
        (recovery_generation != wifi_recovery_generation_.load(std::memory_order_acquire) ||
         WifiManager::GetInstance().IsConnected())) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        if (!blufi.AbortProvisioningSetup(provisioning_token)) {
            ESP_LOGE(TAG, "WiFi recovery cancellation could not restore provisioning state");
            return WifiConfigEntryResult::kRetry;
        }
#endif
        if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "WiFi recovery cancellation could not restore application state");
            return WifiConfigEntryResult::kRetry;
        }
        ESP_LOGI(TAG, "WiFi recovery cancelled at commit because station reconnected");
        return WifiConfigEntryResult::kCancelled;
    }
    bool config_entry_claimed = false;
    wifi_recovery_timer_gate_.RunArmTransaction(
        [this, require_disconnected, recovery_generation]() {
            if (in_config_mode_) {
                return false;
            }
            return !require_disconnected ||
                (recovery_generation == wifi_recovery_generation_.load(
                     std::memory_order_acquire) &&
                 !WifiManager::GetInstance().IsConnected());
        },
        [this, &config_entry_claimed]() {
            wifi_recovery_deadline_us_.store(0, std::memory_order_release);
            esp_timer_stop(connect_timer_);
            in_config_mode_ = true;
            wifi_config_station_teardown_pending_ = true;
            wifi_config_deferred_connected_ = false;
            wifi_config_deferred_connected_data_.clear();
            wifi_config_deferred_connected_sequence_ = 0;
            config_entry_claimed = true;
        });
    if (!config_entry_claimed) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        if (!blufi.AbortProvisioningSetup(provisioning_token)) {
            ESP_LOGE(TAG, "WiFi recovery commit cancellation could not restore provisioning state");
            return WifiConfigEntryResult::kRetry;
        }
#endif
        if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "WiFi recovery commit cancellation could not restore application state");
            return WifiConfigEntryResult::kRetry;
        }
        ESP_LOGI(TAG, "WiFi recovery cancelled immediately before station teardown");
        return WifiConfigEntryResult::kCancelled;
    }
    if (!WifiManager::GetInstance().TryStopStation()) {
        bool replay_connected = false;
        std::string replay_connected_data;
        uint64_t replay_connected_sequence = 0;
        wifi_recovery_timer_gate_.RunInvalidationTransaction([this, &replay_connected,
                                                               &replay_connected_data,
                                                               &replay_connected_sequence]() {
            wifi_config_station_teardown_pending_ = false;
            in_config_mode_ = false;
            replay_connected = wifi_config_deferred_connected_;
            replay_connected_data = wifi_config_deferred_connected_data_;
            replay_connected_sequence = wifi_config_deferred_connected_sequence_;
            wifi_config_deferred_connected_ = false;
            wifi_config_deferred_connected_data_.clear();
            wifi_config_deferred_connected_sequence_ = 0;
        });
        if (replay_connected) {
            std::lock_guard<std::recursive_mutex> event_lock(network_event_mutex_);
            if (network_event_sequence_ == replay_connected_sequence) {
                OnNetworkEvent(NetworkEvent::Connected, replay_connected_data);
            }
        }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        if (!blufi.AbortProvisioningSetup(provisioning_token)) {
            ESP_LOGE(TAG, "WiFi station teardown rollback could not restore provisioning state");
            return WifiConfigEntryResult::kRetry;
        }
#endif
        if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "WiFi station teardown rollback could not restore application state");
        }
        ESP_LOGW(TAG, "WiFi station teardown busy; retaining config intent for retry");
        return WifiConfigEntryResult::kRetry;
    }
    wifi_recovery_timer_gate_.RunInvalidationTransaction([this]() {
        wifi_config_station_teardown_pending_ = false;
        wifi_config_deferred_connected_ = false;
        wifi_config_deferred_connected_data_.clear();
        wifi_config_deferred_connected_sequence_ = 0;
    });

#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    if (!wifi_manager.TryStartConfigAp()) {
        in_config_mode_ = false;
        if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "WiFi config AP start rollback could not restore application state");
        }
        ESP_LOGW(TAG, "WiFi config AP start busy; retaining config intent for retry");
        return WifiConfigEntryResult::kRetry;
    }
#endif

    if (!app.PublishWifiConfigEntry(preparation)) {
        wifi_recovery_timer_gate_.RunInvalidationTransaction([this]() {
            in_config_mode_ = false;
        });
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        wifi_manager.StopConfigAp();
#endif
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        if (!blufi.AbortProvisioningSetup(provisioning_token)) {
            ESP_LOGE(TAG, "WiFi config publication rollback remains fail-closed");
            return WifiConfigEntryResult::kRetry;
        }
#endif
        if (!app.RollbackWifiConfigEntry(preparation)) {
            ESP_LOGE(TAG, "WiFi config publication rollback could not restore application state");
        }
        return WifiConfigEntryResult::kRetry;
    }
    AppExitToChatboxForSystemFlow();

    if (show_notification) {
        GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    }

#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    // Arm the hard-timeout safety gate immediately after init so that BLE
    // advertising cannot run forever if provisioning never completes.
    // Teardown is posted to the Application task inside the timer callback
    // (never in callback context) to avoid WDT/race conditions (§9). Re-arming
    // is safe even if BLE was already up (StartBleSetupTimeout recreates the
    // timer cleanly), and it correctly switches the timeout to this explicit
    // setup window.
    blufi.StartBleSetupTimeout(CONFIG_BLE_SETUP_TIMEOUT_SEC);
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
    return WifiConfigEntryResult::kStarted;
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    RequestWifiConfigMode(true);
}

void WifiBoard::ResumePendingWifiConfigMode() {
    if (wifi_config_entry_intent_.LoadFlags() != 0) {
        ScheduleWifiConfigIntentDrain();
    }
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddNumberToObject(network, "rssi", rssi);
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    cJSON_AddStringToObject(root, "ble_state", Blufi::GetInstance().GetBleStateString());
#else
    cJSON_AddStringToObject(root, "ble_state", "off");
#endif
    cJSON_AddStringToObject(root, "ap_state", GetApStateString());

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
