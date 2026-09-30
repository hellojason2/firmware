#include "application_internal.h"

void Application::DispatchIncomingJson(const cJSON* root, uint64_t callback_transport_epoch,
                                       bool is_websocket_protocol, ChatRequestContext context) {
    ChatRuntimeTiming timing(1, []() { return static_cast<uint64_t>(esp_timer_get_time()); },
        [](uint32_t site, uint32_t hi, uint32_t lo) {
            ESP_LOGW(TAG, "chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                     static_cast<unsigned>(site), static_cast<unsigned long>(hi),
                     static_cast<unsigned long>(lo));
        });
    if (!IsChatRequestCurrent(context))
        return;
    auto* display = Board::GetInstance().GetDisplay();
    // Parse JSON data
    auto type = cJSON_GetObjectItem(root, "type");
    // US-006 Slice-01 (DIV-FW-NULLDEREF): guard the type deref on the path the
    // additive lesson_ branch joins. A missing/non-string type would null-deref
    // type->valuestring below. Both transports already pre-guard this
    // (websocket_protocol.cc, mqtt_protocol.cc), so no valid frame changes
    // behavior — defense-in-depth on the shared dispatch path only.
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Missing or non-string message type, dropping frame");
        return;
    }
    if (lesson_asset_sync_quiet_.load() &&
        (strcmp(type->valuestring, "tts") == 0 || strcmp(type->valuestring, "stt") == 0)) {
        ESP_LOGI(TAG, "lesson asset sync quiet dropped voice frame type=%s", type->valuestring);
        return;
    }
        if (strcmp(type->valuestring, "tts") == 0) {
        if (!IsChatLessonRequestCurrent(context))
            return;
        if (HandleLessonPlayoutTts(root, context))
            return;
        auto state = cJSON_GetObjectItem(root, "state");
        // Guard the state deref: a tts frame with no "state" or a non-string
        // state null-derefs state->valuestring below (deep-audit #4 HIGH — a
        // malformed/MITM frame crashes the audio task). cJSON_IsString covers
        // both the missing-key (null node) and wrong-type cases.
        if (!cJSON_IsString(state)) {
            ESP_LOGW(TAG, "tts frame missing or non-string state; dropping");
            return;
        }
        if (strcmp(state->valuestring, "start") == 0) {
            audio_service_.ResetDecoder();
            if (GetDeviceState() == kDeviceStateListening &&
                listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                listening_started_ms_.store(0);
                last_listening_activity_ms_.store(0);
            }
            audio_service_.SetPlaybackGeneration(++speaking_generation_);
            speaking_arm_dispatch_.BeginResponse(speaking_generation_.load());
            tts_audio_accepting_.store(true);
            const auto started_generation = speaking_generation_.load();
            Schedule([this, context, started_generation]() {
                if (!IsChatLessonRequestCurrent(context) ||
                    started_generation != speaking_generation_.load())
                    return;
                aborted_ = false;
                auto current_generation = speaking_generation_.load();
                last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
                SetDeviceState(kDeviceStateSpeaking);
                ESP_LOGI(TAG, "tts_start_received generation=%lu",
                         (unsigned long)current_generation);
                ArmSpeakingTimeout();
            });
        } else if (strcmp(state->valuestring, "stop") == 0) {
            speaking_arm_dispatch_.Cancel();
            tts_audio_accepting_.store(false);
            int64_t t_recv = esp_timer_get_time() / 1000;
            const auto t_recv_sec = static_cast<unsigned long>(t_recv / 1000);
            const auto t_recv_ms = static_cast<unsigned long>(t_recv % 1000);
            ESP_LOGI(TAG, "tts_stop_received ts=%lu%03lu", t_recv_sec, t_recv_ms);
            auto reason = cJSON_GetObjectItem(root, "reason");
            bool is_interrupt =
                cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0;
            auto drain_id = cJSON_GetObjectItem(root, "drainId");
            std::string tts_drain_id;
            if (cJSON_IsString(drain_id) && strlen(drain_id->valuestring) <= 64) {
                tts_drain_id = drain_id->valuestring;
            }
            const std::uint64_t stopped_audio_generation =
                static_cast<std::uint64_t>(speaking_generation_.load()) + 1;
            if (is_interrupt) {
                audio_service_.SetPlaybackGeneration(++speaking_generation_);
                audio_service_.ResetDecoder();
                ESP_LOGI(TAG, "tts_stop_interrupt_flush ts=%lu%03lu", t_recv_sec, t_recv_ms);
            }
            auto continue_listening = cJSON_GetObjectItem(root, "continue_listening");
            bool force_continue_listening = cJSON_IsTrue(continue_listening);
            auto listen_mode = cJSON_GetObjectItem(root, "listen_mode");
            bool force_realtime_listen =
                cJSON_IsString(listen_mode) && strcmp(listen_mode->valuestring, "realtime") == 0;
            bool explicit_stop_listening =
                cJSON_IsBool(continue_listening) && !cJSON_IsTrue(continue_listening) &&
                cJSON_IsString(listen_mode) && strcmp(listen_mode->valuestring, "manual") == 0;
            const auto stop_callback_generation = speaking_generation_.load();
            Schedule([this, force_continue_listening, force_realtime_listen,
                      explicit_stop_listening, stopped_audio_generation, is_interrupt, tts_drain_id,
                      context, stop_callback_generation]() {
                if (!IsChatLessonRequestCurrent(context) ||
                    stop_callback_generation != speaking_generation_.load())
                    return;
                ++speaking_generation_;
                last_speaking_activity_ms_.store(0);
                if (!is_interrupt && !tts_drain_id.empty()) {
                    const bool playback_drained =
                        audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                    if (!IsChatLessonRequestCurrent(context))
                        return;
                    if (playback_drained) {
                        if (protocol_)
                            protocol_->SendTtsDrainAck(tts_drain_id);
                    } else {
                        ESP_LOGW(TAG,
                                 "tts_stop_playback_drain_timeout timeout_ms=%lu action=drain_ack",
                                 static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                    }
                }
                const bool lesson_interactive_turn = lesson_interactive_listen_pending_.load() ||
                                                     lesson_interactive_listening_active_.load();
                const std::uint64_t terminal_audio_generation =
                    lesson_terminal_audio_generation_.exchange(0);
                if (terminal_audio_generation == stopped_audio_generation) {
                    ESP_LOGI(TAG,
                             "terminal lesson tts stop matched generation_hi=%lu generation_lo=%lu "
                             "state=%d",
                             static_cast<unsigned long>(stopped_audio_generation >> 32),
                             static_cast<unsigned long>(stopped_audio_generation),
                             static_cast<int>(GetDeviceState()));
                    lesson_idle_repaint_suppressed_.store(true);
                    SetDeviceState(kDeviceStateIdle);
                    return;
                }
                if (terminal_audio_generation != 0) {
                    ESP_LOGI(TAG,
                             "stale terminal lesson tts stop ignored terminal_hi=%lu "
                             "terminal_lo=%lu stopped_hi=%lu stopped_lo=%lu",
                             static_cast<unsigned long>(terminal_audio_generation >> 32),
                             static_cast<unsigned long>(terminal_audio_generation),
                             static_cast<unsigned long>(stopped_audio_generation >> 32),
                             static_cast<unsigned long>(stopped_audio_generation));
                }
                if (lesson_runtime_active_.load() && !lesson_interactive_turn) {
                    ESP_LOGI(TAG, "lesson tts stop continue ignored state=%d",
                             static_cast<int>(GetDeviceState()));
                    lesson_idle_repaint_suppressed_.store(true);
                    SetDeviceState(kDeviceStateIdle);
                    return;
                }
                if (explicit_stop_listening && GetDeviceState() == kDeviceStateListening) {
                    audio_service_.EnableVoiceProcessing(false);
                    listening_started_ms_.store(0);
                    last_listening_activity_ms_.store(0);
                    while (audio_service_.PopPacketFromSendQueue() != nullptr) {
                    }
                    SetDeviceState(kDeviceStateIdle);
                    ESP_LOGI(TAG, "manual_tts_stop -> idle from listening");
                    return;
                }
                const bool voice_turn_owned = microphone_uplink_authorized_.load() &&
                                              !passive_ws_intent_.load() && online_intent_.load() &&
                                              (GetDeviceState() == kDeviceStateSpeaking ||
                                               GetDeviceState() == kDeviceStateListening);
                if (force_continue_listening && !lesson_interactive_turn) {
                    if (!voice_turn_owned) {
                        ESP_LOGW(
                            TAG,
                            "tts_stop_continue_listening_rejected state=%d passive=%d online=%d",
                            static_cast<int>(GetDeviceState()), passive_ws_intent_.load() ? 1 : 0,
                            online_intent_.load() ? 1 : 0);
                        return;
                    }
                    bool playback_drained =
                        audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                    if (!IsChatLessonRequestCurrent(context))
                        return;
                    if (!playback_drained) {
                        ESP_LOGW(TAG,
                                 "tts_stop_playback_drain_timeout timeout_ms=%lu "
                                 "action=continue_listening",
                                 static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                    }
                    if (force_realtime_listen) {
                        listening_mode_ = kListeningModeRealtime;
                    } else {
                        listening_mode_ = GetDefaultListeningMode();
                    }
                    SetDeviceState(kDeviceStateListening);
                    if (protocol_) {
                        protocol_->SendStartListening(kListeningModeRealtime);
                    }
                    audio_service_.EnableVoiceProcessing(true);
                    const uint64_t resumed_ms = esp_timer_get_time() / 1000;
                    ESP_LOGI(TAG, "mic_loop_resumed ts=%lu%03lu reason=tts_stop_continue_listening",
                             static_cast<unsigned long>(resumed_ms / 1000),
                             static_cast<unsigned long>(resumed_ms % 1000));
                    return;
                }
                if (GetDeviceState() == kDeviceStateSpeaking) {
                    if (listening_mode_ == kListeningModeManualStop) {
                        if (lesson_interactive_turn) {
                            bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                            if (!IsChatLessonRequestCurrent(context))
                                return;
                            if (!playback_drained) {
                                ESP_LOGW(
                                    TAG,
                                    "tts_stop_playback_drain_timeout timeout_ms=%lu "
                                    "action=lesson_listening",
                                    static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                            }
                            SetDeviceState(kDeviceStateListening);
                            ESP_LOGI(TAG, "lesson prompt complete -> listening");
                        } else {
                            SetDeviceState(kDeviceStateIdle);
                        }
                    } else if (listening_mode_ == kListeningModeAutoStop) {
                        bool playback_drained = audio_service_.WaitForPlaybackQueueEmpty(kTtsStopPlaybackDrainTimeoutMs);
                        if (!IsChatLessonRequestCurrent(context))
                            return;
                        if (!playback_drained) {
                            ESP_LOGW(TAG,
                                     "tts_stop_playback_drain_timeout timeout_ms=%lu action=idle",
                                     static_cast<unsigned long>(kTtsStopPlaybackDrainTimeoutMs));
                        }
                        SetDeviceState(kDeviceStateIdle);
                    } else {
                        SetDeviceState(kDeviceStateListening);
                        const uint64_t resumed_ms = esp_timer_get_time() / 1000;
                        ESP_LOGI(TAG, "mic_loop_resumed ts=%lu%03lu",
                                 static_cast<unsigned long>(resumed_ms / 1000),
                                 static_cast<unsigned long>(resumed_ms % 1000));
                    }
                }
            });
        } else if (strcmp(state->valuestring, "sentence_start") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGD(TAG, "<< %s",
                         text->valuestring);
                if (!lesson_runtime_active_.load()) {
                    if (context)
                        display->SetChatMessage("assistant", text->valuestring);
                    else
                        Schedule([display, message = std::string(text->valuestring)]() {
                            display->SetChatMessage("assistant", message.c_str());
                        });
                }
            }
        }
        } else if (strcmp(type->valuestring, "stt") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGD(TAG, ">> %s",
                     text->valuestring);
            if (!lesson_runtime_active_.load()) {
                if (context)
                    display->SetChatMessage("user", text->valuestring);
                else
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("user", message.c_str());
                    });
            }
        }
    } else if (strcmp(type->valuestring, "llm") == 0) {
#if CONFIG_TBOT_VOICE_DEMO
        return;
#endif
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion)) {
            if (!lesson_runtime_active_.load()) {
                if (context) {
                    display->SetEmotion(emotion->valuestring);
                    HandleEmotionGesture(emotion->valuestring);
                } else
                    Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                        display->SetEmotion(emotion_str.c_str());
                        HandleEmotionGesture(emotion_str.c_str());
                    });
            }
        }
    } else if (strcmp(type->valuestring, "mcp") == 0) {
        auto payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            McpServer::GetInstance().ParseMessage(payload, context);
        }
    } else if (strcmp(type->valuestring, "system") == 0) {
        auto command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command)) {
            ESP_LOGI(TAG, "System command: %s", command->valuestring);
            if (strcmp(command->valuestring, "reboot") == 0) {
                if (lesson_runtime_active_.load()) {
                    ESP_LOGI(TAG, "System reboot ignored during lesson");
                    return;
                }
                if (context)
                    Reboot(context);
                else
                    Schedule([this]() { Reboot(); });
            } else if (strcmp(command->valuestring, "unpair") == 0) {
                if (context) {
                    BeginChatUnpair(root, context);
                    return;
                }
                if (lesson_runtime_active_.load()) {
                    ESP_LOGI(TAG, "System unpair ignored during lesson");
                    return;
                }
                const auto* request_id = cJSON_GetObjectItem(root, "request_id");
                if (cJSON_IsString(request_id) && request_id->valuestring != nullptr &&
                    request_id->valuestring[0] != '\0' &&
                    std::strlen(request_id->valuestring) <= 64) {
                    cJSON* ack = cJSON_CreateObject();
                    if (ack != nullptr) {
                        cJSON_AddStringToObject(ack, "type", "system_ack");
                        cJSON_AddStringToObject(ack, "command", "unpair");
                        cJSON_AddStringToObject(ack, "request_id", request_id->valuestring);
                        char* encoded = cJSON_PrintUnformatted(ack);
                        const bool sent = encoded != nullptr && protocol_ != nullptr &&
                                          protocol_->SendLessonFrame(encoded);
                        if (!sent) {
                            ESP_LOGW(TAG, "System unpair acknowledgement could not be sent");
                        }
                        if (encoded != nullptr)
                            cJSON_free(encoded);
                        cJSON_Delete(ack);
                    }
                }
                EnterRepairPairingMode();
            } else if (strcmp(command->valuestring, "wifi_setup") == 0) {
                if (lesson_runtime_active_.load()) {
                    ESP_LOGI(TAG, "System WiFi setup ignored during lesson");
                    return;
                }
                static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
            } else {
                ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
            }
        }
    } else if (strcmp(type->valuestring, "alert") == 0) {
        auto status = cJSON_GetObjectItem(root, "status");
        auto message = cJSON_GetObjectItem(root, "message");
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
            if (!lesson_runtime_active_.load()) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            }
        } else {
            ESP_LOGW(TAG, "Alert command requires status, message and emotion");
        }
    } else if (strcmp(type->valuestring, "robot_action") == 0) {
        if (!HandleRobotActionMessage(root, context)) {
            ESP_LOGW(TAG, "Unsupported robot action");
        }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    } else if (strcmp(type->valuestring, "custom") == 0) {
        auto payload = cJSON_GetObjectItem(root, "payload");
        char* root_str = cJSON_PrintUnformatted(root);
        ESP_LOGI(TAG, "Received custom message: %s", root_str ? root_str : "(null)");
        if (root_str != nullptr) {
            cJSON_free(root_str);
        }
        if (cJSON_IsObject(payload)) {
            if (HandleRobotActionMessage(payload, context)) {
                return;
            }
            char* payload_str_raw = cJSON_PrintUnformatted(payload);
            std::string payload_str =
                (payload_str_raw != nullptr) ? std::string(payload_str_raw) : std::string();
            if (payload_str_raw != nullptr) {
                cJSON_free(payload_str_raw);
            }
            if (!lesson_runtime_active_.load()) {
                if (context)
                    display->SetChatMessage("system", payload_str.c_str());
                else
                    Schedule([this, display, payload_str = std::move(payload_str)]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            }
        } else {
            ESP_LOGW(TAG, "Invalid custom message format: missing payload");
        }
#endif
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    } else if (strncmp(type->valuestring, "lesson_", 7) == 0) {
        if (!is_websocket_protocol) {
            ESP_LOGW(TAG, "lesson_* ignored on non-WebSocket transport");
            return;
        }
        EnqueueLessonMessage(root, callback_transport_epoch, context);
#endif
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
    }
}
