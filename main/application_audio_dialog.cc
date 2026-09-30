#include "application_internal.h"

void Application::RecoverChatPlayout(uint32_t site) {
    if (chat_playout_recovery_) return;
    ESP_LOGW(TAG, "chat_recovery site=%u", static_cast<unsigned>(site));
    if (!chat_playout_stamp_ && chat_protocol_signals_) {
        chat_protocol_signals_->TrySource(chat_playout_response_.source);
        chat_playout_response_.protocol_generation = protocol_generation_.load();
        chat_playout_response_.connect_generation = connect_generation_.load();
        chat_rearm_signals_ = chat_protocol_signals_;
        chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    }
    chat_rearm_voice_intent_ = false;
    chat_playout_ready_ = false;
    chat_playout_recovery_ = true;
    chat_rearm_phase_ = ChatRearmPhase::Recovery;
    chat_playout_controller_.Cancel();
    RetireChatOutbound();
    tts_audio_accepting_.store(false);
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX)
        speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    chat_rearm_owner_ = chat_playout_response_;
    chat_rearm_owner_.response_generation = generation;
    chat_rearm_owner_.reset_token = chat_audio_reset_serial_;
    microphone_uplink_authorized_.store(false);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::PollChatPlayout(uint64_t now_us) {
    using Controller = ConversationPlayoutController;
    using Result = ChatOutboundMailbox::Result;
    if (!chat_playout_stamp_ || chat_playout_recovery_)
        return;
    const auto& response = chat_playout_response_;
    if (!chat_protocol_signals_ || !chat_protocol_signals_->intake.Current(chat_playout_stamp_) ||
        !chat_protocol_signals_->MatchesSource(response.source) || chat_protocol_owned_.load() ||
        response.protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
        // Source/intent loss cancels the same live response immediately, but
        // obsolete drain work must not reset a successor response or reset token.
        if (response.response_generation == speaking_generation_.load() &&
            audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
            RecoverChatPlayout(201);
            return;
        }
        chat_playout_controller_.Cancel();
        if (chat_playout_ack_admitted_ && chat_playout_ack_.generation == chat_outbound_generation_)
            RetireChatOutbound();
        chat_playout_ready_ = false;
        chat_playout_cancelled_ = true;
        chat_playout_stamp_ = 0;
        return;
    }
    // The original clock remains owned even while mailbox/audio/outbound work
    // is Busy. Ready retains this same clock for the later listen-state consumer.
    if (chat_listen_origin_ == ChatListenOrigin::Drain && chat_playout_begun_ &&
        chat_rearm_phase_ != ChatRearmPhase::Armed &&
        chat_rearm_phase_ != ChatRearmPhase::IdleComplete &&
        now_us - chat_playout_stop_.received_us >= Controller::kTimeoutUs) {
        RecoverChatPlayout(202);
        return;
    }
    ChatPlayoutIntake::Stop stop;
    // Local listen/abort ownership must not consume the retained server STOP.
    // A confirmed server START establishes a new Drain origin.
    const auto read = chat_listen_origin_ == ChatListenOrigin::Drain
                          ? chat_protocol_signals_->intake.TryCollect(chat_playout_stamp_, stop)
                          : ChatPlayoutIntake::Read::None;
    if (read == ChatPlayoutIntake::Read::Fault) {
        RecoverChatPlayout(203);
        return;
    }
    if (read == ChatPlayoutIntake::Read::Ready && (stop.interrupt || stop.conflict)) {
        chat_playout_stop_ = stop;
        RecoverChatPlayout(204);
        return;
    }
    if (!chat_playout_begun_ && read == ChatPlayoutIntake::Read::Ready) {
        chat_playout_stop_ = stop;
        // Observe time after a newer STOP without moving its original deadline.
        if (now_us < stop.received_us)
            now_us = static_cast<uint64_t>(esp_timer_get_time());
        if (now_us - stop.received_us >= Controller::kTimeoutUs) {
            RecoverChatPlayout(205);
            return;
        }
        const Controller::Ownership owner{response.source.connection_epoch,
                                          response.response_generation, stop.reset_epoch, false};
        const Controller::Token token{owner.connection_epoch,
                                      owner.response_generation,
                                      owner.reset_epoch,
                                      {stop.drain_id.data(), stop.drain_id_size}};
        if (!stop.valid || !stop.reset_captured || stop.interrupt ||
            !chat_playout_controller_.Begin(stop.received_us, token, owner).accepted) {
            RecoverChatPlayout(206);
            return;
        }
        chat_playout_begun_ = true;
    }
    if (chat_outbound_fault_ || chat_playback_fault_ || chat_protocol_infrastructure_fault_ ||
        (chat_protocol_fault_ && chat_protocol_fault_generation_ == response.protocol_generation &&
         chat_protocol_fault_era_ == chat_protocol_signals_->Capture())) {
        RecoverChatPlayout(207);
        return;
    }
    if (!chat_playout_unhandled_completion_) {
        ChatOutboundMailbox::Completion completion;
        if (PollChatOutbound(&completion)) {
            if (chat_start_obsolete_generation_ &&
                completion.job.generation == chat_start_obsolete_generation_) {
                // Retain obsolete identity until the worker's final retirement;
                // its completion can never advance the successor controller.
            } else if (completion.job.kind == ChatOutboundMailbox::Kind::ListenStart &&
                       chat_rearm_admitted_ &&
                       completion.job.request_id == chat_rearm_job_.request_id &&
                       completion.job.generation == chat_rearm_job_.generation &&
                       completion.job.protocol_generation == chat_rearm_job_.protocol_generation &&
                       completion.job.connection_epoch == chat_rearm_job_.connection_epoch) {
                chat_rearm_delivery_ = completion;
                if (!IsChatOutboundCompletionCurrent(completion) ||
                    completion.result != Result::Sent) {
                    RecoverChatPlayout(208);
                    return;
                }
                xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
            } else if (completion.job.kind != ChatOutboundMailbox::Kind::DrainAck ||
                       !chat_playout_ack_admitted_ ||
                       completion.job.request_id != chat_playout_ack_.request_id) {
                chat_playout_unhandled_completion_ = completion;
                RecoverChatPlayout(209);
                return;
            } else {
                const auto delivery =
                    !IsChatOutboundCompletionCurrent(completion) ? Controller::Delivery::Stale
                    : completion.result == Result::Sent          ? Controller::Delivery::Sent
                    : completion.result == Result::Busy          ? Controller::Delivery::Busy
                    : completion.result == Result::Stale         ? Controller::Delivery::Stale
                                                                 : Controller::Delivery::Failed;
                chat_playout_controller_.Deliver(chat_playout_ack_controller_id_, delivery);
                // Delivery retires the physical ACK even if drain observation is Busy.
                chat_playout_ack_admitted_ = false;
                chat_playout_ack_controller_id_ = 0;
            }
        }
        if (chat_outbound_fault_) {
            RecoverChatPlayout(210);
            return;
        }
    }
    if (chat_start_obsolete_reservation_) {
        if (chat_outbound_reservation_ == chat_start_obsolete_reservation_)
            return;
        chat_start_obsolete_reservation_ = 0;
        chat_start_obsolete_generation_ = 0;
        chat_start_obsolete_ack_ = {};
        if (ActivateChatOutbound(response.source.connection_epoch) != Result::Sent) {
            RecoverChatPlayout(211);
            return;
        }
    }
    if (chat_playout_ready_ || chat_rearm_phase_ == ChatRearmPhase::Pending)
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    if (chat_listen_origin_ != ChatListenOrigin::Drain || !chat_playout_begun_ ||
        chat_playout_ready_ || chat_rearm_phase_ == ChatRearmPhase::Armed ||
        chat_rearm_phase_ == ChatRearmPhase::IdleComplete)
        return;
    PlaybackDrainSnapshot snapshot;
    const bool observed = audio_service_.TryGetPlaybackDrainSnapshot(snapshot);
    const Controller::Ownership owner{response.source.connection_epoch,
                                      response.response_generation, chat_playout_stop_.reset_epoch,
                                      false};
    const auto effect =
        chat_playout_controller_.Poll(now_us, owner, observed ? &snapshot : nullptr);
    if (effect.kind == Controller::EffectKind::Complete) {
        chat_playout_ready_ = true;
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
        return;
    }
    if (effect.kind == Controller::EffectKind::Cancel ||
        effect.kind == Controller::EffectKind::Recover) {
        RecoverChatPlayout(212);
        return;
    }
    if (effect.kind == Controller::EffectKind::SubmitAck) {
        if (chat_playout_stop_.received_us > UINT64_MAX - Controller::kTimeoutUs) {
            RecoverChatPlayout(213);
            return;
        }
        chat_playout_ack_ = {};
        chat_playout_ack_.kind = ChatOutboundMailbox::Kind::DrainAck;
        chat_playout_ack_.deadline_us = chat_playout_stop_.received_us + Controller::kTimeoutUs;
        chat_playout_ack_.SetPayload(effect.drain_id.data(), effect.drain_id_size);
        chat_playout_ack_controller_id_ = effect.request_id;
        chat_playout_ack_admitted_ = false;
    }
    if (chat_playout_ack_controller_id_ && !chat_playout_ack_admitted_) {
        const auto admitted = SubmitChatOutbound(chat_playout_ack_);
        if (admitted == Result::Sent)
            chat_playout_ack_admitted_ = true;
        else if (admitted != Result::Busy)
            RecoverChatPlayout(214);
    }
}

bool Application::SelectChatProtocolSource(ConnectionSource source, uint64_t protocol_generation,
                                           uint32_t connect_generation) {
    // Caller proves current successful Open/source identity under lifetime protection. No socket
    // lookup or implicit activation can turn an old callback into a new source.
    if (!chat_protocol_signals_ || chat_protocol_owned_.load(std::memory_order_acquire) ||
        protocol_generation != protocol_generation_.load() ||
        connect_generation != connect_generation_.load())
        return false;
    chat_source_connect_generation_.store(connect_generation);
    if (!chat_protocol_signals_->EnableForSource(source))
        return false;
    chat_start_handled_serial_ = chat_start_failed_serial_ = chat_start_effects_serial_ = 0;
    return true;
}

void Application::PollChatSourceOpen(uint64_t now_us) {
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None &&
        chat_recovery_.lesson_generation != lesson_runtime_generation_.load())
        CancelChatRecovery();
    if (!chat_protocol_signals_)
        return;
    ChatProtocolSignals::Opened opened;
    if (!chat_protocol_signals_->ReadOpened(opened) ||
        opened.source.source_id <= chat_source_open_handled_)
        return;
    if (!protocol_ || opened.connect_generation != connect_generation_.load() ||
        IsConnectSuccessPublicationSuppressed()) {
        chat_source_open_handled_ = opened.source.source_id;
        return;
    }
    if (opened.deadline_us && now_us >= opened.deadline_us) {
        chat_source_open_handled_ = opened.source.source_id;
        ESP_LOGW(TAG, "chat_source_fault reason=open_deadline");
        chat_protocol_signals_->PublishConnectionFault(opened.source, opened.connect_generation,
                                                       ChatProtocolSignals::Error);
        return;
    }
    if (chat_protocol_owned_.load())
        return;
    chat_source_open_handled_ = opened.source.source_id;
    if (protocol_->CurrentConnectionEpoch() != opened.source.connection_epoch)
        return;
    if (!SelectChatProtocolSource(opened.source, protocol_generation_.load(),
                                  opened.connect_generation))
        return;
    if (chat_recovery_.kind != ChatRecoveryIntent::Kind::None && chat_recovery_.attempted &&
        chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.connect_generation == opened.connect_generation) {
        chat_recovery_.adopted = true;
        ESP_LOGI(TAG, "chat_recovery outcome=4");
    }
    backend_recovery_window_.Reset();
    if (passive_ws_intent_.load()) {
        online_intent_.store(false);
        microphone_uplink_authorized_.store(false);
        if (IsDeviceClaimed() && !lesson_runtime_active_.load()) {
            StartHeartbeat();
            DispatchDeviceHeartbeat();
        } else
            StopHeartbeat();
    } else {
        const bool lesson_answer_turn = lesson_interactive_listen_pending_.load() ||
                                        lesson_interactive_listening_active_.load();
        if (lesson_runtime_active_.load() && !lesson_answer_turn) {
            online_intent_.store(false);
            StopHeartbeat();
            return;
        }
        online_intent_.store(true);
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    }
    backend_offline_.store(false);
    DismissAlert();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (IsDeviceClaimed())
        StopClaimPoll();
    if (opened.sample_rate != board.GetAudioCodec()->output_sample_rate())
        ESP_LOGW(TAG, "Server sample rate %d differs from device output rate", opened.sample_rate);
}

void Application::PollChatProtocolSignals() {
    if (!chat_protocol_signals_)
        return;
    uint32_t flags = 0;
    chat_protocol_signals_->Collect(flags);
    ChatProtocolSignals::Failure failure;
    const bool current_failure = chat_protocol_signals_->ReadFailure(failure) &&
                                 failure.connect_generation == connect_generation_.load();
    if (current_failure)
        flags |= failure.flags;
    if (!flags)
        return;
    if (chat_protocol_signals_->SourceSelected() &&
        (chat_source_connect_generation_.load() != connect_generation_.load() ||
         chat_protocol_owned_.load(std::memory_order_acquire)))
        return;
    chat_protocol_fault_ = true;
    chat_protocol_fault_generation_ = protocol_generation_.load();
    chat_protocol_fault_era_ = chat_protocol_signals_->Capture();
    if (chat_cleanup_enabled_ && current_failure &&
        failure.source.source_id > chat_source_failure_handled_) {
        chat_source_failure_handled_ = failure.source.source_id;
        HandleChatSourceFailure();
    }
    // Task 4b consumes this retained fault with the current chat intent. Never
    // call legacy recovery/audio side effects from a transport callback.
}

void Application::HandleChatSourceFailure() {
    if (!lesson_runtime_active_.load() && !passive_ws_intent_.load() && online_intent_.load() &&
        RetainChatRecovery(ChatRecoveryIntent::Kind::Background, GetDefaultListeningMode()) &&
        chat_recovery_.kind == ChatRecoveryIntent::Kind::Background &&
        chat_rearm_phase_ == ChatRearmPhase::Pending &&
        (chat_listen_origin_ == ChatListenOrigin::Wake ||
         chat_listen_origin_ == ChatListenOrigin::User)) {
        chat_recovery_.kind = chat_listen_origin_ == ChatListenOrigin::Wake
                                  ? ChatRecoveryIntent::Kind::Wake
                                  : ChatRecoveryIntent::Kind::Listen;
        chat_recovery_.received_us = chat_listen_received_us_;
        chat_recovery_.deadline_us = chat_rearm_job_.deadline_us;
        chat_recovery_.mode = chat_rearm_mode_;
        if (const auto* wake = chat_control_intents_.Front();
            wake && wake->job.kind == ChatOutboundMailbox::Kind::Wake) {
            chat_recovery_.read_wake = wake->resolve_wake;
            chat_recovery_.wake_text = wake->job.payload;
            chat_recovery_.wake_size = wake->job.payload_size;
        }
    }
    chat_rearm_voice_intent_ = false;
    chat_rearm_phase_ = ChatRearmPhase::None;
    tts_audio_accepting_.store(false);
    microphone_uplink_authorized_.store(false);
    speaking_arm_dispatch_.Cancel();
    RetireChatOutbound();
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX)
        speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    RequestLessonStorageAbandonment();
    backend_offline_.store(true);
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    const auto state = GetDeviceState();
    if (state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting)
        return;
    if (connect_in_flight_.load())
        return;
    deferred_close_generation_ = 0;
    protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
    PollChatProtocolCleanup();
    if (!lesson_runtime_active_.load() || !passive_ws_intent_.load())
        SetDeviceState(kDeviceStateIdle);
    if (ShouldKeepManagementHeartbeat()) {
        StartHeartbeat();
        DispatchDeviceHeartbeat();
    } else
        StopHeartbeat();
    auto* display = board.GetDisplay();
    if (!lesson_runtime_active_.load())
        display->SetChatMessage("system", "");
    if (passive_ws_intent_.load()) {
        if (!reconnect_passive_.load())
            SchedulePassiveLessonReconnect();
        return;
    }
    if (!online_intent_.load())
        return;
    if (lesson_runtime_active_.load()) {
        online_intent_.store(false);
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        display->SetStatus(Lang::Strings::PLEASE_WAIT);
        return;
    }
    display->SetStatus(Lang::Strings::SERVER_UNAVAILABLE_RETRYING);
    display->SetEmotion("thinking");
    RequestChatCue(Lang::Sounds::OGG_EXCLAMATION);
    ScheduleReconnect(GetDefaultListeningMode(), false);
}
