#include "application_internal.h"

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down",
              Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();

    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    while (true) {
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < kOtaCheckMaxAttempts; ++attempt) {
            auto current_state = GetDeviceState();
            if (current_state == kDeviceStateWifiConfiguring ||
                current_state == kDeviceStateAudioTesting || current_state == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Skipping OTA version check because activation ended");
                return;
            }
            display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

            err = ota_->CheckVersion();
            if (err == ESP_OK) {
                break;
            }
            if (attempt + 1 >= kOtaCheckMaxAttempts) {
                char error_message[32];
                snprintf(error_message, sizeof(error_message), "code=%d", err);
                Alert(Lang::Strings::ERROR, error_message, "cloud_slash",
                      Lang::Sounds::OGG_EXCLAMATION);
                ESP_LOGE(TAG, "OTA version check exhausted its bounded retry budget, code=%d", err);
                return;
            }

            const int retry_delay = kOtaRetryDelaysSeconds[attempt];
            ESP_LOGW(TAG, "OTA version check failed; retry in %d seconds (%d/%d), code=%d",
                     retry_delay, attempt + 1, kOtaCheckMaxAttempts, err);
            for (int elapsed_seconds = 0; elapsed_seconds < retry_delay; ++elapsed_seconds) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                auto delayed_state = GetDeviceState();
                if (delayed_state == kDeviceStateWifiConfiguring ||
                    delayed_state == kDeviceStateAudioTesting) {
                    ESP_LOGI(TAG, "Aborting OTA retry because WiFi config mode is active");
                    return;
                }
                if (delayed_state == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "Aborting OTA retry because activation ended");
                    return;
                }
            }
        }

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::Reboot(ChatRequestContext context) {
    if (!IsChatRequestCurrent(context))
        return;
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson reboot ignored");
        return;
    }
    Schedule([this, context]() {
        if (!IsChatRequestCurrent(context))
            return;
        reboot_pending_.store(true);
        CloseAudioChannelByIntent();
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        CompletePendingProtocolWork();
    });
}

void Application::CompleteReboot() {
    RetireChatOutbound();
    if (chat_cleanup_enabled_) {
        reboot_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        PollChatProtocolCleanup();
        return;
    }
    if (protocol_work_lifetime_.Busy()) {
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReboot);
        return;
    }
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    CancelLessonRobotEntranceOnDisplay();
    protocol_.reset();
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::IsConnectSuccessPublicationSuppressed() const {
    return protocol_work_lifetime_.Pending() || connect_close_deferral_.Pending() ||
           reset_pending_.load() || reboot_pending_.load();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson firmware upgrade ignored");
        return false;
    }
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        CloseAudioChannelByIntent();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade");

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        if (!audio_service_.Start()) {
            ESP_LOGE(TAG, "Firmware upgrade rollback could not restart audio service");
        }
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}
