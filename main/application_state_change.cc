#include "application_internal.h"

void Application::HandleStateChangedEvent() {
    ChatRuntimeTiming timing(2,
        []() { return static_cast<uint64_t>(esp_timer_get_time()); },
        [](uint32_t site, uint32_t hi, uint32_t lo) {
            ESP_LOGW(TAG, "chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                     static_cast<unsigned>(site), static_cast<unsigned long>(hi),
                     static_cast<unsigned long>(lo));
        });
    if (AdvanceChatRearm(static_cast<uint64_t>(esp_timer_get_time()))) {
        RenderChatRearm();
        return;
    }
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    if (new_state != kDeviceStateListening && new_state != kDeviceStateSpeaking) {
        microphone_uplink_authorized_.store(false);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // H3 (LOCKED: all 21 states runtime-driven): resolve the live runtime to a
    // connect-state spec so the screen copy + timeout come from the contract
    // table, not hand-coded xiaozhi literals. The audio/wake-word side effects
    // per DeviceState are unchanged below; only the SetStatus SOURCE is
    // redirected through the mapper for the contract-owned states (BOOT,
    // WIFI_CONNECTING/CONNECTED, BOOTSTRAP_FETCHING, BACKEND_CONNECTING, ONLINE,
    // OTA_UPDATING, ERROR_RECOVERABLE). AP states stay defined-but-dormant.
    const TbotConnectStateSpec* connect_spec = TbotConnectMapper::Resolve(
        new_state, claim_substate_, GetBleSubstate(), backend_offline_.load());
    const char* connect_copy = ConnectStateScreenCopy(connect_spec);

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle: {
            if (IsWifiConfigEntryPending())
                break;
            const bool suppress_lesson_idle_repaint =
                lesson_idle_repaint_suppressed_.exchange(false);
            if (lesson_runtime_active_.load()) {
                if (!suppress_lesson_idle_repaint) {
                    display->SetLessonCaption("");
                    display->ClearChatMessages();
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                }
                listening_started_ms_.store(0);
                last_listening_activity_ms_.store(0);
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
                break;
            }
            if (suppress_lesson_idle_repaint) {
                listening_started_ms_.store(0);
                last_listening_activity_ms_.store(0);
                audio_service_.EnableVoiceProcessing(false);
                if (IsDeviceClaimed() && !connect_in_flight_.load() &&
                    !lesson_asset_sync_quiet_.load()) {
                    audio_service_.EnableWakeWordDetection(true);
                } else {
                    audio_service_.EnableWakeWordDetection(false);
                }
                break;
            }
            // ONLINE (or OFFLINE_RETRY / a claim overlay) per the mapper.
            display->SetStatus(connect_copy);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral");  // Then set emotion (wechat mode checks child count)
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);
            audio_service_.EnableVoiceProcessing(false);
            // TBOT BLE+audio contention fix: the AFE/mic input only runs while
            // wake-word (or voice processing) is enabled — that is what feeds the
            // AFE FEED ringbuffer. While the device is UNCLAIMED it is sitting in
            // claimable standby (BLE advertising + claim poll) and has no lessons,
            // so we keep the mic OFF here. Running the AFE mic pipeline alongside
            // BLE advertising on real hardware overflows the FEED ringbuffer
            // ("Ringbuffer of AFE(FEED) is full") and errors the robot. Once the
            // device is CLAIMED, Idle enables wake-word exactly as before so
            // lessons (wake word -> talk) work normally. A fresh claim confirm
            // enables wake-word explicitly (see ConfirmPendingTbotClaim) so audio
            // comes up without a reboot.
            if (IsDeviceClaimed() && !connect_in_flight_.load() &&
                !lesson_asset_sync_quiet_.load()) {
                audio_service_.EnableWakeWordDetection(true);
            } else {
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        }
        case kDeviceStateConnecting:
            if (lesson_runtime_active_.load()) {
                if (lesson_interactive_listen_pending_.load()) {
                    display->ClearChatMessages();
                    display->SetStatus("Sắp đến lượt con...");
                } else {
                    display->SetStatus(Lang::Strings::PLEASE_WAIT);
                }
                break;
            }
            // BACKEND_CONNECTING per the mapper ("Connecting...").
            display->SetStatus(connect_copy);
            display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
            microphone_uplink_authorized_.store(false);
            {
                int64_t now_ms = esp_timer_get_time() / 1000;
                listening_started_ms_.store(now_ms);
                last_listening_activity_ms_.store(now_ms);
            }
            const bool lesson_interactive_listen =
                lesson_interactive_listen_pending_.exchange(false);
            const bool lesson_interactive_active = lesson_interactive_listening_active_.load();
            if (lesson_interactive_listen || lesson_interactive_active) {
                lesson_interactive_listening_active_.store(true);
                display->ClearChatMessages();
                display->SetStatus("Con nói nhé...");
                display->SetChatMessage("system", "Con nói nhé.");
            } else {
                display->SetStatus(Lang::Strings::LISTENING);
                display->SetEmotion("thinking");
            }

            protocol_->SendStartListening(listening_mode_);
            if (!protocol_->IsAudioChannelOpened()) {
                ESP_LOGW(TAG, "listen_start_send_failed -> reconnect");
                ListeningMode mode = listening_mode_;
                audio_service_.EnableVoiceProcessing(false);
                SetDeviceState(kDeviceStateConnecting);
                Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
                break;
            }

            const bool lesson_capture_requested = RequestChatLessonCapture();
            if (!lesson_capture_requested)
                microphone_uplink_authorized_.store(true);

            // Make sure the audio processor is running
            if (lesson_capture_requested || play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice
                // processing This prevents audio truncation when STOP arrives late due to network
                // jitter
                if (listening_mode_ == kListeningModeAutoStop && !aborted_) {
                    bool playback_drained =
                        audio_service_.WaitForPlaybackQueueEmpty(kListenPlaybackDrainTimeoutMs);
                    if (!playback_drained) {
                        ESP_LOGW(
                            TAG,
                            "playback_queue_drain_timeout timeout_ms=%lu action=force_listening",
                            static_cast<unsigned long>(kListenPlaybackDrainTimeoutMs));
                    }
                }
                if (!lesson_capture_requested)
                    audio_service_.EnableVoiceProcessing(true);
            }

            if (!lesson_capture_requested) {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
                // Enable wake word detection in listening mode (configured via Kconfig)
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
                // Disable wake word detection in listening mode
                audio_service_.EnableWakeWordDetection(false);
#endif
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (lesson_interactive_listen) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            } else if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        }
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            if (!lesson_runtime_active_.load()) {
                display->SetEmotion("happy");
            }
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            // NOTE: in Realtime mode we KEEP wake-word + voice-processing
            // running so user can barge in. Echo from speaker is suppressed
            // by device-side AEC (CONFIG_USE_DEVICE_AEC=y) BEFORE the
            // signal reaches the wake-word ML, so echo no longer false-fires.
            break;
        case kDeviceStateStarting:
            // BOOT per the mapper ("Starting").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateActivating:
            // BOOTSTRAP_FETCHING per the mapper ("Loading setup...").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateUpgrading:
            // OTA_UPDATING per the mapper ("Updating...").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateFatalError:
            // ERROR_RECOVERABLE per the mapper ("Hold button 5s to retry").
            display->SetStatus(connect_copy);
            break;
        case kDeviceStateWifiConfiguring:
            // H2: entering Wi-Fi setup -> stop the heartbeat (not a live online
            // session; it (re)starts only from OnConnected).
            display->SetStatus(connect_copy);
            StopHeartbeat();
            StopClaimPoll();
            if (chat_cleanup_enabled_)
                RequestChatAudioCleanup(speaking_generation_.load(), false, false, false);
            else {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        default:
            // Do nothing
            break;
    }
}

