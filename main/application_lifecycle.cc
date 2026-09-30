#include "application_internal.h"

DRAM_ATTR StaticTask_t open_channel_task_buffer;
DRAM_ATTR StackType_t open_channel_task_stack[kOpenChannelWorkerStackDepth];
DRAM_ATTR StaticQueue_t open_channel_queue_buffer;
DRAM_ATTR NetworkWorkItem open_channel_queue_storage[2];
QueueHandle_t open_channel_queue = nullptr;
TaskHandle_t open_channel_task = nullptr;
StaticTask_t chat_outbound_task_buffer;
EXT_RAM_BSS_ATTR StackType_t chat_outbound_task_stack[kChatOutboundWorkerStackDepth];
StaticTask_t chat_audio_cleanup_task_buffer;
EXT_RAM_BSS_ATTR StackType_t chat_audio_cleanup_task_stack[kChatAudioCleanupWorkerStackDepth];

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
DRAM_ATTR StaticTask_t lesson_message_task_buffer;
DRAM_ATTR StaticQueue_t lesson_message_queue_buffer;
DRAM_ATTR StackType_t lesson_message_task_stack[kLessonMessageWorkerStackDepth];
uint8_t* lesson_message_queue_storage = nullptr;

void LogLessonWorkerStackWatermark(const char* stage) {
    const UBaseType_t free_stack_bytes = uxTaskGetStackHighWaterMark(nullptr);
    if (free_stack_bytes < kLessonMessageWorkerMinimumFreeStackBytes) {
        ESP_LOGE(TAG, "lesson_worker stack low stage=%s free=%u threshold=%u", stage,
                 static_cast<unsigned>(free_stack_bytes),
                 static_cast<unsigned>(kLessonMessageWorkerMinimumFreeStackBytes));
        return;
    }
    ESP_LOGI(TAG, "lesson_worker stack stage=%s free=%u", stage,
             static_cast<unsigned>(free_stack_bytes));
}
#endif

Application::Application() {
    event_group_ = xEventGroupCreate();
    if (!InitializeChatOutboundWorker()) {
        ESP_LOGE(TAG, "Failed to create persistent chat outbound worker");
    }

    open_channel_queue = xQueueCreateStatic(2, sizeof(NetworkWorkItem),
                                            reinterpret_cast<uint8_t*>(open_channel_queue_storage),
                                            &open_channel_queue_buffer);
    if (open_channel_queue != nullptr) {
        open_channel_task = xTaskCreateStatic(
            &Application::OpenChannelTask, "lesson_ws", kOpenChannelWorkerStackDepth, this,
            tskIDLE_PRIORITY + 3, open_channel_task_stack, &open_channel_task_buffer);
    }
    if (open_channel_queue == nullptr || open_channel_task == nullptr) {
        ESP_LOGE(TAG, "Failed to create persistent internal websocket worker");
    }

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    constexpr uint32_t kLessonWorkerMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    lesson_message_queue_storage = static_cast<uint8_t*>(heap_caps_malloc(
        (kLessonMessageQueueDepth + 1) * sizeof(LessonQueueItem), kLessonWorkerMemoryCaps));
    if (lesson_message_queue_storage != nullptr) {
        lesson_message_queue_ =
            xQueueCreateStatic(kLessonMessageQueueDepth + 1, sizeof(LessonQueueItem),
                               lesson_message_queue_storage, &lesson_message_queue_buffer);
    }
    if (lesson_message_queue_ != nullptr) {
        lesson_message_task_handle_ = xTaskCreateStatic(
            &Application::LessonMessageTask, "lesson_worker", kLessonMessageWorkerStackDepth, this,
            tskIDLE_PRIORITY + 2, lesson_message_task_stack, &lesson_message_task_buffer);
    }
    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        ESP_LOGE(TAG,
                 "Failed to create persistent lesson worker stack=%p storage=%p queue=%p task=%p",
                 lesson_message_task_stack, lesson_message_queue_storage, lesson_message_queue_,
                 lesson_message_task_handle_);
        if (lesson_message_task_handle_ != nullptr) {
            vTaskDelete(lesson_message_task_handle_);
            lesson_message_task_handle_ = nullptr;
        }
        lesson_message_queue_ = nullptr;
        heap_caps_free(lesson_message_queue_storage);
        lesson_message_queue_storage = nullptr;
    }
#endif

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    CancelLessonRobotEntranceOnDisplay();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (claim_poll_timer_ != nullptr) {
        esp_timer_stop(claim_poll_timer_);
        esp_timer_delete(claim_poll_timer_);
    }
    if (claim_expiry_timer_ != nullptr) {
        esp_timer_stop(claim_expiry_timer_);
        esp_timer_delete(claim_expiry_timer_);
    }
    if (claim_assets_retry_timer_ != nullptr) {
        esp_timer_stop(claim_assets_retry_timer_);
        esp_timer_delete(claim_assets_retry_timer_);
    }
    if (heartbeat_timer_ != nullptr) {
        esp_timer_stop(heartbeat_timer_);
        esp_timer_delete(heartbeat_timer_);
    }
    if (speaking_timeout_timer_ != nullptr) {
        esp_timer_stop(speaking_timeout_timer_);
        esp_timer_delete(speaking_timeout_timer_);
    }
    if (lesson_asset_sync_wake_rearm_timer_ != nullptr) {
        esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
        esp_timer_delete(lesson_asset_sync_wake_rearm_timer_);
    }
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    StopLessonMessageTask();
    if (lesson_message_queue_ != nullptr) {
        LessonQueueItem item;
        while (xQueueReceive(lesson_message_queue_, &item, 0) == pdTRUE) {
            delete static_cast<ChatRequestContext*>(item.source_context);
            if (item.kind == LessonQueueItemKind::kFrame && item.payload != nullptr) {
                cJSON_free(item.payload);
            }
        }
        lesson_message_queue_ = nullptr;
    }
    heap_caps_free(lesson_message_queue_storage);
    lesson_message_queue_storage = nullptr;
#endif
    vEventGroupDelete(event_group_);
}

void Application::Initialize() {
#if CONFIG_TBOT_HIL_STORAGE_FAULTS
    ESP_LOGW(TAG, "TBOT_HIL_STORAGE_FAULTS_ENABLED non-production-image");
#endif
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    std::string build_identity_error;
    if (!PreloadRunningEspBuildIdentity(&build_identity_error)) {
        ESP_LOGW(TAG, "Build identity preload failed reason=%s", build_identity_error.c_str());
    }

    auto display = board.GetDisplay();
    display->SetupUI();
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    if (IsDeviceClaimed()) {
        if (!audio_service_.Start()) {
            ESP_LOGE(TAG, "Claimed boot audio startup failed; initialization remains stopped");
            return;
        }
    } else {
        ESP_LOGI(TAG, "Unclaimed boot: deferring audio workers until claim confirmation");
    }
    robot_uart_.Initialize();
    if (xTaskCreate(
            [](void* context) {
                auto* self = static_cast<Application*>(context);
                while (true) {
                    if (self->GetDeviceState() == kDeviceStateSpeaking)
                        self->speaking_arm_dispatch_.Poll(
                            static_cast<uint64_t>(esp_timer_get_time() / 1000),
                            self->GetDeviceState() == kDeviceStateSpeaking &&
                                !self->lesson_runtime_active_.load(),
                            [self](const SpeakingArmGesture::Target& target, auto owns) {
                                return self->robot_uart_.TrySendAutomaticArm(
                                    target.left, target.percent, [self, owns]() {
                                        return owns() &&
                                               self->GetDeviceState() == kDeviceStateSpeaking &&
                                               !self->lesson_runtime_active_.load();
                                    });
                            });
                    vTaskDelay(pdMS_TO_TICKS(25));
                }
            },
            "speaking_arms", 3072, this, 1, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Speaking arm worker unavailable");
    }

    AppManagerInit();
    AppManagerSetSlaveSender([this](const char* line) { robot_uart_.SendControlLine(line); });
    AppManagerSetSoundPlayer(
        [this](const std::vector<int16_t>& pcm) { audio_service_.QueuePcmForPlayback(pcm); });
    robot_uart_.SetEventCallback([this](RobotInputEvent evt) {
        Schedule([evt]() {
            switch (evt) {
                case RobotInputEvent::LeftClick:
                    AppHandleInputLeft();
                    break;
                case RobotInputEvent::RightClick:
                    AppHandleInputRight();
                    break;
                case RobotInputEvent::BothClick:
                    AppHandleInputBothClick();
                    break;
                case RobotInputEvent::MenuHold:
                    AppHandleMenuHold();
                    break;
                case RobotInputEvent::RightHold:
                    AppHandleRightHold();
                    break;
                case RobotInputEvent::SlaveReady:
                    AppOnSlaveReady();
                    break;
            }
        });
    });

    AudioServiceCallbacks callbacks;
    callbacks.on_playback_failed = [this](uint32_t response) {
        lesson_audio_playout_.PublishFailure(response);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.on_output_completed = [this](uint32_t response, bool conversation, uint32_t now_ms) {
        speaking_arm_dispatch_.PublishOutput(response, conversation, now_ms);
        lesson_audio_playout_.PublishOutput(response, conversation,
                                            static_cast<uint64_t>(esp_timer_get_time() / 1000));
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        if (speaking) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            last_vad_speech_ms_ = now_ms;
            if (GetDeviceState() == kDeviceStateListening) {
                last_listening_activity_ms_.store(now_ms);
            }
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        if (new_state != kDeviceStateSpeaking)
            speaking_arm_dispatch_.Cancel();
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        const bool lesson_active = lesson_runtime_active_.load();
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                if (!lesson_active) {
                    display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (lesson_active) {
                    break;
                }
                if (data.empty()) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                if (!lesson_active) {
                    std::string msg = Lang::Strings::CONNECTED_TO;
                    msg += data;
                    display->ShowNotification(msg.c_str(), 30000);
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                break;
            case NetworkEvent::WifiConfigModeExit:
                break;
            case NetworkEvent::ModemDetecting:
                if (!lesson_active) {
                    display->SetStatus(Lang::Strings::DETECTING_MODULE);
                }
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                if (!lesson_active) {
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                }
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}
