#include "application_internal.h"



bool Application::IsMicrophoneUplinkAuthorized() const {
    if (!microphone_uplink_authorized_.load() || passive_ws_intent_.load() ||
        !online_intent_.load()) {
        return false;
    }
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateListening) {
        return !lesson_runtime_active_.load() || lesson_interactive_listening_active_.load();
    }
    return state == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime &&
           !lesson_runtime_active_.load();
}

void Application::HandleListeningWatchdogTick() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

#if !CONFIG_USE_AUDIO_PROCESSOR || CONFIG_USE_DEVICE_AEC
    // Realtime silence expiry is safe only when the active processor supplies VAD.
    if (listening_mode_ == kListeningModeRealtime) {
        ESP_LOGD(TAG, "realtime_watchdog_disabled_without_vad");
        return;
    }
#endif

    const int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t started_ms = listening_started_ms_.load();
    int64_t last_activity_ms = last_listening_activity_ms_.load();
    if (started_ms <= 0 || last_activity_ms <= 0) {
        listening_started_ms_.store(now_ms);
        last_listening_activity_ms_.store(now_ms);
        return;
    }
    if (IsVoiceDetected()) {
        last_listening_activity_ms_.store(now_ms);
        last_activity_ms = now_ms;
    }

    const int64_t idle_ms = now_ms - last_activity_ms;
    const int64_t turn_ms = now_ms - started_ms;
    const uint32_t idle_limit_ms =
        listening_mode_ == kListeningModeAutoStop
            ? kListeningNoSpeechTimeoutMs
            : (listening_mode_ == kListeningModeRealtime ? kListeningRealtimeNoSpeechTimeoutMs
                                                         : kListeningMaxTurnMs);
    const uint32_t turn_limit_ms = listening_mode_ == kListeningModeAutoStop
                                       ? kListeningAutoStopMaxTurnMs
                                       : kListeningMaxTurnMs;
    const bool turn_timed_out =
        listening_mode_ != kListeningModeRealtime && turn_ms >= turn_limit_ms;
    if (idle_ms < idle_limit_ms && !turn_timed_out) {
        return;
    }
    if (HandleChatStopListening()) {
        if (!RequestChatCue(Lang::Sounds::OGG_EXCLAMATION))
            ESP_LOGW(TAG, "chat_timeout_cue_busy");
        return;
    }

    uint32_t decode_q = 0, send_q = 0, playback_q = 0;
    audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
    auto audio_stats = audio_service_.GetDebugStatistics();
    ESP_LOGW(TAG,
             "listening_watchdog_timeout mode=%d idle_ms=%ld turn_ms=%ld decode_q=%lu send_q=%lu "
             "playback_q=%lu decode_drop=%lu encode_drop=%lu reconnects=%lu",
             static_cast<int>(listening_mode_), static_cast<long>(idle_ms),
             static_cast<long>(turn_ms), (unsigned long)decode_q, (unsigned long)send_q,
             (unsigned long)playback_q, (unsigned long)audio_stats.decode_drop_count,
             (unsigned long)audio_stats.encode_drop_count, (unsigned long)reconnect_count_.load());

    if (protocol_) {
        protocol_->SendStopListening();
    }
    audio_service_.EnableVoiceProcessing(false);
    microphone_uplink_authorized_.store(false);
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {
    }
    listening_started_ms_.store(0);
    last_listening_activity_ms_.store(0);
    lesson_interactive_listen_pending_.store(false);
    lesson_interactive_listening_active_.store(false);
    SetDeviceState(kDeviceStateIdle);
    auto display = Board::GetInstance().GetDisplay();
    if (lesson_runtime_active_.load()) {
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
    } else {
        display->SetStatus(Lang::Strings::SERVER_TIMEOUT);
        display->SetEmotion("thinking");
        audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    }
}

bool Application::IsChatRearmRecoveryCurrent() const {
    return chat_protocol_signals_ && chat_protocol_signals_ == chat_rearm_signals_ &&
           (chat_protocol_signals_->Capture() == chat_rearm_source_era_ ||
            !chat_protocol_signals_->Capture()) &&
           !chat_protocol_owned_.load() &&
           chat_rearm_owner_.protocol_generation == protocol_generation_.load() &&
           chat_rearm_owner_.connect_generation == connect_generation_.load() &&
           chat_rearm_owner_.response_generation == speaking_generation_.load() &&
           audio_service_.IsCurrentChatPlaybackReset(chat_rearm_owner_.reset_token);
}

bool Application::AdvanceChatRearm(uint64_t now_us) {
    using Phase = ChatRearmPhase;
    using Result = ChatOutboundMailbox::Result;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() ||
        lesson_runtime_active_.load())
        return false;
    const auto state = GetDeviceState();
    if (state != kDeviceStateSpeaking && state != kDeviceStateListening &&
        state != kDeviceStateIdle &&
        !(state == kDeviceStateConnecting && chat_rearm_phase_ == Phase::Pending))
        return false;
    if (chat_rearm_phase_ == Phase::Recovery) {
        if (!IsChatRearmRecoveryCurrent())
            return true;
        microphone_uplink_authorized_.store(false);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    if (!chat_playout_stamp_)
        return false;
    if (!chat_playout_ready_ && chat_rearm_phase_ == Phase::None)
        return true;
    const auto& response = chat_playout_response_;
    const bool current = chat_protocol_signals_->MatchesSource(response.source) &&
                         !chat_protocol_owned_.load() &&
                         response.protocol_generation == protocol_generation_.load() &&
                         response.connect_generation == connect_generation_.load() &&
                         response.response_generation == speaking_generation_.load() &&
                         audio_service_.IsCurrentChatPlaybackReset(response.reset_token);
    if (!current) {
        if (response.response_generation == speaking_generation_.load() &&
            chat_rearm_signals_ == chat_protocol_signals_ &&
            (chat_protocol_signals_->Capture() == chat_rearm_source_era_ ||
             !chat_protocol_signals_->Capture()) &&
            audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
            RecoverChatPlayout(225);
            SetDeviceState(kDeviceStateIdle);
        }
        return true;
    }
    PollChatProtocolSignals();
    ChatPlayoutIntake::Stop terminal;
    const auto terminal_read =
        chat_listen_origin_ == ChatListenOrigin::Drain
            ? chat_protocol_signals_->intake.TryCollect(chat_playout_stamp_, terminal)
            : ChatPlayoutIntake::Read::None;
    if (terminal_read == ChatPlayoutIntake::Read::Fault ||
        (terminal_read == ChatPlayoutIntake::Read::Ready &&
         (terminal.interrupt || terminal.conflict))) {
        RecoverChatPlayout(226);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    const bool fault =
        chat_outbound_fault_ || chat_audio_fault_ || chat_playback_fault_ ||
        chat_protocol_infrastructure_fault_ || protocol_work_lifetime_.Pending() || !protocol_ ||
        protocol_->CurrentConnectionEpoch() != response.source.connection_epoch ||
        (chat_protocol_fault_ && chat_protocol_fault_generation_ == response.protocol_generation &&
         chat_protocol_fault_era_ == chat_protocol_signals_->Capture());
    if (fault || !online_intent_.load() || passive_ws_intent_.load()) {
        RecoverChatPlayout(227);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    if ((chat_rearm_phase_ == Phase::Pending && state != kDeviceStateSpeaking &&
         state != kDeviceStateConnecting) ||
        (chat_rearm_phase_ == Phase::Armed && state != kDeviceStateListening)) {
        RecoverChatPlayout(228);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    if (chat_rearm_phase_ == Phase::Armed || chat_rearm_phase_ == Phase::IdleComplete)
        return true;
    const auto received_us = chat_listen_origin_ == ChatListenOrigin::Drain
                                 ? chat_playout_stop_.received_us
                                 : chat_listen_received_us_;
    if (now_us < received_us || now_us - received_us >= ConversationPlayoutController::kTimeoutUs ||
        !online_intent_.load() || passive_ws_intent_.load()) {
        RecoverChatPlayout(229);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    if (chat_rearm_phase_ == Phase::None) {
        const bool owned = (microphone_uplink_authorized_.load() || chat_rearm_voice_intent_) &&
                           (state == kDeviceStateSpeaking || state == kDeviceStateListening);
        const bool resume =
            !chat_playout_stop_.explicit_manual_stop && owned &&
            (chat_playout_stop_.continue_listening ||
             (state == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime));
        microphone_uplink_authorized_.store(false);
        if (!resume) {
            ESP_LOGW(TAG, "chat_rearm_idle site=401 owned=%u manual=%u continuation=%u",
                     static_cast<unsigned>(owned),
                     static_cast<unsigned>(chat_playout_stop_.explicit_manual_stop),
                     static_cast<unsigned>(chat_playout_stop_.continue_listening));
            chat_rearm_voice_intent_ = false;
            chat_rearm_phase_ = Phase::IdleComplete;
            chat_playout_ready_ = false;
            RequestChatAudioCleanup(response.response_generation, false, false,
                                    IsDeviceClaimed() && !connect_in_flight_.load() &&
                                        !lesson_asset_sync_quiet_.load());
            SetDeviceState(kDeviceStateIdle);
            return true;
        }
        chat_rearm_mode_ =
            chat_playout_stop_.continue_listening
                ? (chat_playout_stop_.realtime ? kListeningModeRealtime : GetDefaultListeningMode())
                : listening_mode_;
        chat_rearm_phase_ = Phase::Pending;
        chat_rearm_voice_intent_ = true;
        chat_rearm_prepared_ =
            RequestChatAudioCleanup(response.response_generation, false, true, false, true, false,
                                    ChatWakePolicy::Listening);
        chat_rearm_job_ = {};
        chat_rearm_job_.kind = ChatOutboundMailbox::Kind::ListenStart;
        chat_rearm_job_.argument = chat_rearm_mode_;
        chat_rearm_job_.deadline_us =
            chat_playout_stop_.received_us + ConversationPlayoutController::kTimeoutUs;
        SetDeviceState(kDeviceStateSpeaking);
    }
    if (chat_control_intents_.Size())
        return true;
    if (chat_start_obsolete_reservation_)
        return true;
    if (!chat_rearm_admitted_) {
        const auto result = SubmitChatOutbound(chat_rearm_job_);
        if (result == Result::Sent)
            chat_rearm_admitted_ = true;
        else if (result != Result::Busy) {
            RecoverChatPlayout(230);
            SetDeviceState(kDeviceStateIdle);
        }
        return true;
    }
    if (!chat_rearm_delivery_ || chat_audio_prepared_ != chat_rearm_prepared_ ||
        audio_service_.IsChatPlaybackResetPending() ||
        chat_audio_reset_completed_ != response.reset_token)
        return true;
    if (!IsChatOutboundCompletionCurrent(*chat_rearm_delivery_) ||
        chat_rearm_delivery_->result != Result::Sent ||
        static_cast<uint64_t>(esp_timer_get_time()) - received_us >=
            ConversationPlayoutController::kTimeoutUs ||
        !audio_service_.ArmChatUplink(chat_rearm_prepared_)) {
        RecoverChatPlayout(231);
        SetDeviceState(kDeviceStateIdle);
        return true;
    }
    chat_rearm_phase_ = Phase::Armed;
    chat_playout_ready_ = false;
    listening_mode_ = chat_rearm_mode_;
    microphone_uplink_authorized_.store(true);
    const auto now_ms = esp_timer_get_time() / 1000;
    listening_started_ms_.store(now_ms);
    last_listening_activity_ms_.store(now_ms);
    SetDeviceState(kDeviceStateListening);
    return true;
}

void Application::RenderChatRearm() {
    // START admission can precede its confirmed Speaking state event.
    if (chat_rearm_phase_ == ChatRearmPhase::None && GetDeviceState() != kDeviceStateSpeaking)
        return;
    if (chat_rearm_phase_ == ChatRearmPhase::Recovery && !IsChatRearmRecoveryCurrent())
        return;
    if (chat_rearm_phase_ != ChatRearmPhase::Recovery &&
        (!chat_protocol_signals_ || chat_rearm_signals_ != chat_protocol_signals_ ||
         !chat_protocol_signals_->MatchesSource(chat_rearm_owner_.source) ||
         chat_rearm_owner_.protocol_generation != protocol_generation_.load() ||
         chat_rearm_owner_.connect_generation != connect_generation_.load() ||
         chat_rearm_owner_.response_generation != speaking_generation_.load() ||
         !audio_service_.IsCurrentChatPlaybackReset(chat_rearm_owner_.reset_token)))
        return;
    if (chat_rearm_phase_ == chat_rearm_rendered_phase_ &&
        chat_rearm_owner_.reset_token == chat_rearm_rendered_reset_ &&
        backend_offline_.load() == chat_rearm_rendered_offline_)
        return;
    // Polling audio readiness must not continuously rebuild the same GIF.
    chat_rearm_rendered_phase_ = chat_rearm_phase_;
    chat_rearm_rendered_reset_ = chat_rearm_owner_.reset_token;
    chat_rearm_rendered_offline_ = backend_offline_.load();
    auto& board = Board::GetInstance();
    board.GetLed()->OnStateChanged();
    auto* display = board.GetDisplay();
    if (chat_rearm_phase_ == ChatRearmPhase::None) {
        display->SetStatus(Lang::Strings::SPEAKING);
    } else if (chat_rearm_phase_ == ChatRearmPhase::Pending) {
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
    } else if (chat_rearm_phase_ == ChatRearmPhase::Armed) {
        display->SetStatus(Lang::Strings::LISTENING);
        display->SetEmotion("thinking");
    } else if (chat_rearm_phase_ == ChatRearmPhase::IdleComplete ||
               chat_rearm_phase_ == ChatRearmPhase::Recovery) {
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        const auto* spec = TbotConnectMapper::Resolve(GetDeviceState(), claim_substate_,
                                                      GetBleSubstate(), backend_offline_.load());
        display->SetStatus(ConnectStateScreenCopy(spec));
        display->ClearChatMessages();
        display->SetEmotion(backend_offline_.load() ? "thinking" : "neutral");
    }
}


    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        if (!audio_service_.IsRunning()) {
            ESP_LOGI(TAG, "Audio test unavailable while provisioning workers are deferred");
            return;
        }
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (pending_tbot_claim_.active) {
        ConfirmPendingTbotClaim();
        return;
    }

    if (claim_substate_ == TbotClaimSubstate::ConfirmTimeout) {
        ESP_LOGI(TAG, "Claim confirm timeout -> retry: re-entering claim standby poll");
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && backend_offline_.load() &&
        (state == kDeviceStateIdle || state == kDeviceStateConnecting ||
         state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap from offline retry -> reopening phone scan standby");
        backend_offline_.store(false);
        if (state != kDeviceStateIdle) {
            SetDeviceState(kDeviceStateIdle);
        }
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && state == kDeviceStateIdle &&
        (claim_substate_ == TbotClaimSubstate::AvailableStandby ||
         claim_substate_ == TbotClaimSubstate::None)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap -> refreshing claimable standby for phone scan");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    if (IsSelectedNormalChatRoute()) {
        if (state == kDeviceStateIdle)
            BeginChatListen(GetDefaultListeningMode(), ChatListenOrigin::User);
        else if (state == kDeviceStateSpeaking)
            HandleChatAbort(kAbortReasonNone, listening_mode_ != kListeningModeManualStop);
        else if (state == kDeviceStateListening)
            CloseAudioChannelByIntent();
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        CloseAudioChannelByIntent();
    }
}

