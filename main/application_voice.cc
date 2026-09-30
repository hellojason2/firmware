#include "application_internal.h"

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

bool Application::HandleChatStopListening() {
    CancelChatRecovery();
    if (!IsSelectedNormalChatRoute())
        return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
               !lesson_runtime_active_.load();
    if (GetDeviceState() != kDeviceStateListening && chat_rearm_phase_ != ChatRearmPhase::Pending)
        return true;
    if (!chat_playout_stamp_) {
        ConnectionSource source;
        if (!audio_service_.IsCurrentChatPlaybackReset(chat_audio_reset_serial_))
            chat_audio_reset_serial_ = RequestChatPlaybackCleanup(speaking_generation_.load());
        if (!chat_protocol_signals_->TrySource(source) ||
            !EstablishChatPlayoutResponse({source, protocol_generation_.load(),
                                           connect_generation_.load(), speaking_generation_.load(),
                                           chat_audio_reset_serial_})) {
            RecoverChatPlayout(222);
            return true;
        }
    }
    if (!RetainChatActiveListen())
        return true;
    chat_rearm_voice_intent_ = false;
    microphone_uplink_authorized_.store(false);
    RequestChatAudioCleanup(
        speaking_generation_.load(), false, false,
        IsDeviceClaimed() && !connect_in_flight_.load() && !lesson_asset_sync_quiet_.load());
    chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
    RequestChatControl(ChatOutboundMailbox::Kind::ListenStop);
    chat_playout_ready_ = false;
    SetDeviceState(kDeviceStateIdle);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::BeginChatListen(ListeningMode mode, ChatListenOrigin origin) {
    if (IsWifiConfigEntryPending())
        return false;
    ConnectionSource available;
    if (origin == ChatListenOrigin::User && chat_protocol_signals_ &&
        chat_protocol_signals_->SourceSelected() && !lesson_runtime_active_.load() &&
        (chat_recovery_.kind != ChatRecoveryIntent::Kind::None || chat_protocol_owned_.load() ||
         !chat_protocol_signals_->TrySource(available) ||
         chat_source_connect_generation_.load() != connect_generation_.load()))
        return RetainChatRecovery(ChatRecoveryIntent::Kind::Listen, mode);
    if (!IsSelectedNormalChatRoute() ||
        (passive_ws_intent_.load() && origin != ChatListenOrigin::User))
        return false;
    const auto state = GetDeviceState();
    if (!protocol_ || lesson_asset_sync_quiet_.load() ||
        (state != kDeviceStateIdle && state != kDeviceStateSpeaking &&
         state != kDeviceStateListening && state != kDeviceStateConnecting))
        return false;
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source) ||
        protocol_->CurrentConnectionEpoch() != source.connection_epoch)
        return false;
    if (origin == ChatListenOrigin::User)
        passive_ws_intent_.store(false);
    online_intent_.store(true);
    if (chat_rearm_phase_ == ChatRearmPhase::Pending)
        return true;
    if (chat_control_intents_.Size() >= 4) {
        chat_protocol_infrastructure_fault_ = true;
        RecoverChatPlayout(223);
        return true;
    }
    if (!chat_outbound_generation_) {
        if (chat_outbound_reservation_) {
            chat_start_obsolete_reservation_ = chat_outbound_reservation_;
        } else if (ActivateChatOutbound(source.connection_epoch) !=
                   ChatOutboundMailbox::Result::Sent) {
            RecoverChatPlayout(224);
            return true;
        }
    }
    if (!chat_playout_stamp_ || chat_playout_response_.source.source_id != source.source_id ||
        chat_playout_response_.source.connection_epoch != source.connection_epoch ||
        chat_playout_response_.protocol_generation != protocol_generation_.load() ||
        chat_playout_response_.connect_generation != connect_generation_.load() ||
        chat_playout_response_.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(chat_playout_response_.reset_token)) {
        auto generation = speaking_generation_.load();
        if (generation >= UINT32_MAX - 1)
            return false;
        speaking_generation_.store(++generation);
        const auto reset = RequestChatPlaybackCleanup(generation);
        if (!EstablishChatPlayoutResponse({source, protocol_generation_.load(),
                                           connect_generation_.load(), generation, reset}))
            return false;
    }
    chat_listen_origin_ = origin;
    chat_listen_received_us_ = static_cast<uint64_t>(esp_timer_get_time());
    chat_rearm_mode_ = mode;
    chat_rearm_phase_ = ChatRearmPhase::Pending;
    chat_rearm_voice_intent_ = true;
    chat_rearm_job_ = {};
    chat_rearm_delivery_.reset();
    chat_rearm_admitted_ = false;
    chat_playout_recovery_ = false;
    chat_playout_ready_ = false;
    microphone_uplink_authorized_.store(false);
    chat_rearm_prepared_ = RequestChatAudioCleanup(speaking_generation_.load(), false, true, false,
                                                   true, false, ChatWakePolicy::Listening);
    chat_rearm_job_.kind = ChatOutboundMailbox::Kind::ListenStart;
    chat_rearm_job_.argument = mode;
    chat_rearm_job_.deadline_us = chat_listen_received_us_ + 10000000ULL;
    if (state != kDeviceStateConnecting)
        SetDeviceState(kDeviceStateSpeaking);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::HandleChatAbort(AbortReason reason, bool resume) {
    CancelChatRecovery();
    if (!IsSelectedNormalChatRoute())
        return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
               !lesson_runtime_active_.load();
    if (!RetainChatActiveListen())
        return true;
    speaking_arm_dispatch_.Cancel();
    interrupt_count_.fetch_add(1, std::memory_order_relaxed);
    aborted_ = true;
    tts_audio_accepting_.store(false);
    last_speaking_activity_ms_.store(0);
    chat_rearm_voice_intent_ = false;
    microphone_uplink_authorized_.store(false);
    RequestChatAudioCleanup(speaking_generation_.load(), true, false,
                            !resume && IsDeviceClaimed() && !connect_in_flight_.load() &&
                                !lesson_asset_sync_quiet_.load());
    RequestChatControl(ChatOutboundMailbox::Kind::Abort, reason);
    chat_playout_response_.reset_token = chat_audio_reset_serial_;
    chat_rearm_owner_ = chat_playout_response_;
    chat_listen_origin_ = ChatListenOrigin::Abort;
    chat_playout_begun_ = chat_playout_ready_ = false;
    chat_playout_controller_.Cancel();
    if (resume)
        BeginChatListen(GetDefaultListeningMode(), ChatListenOrigin::Abort);
    else {
        chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
        SetDeviceState(kDeviceStateIdle);
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    }
    return true;
}

bool Application::HandleChatWake(const std::string& wake_word, bool read_worker) {
    if (IsWifiConfigEntryPending())
        return true;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() ||
        lesson_runtime_active_.load())
        return false;
    ConnectionSource available;
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None || chat_protocol_owned_.load() ||
        !chat_protocol_signals_->TrySource(available) ||
        chat_source_connect_generation_.load() != connect_generation_.load()) {
        const bool new_wake = chat_recovery_.kind == ChatRecoveryIntent::Kind::None ||
                              chat_recovery_.kind == ChatRecoveryIntent::Kind::Background;
        if (RetainChatRecovery(ChatRecoveryIntent::Kind::Wake, kListeningModeAutoStop) &&
            new_wake) {
            if (wake_word.size() > ChatOutboundMailbox::kMaxPayloadSize) {
                CancelChatRecovery(5);
                return true;
            }
            chat_recovery_.read_wake = read_worker;
            chat_recovery_.wake_size = wake_word.size();
            std::memcpy(chat_recovery_.wake_text.data(), wake_word.data(), wake_word.size());
        }
        return true;
    }
    const auto state = GetDeviceState();
    if (!protocol_ || lesson_asset_sync_quiet_.load() ||
        (state != kDeviceStateIdle && state != kDeviceStateSpeaking &&
         state != kDeviceStateListening && state != kDeviceStateConnecting)) {
        ESP_LOGI(TAG, "chat_recovery outcome=5");
        return true;
    }
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source) ||
        protocol_->CurrentConnectionEpoch() != source.connection_epoch)
        return true;
    ESP_LOGI(TAG, "chat_recovery outcome=6");
    passive_ws_intent_.store(false);
    online_intent_.store(true);
    const bool active = state == kDeviceStateSpeaking || state == kDeviceStateListening;
    if (active)
        HandleChatAbort(kAbortReasonWakeWordDetected, false);
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!active && !RequestChatControl(ChatOutboundMailbox::Kind::Wake, 0, wake_word, read_worker))
        return true;
#else
    (void)wake_word;
    (void)read_worker;
#endif
    BeginChatListen(active ? GetDefaultListeningMode() : kListeningModeAutoStop,
                    ChatListenOrigin::Wake);
    if (active && !RequestChatCue(Lang::Sounds::OGG_POPUP))
        ESP_LOGW(TAG, "chat_popup_busy");
#if !CONFIG_SEND_WAKE_WORD_DATA
    if (!active && !RequestChatCue(Lang::Sounds::OGG_POPUP))
        ESP_LOGW(TAG, "chat_popup_busy");
#endif
    return true;
}

void Application::HandleStopListeningEvent() {
    if (HandleChatStopListening())
        return;
    auto state = GetDeviceState();
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() || lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson stop listening ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (state == kDeviceStateListening) {
        lesson_interactive_listen_generation_.fetch_add(1);
        if (protocol_) {
            protocol_->SendStopListening();
        }
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (HandleChatWake({}, true))
        return;
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored wake word");
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        if (lesson_interactive_listen_pending_.load() ||
            lesson_interactive_listening_active_.load()) {
            ESP_LOGI(TAG, "lesson wake ignored state=%d", static_cast<int>(state));
            return;
        }
        AbortSpeaking(kAbortReasonWakeWordDetected);
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            audio_service_.EnableWakeWordDetection(true);
        } else {
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    if (HandleChatWake(wake_word))
        return;
    // Check state again in case it was changed during scheduling
    auto state = GetDeviceState();
    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
        return;
    }
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake continue ignored state=%d", static_cast<int>(state));
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (connect_in_flight_.load()) {
            if (passive_ws_intent_.load()) {
                deferred_wake_word_ = wake_word;
            }
            ESP_LOGW(TAG, "wake_audio_channel_open_deferred: connect already in flight");
            return;
        }
        reconnect_mode_ = GetDefaultListeningMode();
        uint32_t gen = ++connect_generation_;
        connect_in_flight_.store(true);
        connect_attempt_active_.store(true);
        ArmConnectWatchdog();
        passive_ws_intent_.store(false);
        auto* ctx = new ConnectContext{this, reconnect_mode_, gen, wake_word, true, false};
        if (!StartOpenChannelWorker(ctx)) {
            delete ctx;
            connect_in_flight_.store(false);
            connect_attempt_active_.store(false);
            CancelConnectWatchdog();
            ESP_LOGE(TAG, "wake_ws_open worker unavailable -> idle");
            audio_service_.EnableWakeWordDetection(true);
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    FinishWakeWordInvoke(wake_word);
}

void Application::FinishWakeWordInvoke(const std::string& wake_word) {
    if (HandleChatWake(wake_word))
        return;
    auto state = GetDeviceState();
    if (state != kDeviceStateConnecting && state != kDeviceStateIdle) {
        return;
    }

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson wake finish ignored state=%d", static_cast<int>(state));
        if (state == kDeviceStateConnecting) {
            SetDeviceState(kDeviceStateIdle);
        }
        return;
    }

    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        audio_service_.EnableWakeWordDetection(true);
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    protocol_->SendWakeWordDetected(wake_word);
    if (!protocol_->IsAudioChannelOpened()) {
        ESP_LOGW(TAG, "wake_detect_send_failed -> reopen audio channel");
        SetDeviceState(kDeviceStateConnecting);
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    SetListeningMode(kListeningModeAutoStop);
#else
    play_popup_on_listening_ = true;
    SetListeningMode(kListeningModeAutoStop);
#endif
}

// H3: localized screen copy for a connect state. The connect-state spec table
// (kTbotConnectStateSpecs) is the single source of truth for WHAT copy a state
// shows; this returns the vi-VN-localized equivalent where a Lang::Strings key
// exists and falls back to the contract screen_text otherwise. Mirrors the
// RenderClaimSubstate() pattern so display copy never drifts from the contract.
static const char* ConnectStateScreenCopy(const TbotConnectStateSpec* spec);
