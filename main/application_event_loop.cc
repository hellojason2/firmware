#include "application_internal.h"

void Application::Run() {
    application_task_ = xTaskGetCurrentTaskHandle();
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    // WDT-1: subscribe the main task to the Task Watchdog Timer. The loop below
    // feeds it on every (bounded) pass, so a genuine hang inside a handler stops
    // the feed and TWDT logs a backtrace. PANIC stays OFF (sdkconfig) -> detection
    // only, no reboot, to avoid false reboots before HIL tuning.
    if (esp_task_wdt_add(nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add(main) failed - TWDT not enabled?");
    }

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_CHAT_OUTBOUND;

    while (true) {
        // req#1: bounded wait (was portMAX_DELAY) so the loop always makes a pass,
        // feeds the watchdog, and never blocks forever.
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(lesson_playout_pending_.load() ? 20 : 2000));
        PollLessonAudioPlayout();
        PollChatOutboundEvents(bits & MAIN_EVENT_CHAT_OUTBOUND);
        esp_task_wdt_reset();  // WDT-1: prove the main loop is iterating

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            // WSS-8: while a connect cycle is still in progress (or a passive
            // lesson preconnect is running), a failed attempt is a RECOVERABLE
            // transient — the wake open-loop (3x) and ScheduleReconnect backoff
            // retry and usually land in Listening within a second or two over the
            // slow cold TLS/tunnel handshake. Flashing "Server unavailable.
            // Retrying..." on each attempt shows a scary error that immediately
            // self-clears. Keep the calm idle/connecting view; the banner is
            // surfaced once for wake-open exhaustion (and any non-connect error,
            // where neither flag is set, still alerts immediately). Listen-mode
            // reconnect keeps slow-period retrying for recovered endpoints.
            if (lesson_runtime_active_.load()) {
                ESP_LOGI(TAG, "lesson error suppressed: %s", last_error_message_.c_str());
                RequestLessonStorageAbandonment();
                lesson_interactive_listen_generation_.fetch_add(1);
                lesson_interactive_listen_pending_.store(false);
                lesson_interactive_listening_active_.store(false);
                auto display = Board::GetInstance().GetDisplay();
                display->SetStatus(Lang::Strings::PLEASE_WAIT);
            } else if (connect_attempt_active_.load() || passive_ws_intent_.load()) {
                ESP_LOGI(TAG,
                         "connect error suppressed (recoverable): attempt_active=%d passive=%d "
                         "in_flight=%d reconnect_attempt=%d",
                         connect_attempt_active_.load() ? 1 : 0, passive_ws_intent_.load() ? 1 : 0,
                         connect_in_flight_.load() ? 1 : 0, reconnect_attempt_);
            } else {
                // Resolve the connect state through the FSM mapper (backend_offline_
                // is set on the ws/backend error) so this render is mapper-driven,
                // not hand-coded. OFFLINE_RETRY -> localized "Server unavailable.
                // Retrying..."; any other resolved state keeps the generic ERROR
                // banner. Detailed error stays in the chat body + log.
                const TbotConnectState cs = TbotConnectMapper::ResolveState(
                    GetDeviceState(), claim_substate_, GetBleSubstate(), backend_offline_.load());
                const char* status = (cs == TbotConnectState::OFFLINE_RETRY)
                                         ? Lang::Strings::SERVER_UNAVAILABLE_RETRYING
                                         : Lang::Strings::ERROR;
                Alert(status, last_error_message_.c_str(), "circle_xmark",
                      Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            RunScheduledTasks();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            if (IsSelectedNormalChatRoute()) {
                NotifyChatOutbound();
            } else if (chat_lesson_capture_token_) {
                PollChatLessonCapture(static_cast<uint64_t>(esp_timer_get_time()));
            } else {
                static uint32_t send_event_count = 0;
                static uint32_t send_packet_count = 0;
                static uint32_t lesson_render_defer_count = 0;
                send_event_count++;
                if (!IsMicrophoneUplinkAuthorized()) {
                    audio_service_.EnableVoiceProcessing(false);
                    uint32_t dropped_packets = 0;
                    while (audio_service_.PopPacketFromSendQueue() != nullptr) {
                        ++dropped_packets;
                    }
                    if (dropped_packets > 0) {
                        ESP_LOGW(
                            TAG,
                            "microphone_uplink_blocked state=%d passive=%d online=%d dropped=%lu",
                            static_cast<int>(GetDeviceState()), passive_ws_intent_.load() ? 1 : 0,
                            online_intent_.load() ? 1 : 0,
                            static_cast<unsigned long>(dropped_packets));
                    }
                } else if (IsLessonNetworkRenderQuiet()) {
                    lesson_render_defer_count++;
                    if (lesson_render_defer_count == 1 || lesson_render_defer_count % 25 == 0) {
                        ESP_LOGI(TAG, "MAIN_EVENT_SEND_AUDIO deferred_for_lesson_render count=%lu",
                                 static_cast<unsigned long>(lesson_render_defer_count));
                    }
                    xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
                    vTaskDelay(pdMS_TO_TICKS(20));
                } else {
                    uint32_t sent_packets = 0;
                    while (sent_packets < kMaxAudioPacketsPerMainLoop) {
                        auto packet = audio_service_.PopPacketFromSendQueue();
                        if (!packet) {
                            break;
                        }
                        const uint32_t timestamp = packet->timestamp;
                        const size_t payload_size = packet->payload.size();
                        if (!protocol_) {
                            ESP_LOGW(TAG,
                                     "MAIN_EVENT_SEND_AUDIO protocol_unavailable event=%lu "
                                     "payload_bytes=%u timestamp=%lu",
                                     static_cast<unsigned long>(send_event_count),
                                     static_cast<unsigned>(payload_size),
                                     static_cast<unsigned long>(timestamp));
                            break;
                        }
                        bool sent = protocol_->SendAudio(std::move(packet));
                        if (!sent) {
                            ESP_LOGW(TAG,
                                     "MAIN_EVENT_SEND_AUDIO send_failed event=%lu payload_bytes=%u "
                                     "timestamp=%lu",
                                     static_cast<unsigned long>(send_event_count),
                                     static_cast<unsigned>(payload_size),
                                     static_cast<unsigned long>(timestamp));
                            break;
                        }
                        send_packet_count++;
                        if (send_packet_count == 1 || send_packet_count % 25 == 0) {
                            ESP_LOGI(TAG,
                                     "MAIN_EVENT_SEND_AUDIO packet count=%lu payload_bytes=%u "
                                     "timestamp=%lu",
                                     static_cast<unsigned long>(send_packet_count),
                                     static_cast<unsigned>(payload_size),
                                     static_cast<unsigned long>(timestamp));
                        }
                        ++sent_packets;
                        esp_task_wdt_reset();
                    }
                    if (sent_packets == kMaxAudioPacketsPerMainLoop) {
                        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                RunScheduledTasks();
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            HasLessonAssetSyncWakeOpportunity();
            if (protocol_start_pending_generation_ != 0 &&
                protocol_start_pending_generation_ == protocol_generation_.load() &&
                !protocol_work_lifetime_.Pending()) {
                StartProtocolWorker();
            }
            PollChatOutboundEvents(MAIN_EVENT_CLOCK_TICK);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            if (wifi_config_preparation_.valid) {
                static_cast<WifiBoard&>(Board::GetInstance()).ResumePendingWifiConfigMode();
            }
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            HandleListeningWatchdogTick();

            bool passive_liveness_failed = false;
            const DeviceState passive_state = GetDeviceState();
            const bool selected_chat_source =
                chat_protocol_signals_ && chat_protocol_signals_->SourceSelected();
            if (passive_ws_intent_.load() && IsDeviceClaimed() && protocol_ != nullptr &&
                !connect_in_flight_.load() &&
                !(selected_chat_source
                      ? protocol_work_lifetime_.BusyExcept(chat_outbound_reservation_)
                      : protocol_work_lifetime_.Busy()) &&
                !protocol_work_lifetime_.Pending() &&
                // Do NOT tear down the passive WS while a lesson SD asset sync is
                // in flight: hashing the ~116MB pack starves the WS receive task so
                // server pongs miss the 10s window, but the connection is fine and
                // the server keeps it open. Killing it here aborts the sync and
                // starts an endless reconnect/re-sync loop. The timer is reset in
                // EndLessonAssetSyncQuiet() so liveness resumes cleanly afterward.
                // (Short-circuits before MaintainPassiveLiveness so no ping/pong
                // state is mutated during the sync.)
                !IsLessonAssetSyncQuiet() && passive_state != kDeviceStateWifiConfiguring &&
                passive_state != kDeviceStateAudioTesting &&
                (selected_chat_source || protocol_->IsAudioChannelOpened()) &&
                !(selected_chat_source ? MaintainChatPassiveLiveness()
                                       : protocol_->MaintainPassiveLiveness())) {
                ESP_LOGW(TAG, "passive_lesson_ws_liveness_failed -> passive backoff");
                backend_offline_.store(true);
                if (chat_cleanup_enabled_) {
                    protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                    PollChatProtocolCleanup();
                } else {
                    protocol_->CloseAudioChannel();
                }
                SchedulePassiveLessonReconnect();
                passive_liveness_failed = true;
            }

            if (!passive_liveness_failed && !selected_chat_source && !reconnect_passive_.load() &&
                clock_ticks_ % 10 == 0 && IsDeviceClaimed() && !lesson_runtime_active_.load() &&
                GetDeviceState() == kDeviceStateIdle && protocol_ != nullptr &&
                !connect_in_flight_.load() && !protocol_->IsAudioChannelOpened()) {
                ESP_LOGW(TAG, "passive_lesson_idle_socket_missing -> passive reconnect");
                StartPassiveLessonWebsocket();
            }

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                SystemInfo::PrintTaskList();
                if (chat_cleanup_enabled_) {
                    PlaybackDrainSnapshot snapshot;
                    const bool available = audio_service_.TryGetPlaybackDrainSnapshot(snapshot);
                    ESP_LOGI(
                        TAG, "chat_metrics snapshot_available=%d source_selected=%d reconnects=%lu",
                        available ? 1 : 0,
                        chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() ? 1 : 0,
                        static_cast<unsigned long>(reconnect_count_.load()));
                } else {
                    LogPeriodicMetrics();
                }
            }
        }
    }
}

