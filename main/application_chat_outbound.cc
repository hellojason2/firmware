#include "application_internal.h"

ChatOutboundMailbox::Result Application::ActivateChatOutbound(uint32_t connection_epoch) {
    using Result = ChatOutboundMailbox::Result;
    if (!chat_outbound_task_ || !protocol_ || !connection_epoch || !protocol_generation_.load())
        return Result::Failed;
    if (chat_outbound_reservation_ || protocol_work_lifetime_.Pending() ||
        protocol_work_lifetime_.Busy())
        return Result::Busy;
    const auto reservation = protocol_work_lifetime_.Reserve();
    if (!reservation)
        return Result::Busy;
    const auto generation = chat_outbound_worker_.AdvanceGeneration();
    const auto protocol_generation = protocol_generation_.load();
    const ChatOutboundWorker::Activation activation{
        protocol_.get(),
        protocol_generation,
        connection_epoch,
        generation,
        this,
        [](void* context) {
            return static_cast<Application*>(context)->audio_service_.PopPacketFromSendQueue();
        },
        [](void* context, const AudioStreamPacket& packet) {
            return static_cast<Application*>(context)->audio_service_.IsCurrentChatUplink(packet);
        },
        [](void* context) {
            auto* app = static_cast<Application*>(context);
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        },
        [](void* context, ConnectionSource source, uint64_t protocol, uint32_t connect) {
            return static_cast<Application*>(context)->IsChatConnectionCurrent(source, protocol,
                                                                               connect);
        }};
    if (!chat_outbound_worker_.Publish(activation)) {
        protocol_work_lifetime_.Release(reservation);
        chat_outbound_fault_ = true;
        return Result::Failed;
    }
    chat_outbound_reservation_ = reservation;
    chat_outbound_generation_ = generation;
    chat_outbound_protocol_generation_ = protocol_generation;
    chat_outbound_connection_epoch_ = connection_epoch;
    chat_outbound_fault_ = false;
    NotifyChatOutbound();
    return Result::Sent;
}

ChatOutboundMailbox::Result Application::SubmitChatOutbound(ChatOutboundMailbox::Job& job) {
    using Result = ChatOutboundMailbox::Result;
    if (!chat_outbound_task_ || chat_outbound_fault_)
        return Result::Failed;
    if (!chat_outbound_reservation_ || !chat_outbound_generation_ ||
        protocol_work_lifetime_.Pending())
        return Result::Stale;
    if (job.request_id == 0) {
        if (chat_outbound_request_id_ == UINT64_MAX) {
            chat_outbound_fault_ = true;
            RetireChatOutbound();
            return Result::Failed;
        }
        job.request_id = ++chat_outbound_request_id_;
        job.generation = chat_outbound_generation_;
        job.protocol_generation = chat_outbound_protocol_generation_;
        job.connection_epoch = chat_outbound_connection_epoch_;
        if (!job.deadline_us) {
            if (job.kind == ChatOutboundMailbox::Kind::DrainAck)
                return Result::Failed;
            job.deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
        }
    }
    if (job.generation != chat_outbound_generation_ ||
        job.request_id <= chat_outbound_last_admitted_id_ ||
        job.request_id > chat_outbound_request_id_ ||
        job.protocol_generation != chat_outbound_protocol_generation_ ||
        job.connection_epoch != chat_outbound_connection_epoch_)
        return Result::Stale;
    if (!job.deadline_us || static_cast<uint64_t>(esp_timer_get_time()) >= job.deadline_us)
        return Result::Failed;
    if (!chat_outbound_worker_.Submit(job))
        return Result::Busy;
    chat_outbound_last_admitted_id_ = job.request_id;
    NotifyChatOutbound();
    return Result::Sent;
}

void Application::RetireChatOutbound() {
    if (!chat_outbound_reservation_ || !chat_outbound_generation_)
        return;
    chat_outbound_worker_.AdvanceGeneration();
    chat_outbound_generation_ = 0;
    NotifyChatOutbound();
}

bool Application::PollChatOutbound(ChatOutboundMailbox::Completion* completion) {
    bool collected = false;
    if (chat_outbound_worker_.TakeAudioFailure()) {
        chat_outbound_fault_ = true;
        RetireChatOutbound();
    }
    if (protocol_work_lifetime_.Pending())
        RetireChatOutbound();
    ChatOutboundMailbox::Completion discarded;
    if (completion || !chat_outbound_generation_ || chat_control_intents_.Size() ||
        chat_connection_messages_.Size()) {
        collected = chat_outbound_worker_.Collect(completion ? *completion : discarded);
        if (collected)
            NotifyChatOutbound();
        if (collected && (chat_connection_messages_.Deliver(completion ? *completion : discarded) ||
                          DeliverChatControl(completion ? *completion : discarded)))
            collected = false;
    }
    if (chat_outbound_reservation_ && chat_outbound_worker_.TakeRetired()) {
        chat_connection_messages_.ObserveRetirement(chat_outbound_reservation_);
        protocol_work_lifetime_.Release(chat_outbound_reservation_);
        chat_outbound_reservation_ = 0;
        CompletePendingProtocolWork();
    }
    return collected;
}

bool Application::IsChatOutboundCompletionCurrent(
    const ChatOutboundMailbox::Completion& completion) const {
    return !completion.stale && chat_outbound_generation_ != 0 && protocol_ &&
           protocol_->CurrentConnectionEpoch() == completion.job.connection_epoch &&
           chat_outbound_worker_.IsCurrent(completion.job.generation) &&
           completion.job.protocol_generation == protocol_generation_.load() &&
           completion.job.protocol_generation == chat_outbound_protocol_generation_ &&
           completion.job.connection_epoch == chat_outbound_connection_epoch_ &&
           !protocol_work_lifetime_.Pending();
}

void Application::NotifyChatOutbound() {
    if (chat_outbound_task_)
        xTaskNotifyGive(chat_outbound_task_);
}

void Application::PollChatOutboundEvents(uint32_t bits) {
    if (bits & MAIN_EVENT_CLOCK_TICK)
        ++clock_ticks_;
    if (bits & MAIN_EVENT_CLOCK_TICK)
        McpServer::GetInstance().PollLessonAssetSyncCompletion();
    if (bits & (MAIN_EVENT_CHAT_OUTBOUND | MAIN_EVENT_CLOCK_TICK)) {
        PollChatSourceOpen(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatProtocolSignals();
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
        if (chat_playout_stamp_ && !chat_playout_recovery_)
            PollChatPlayout(static_cast<uint64_t>(esp_timer_get_time()));
        else
            PollChatOutbound();
        PollChatControls(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatConnectionMessages(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatUnpair(static_cast<uint64_t>(esp_timer_get_time()));
        PollChatInboundMessages();
        if (chat_cleanup_enabled_) {
            PollChatAudioCleanup();
            PollChatLessonCapture(static_cast<uint64_t>(esp_timer_get_time()));
            PollChatProtocolCleanup();
            if (bits & MAIN_EVENT_CLOCK_TICK)
                RetryChatAudioCleanup();
            PollChatReboot();
            PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
        }
    }
}

void Application::ChatOutboundTask(void* context) {
    auto* app = static_cast<Application*>(context);
    for (;;) {
        const bool retry = app->chat_outbound_worker_.RunOnce(esp_timer_get_time());
        ulTaskNotifyTake(pdTRUE, retry ? pdMS_TO_TICKS(10) : portMAX_DELAY);
    }
}

bool Application::RequestChatControl(ChatOutboundMailbox::Kind kind, int32_t argument,
                                     const std::string& payload, bool read_wake) {
    if (!IsSelectedNormalChatRoute())
        return false;
    if (chat_protocol_owned_.load() ||
        chat_source_connect_generation_.load() != connect_generation_.load())
        return false;
    ChatControlIntents::Intent intent;
    if (!chat_protocol_signals_->TrySource(intent.source))
        return false;
    intent.protocol_generation = protocol_generation_.load();
    intent.connect_generation = connect_generation_.load();
    intent.response_generation = speaking_generation_.load();
    intent.job.kind = kind;
    intent.job.argument = argument;
    intent.resolve_wake = read_wake;
    intent.job.deadline_us = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    const size_t listen_slot = chat_rearm_phase_ == ChatRearmPhase::Pending ? 1 : 0;
    if (chat_control_intents_.Size() + listen_slot >= 4 ||
        !intent.job.SetPayload(payload.data(), payload.size()) ||
        !chat_control_intents_.Push(intent)) {
        chat_protocol_infrastructure_fault_ = true;
        RecoverChatPlayout(215);
        return false;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return true;
}

void Application::PollChatControls(uint64_t now_us) {
    auto* intent = chat_control_intents_.Front();
    if (!intent)
        return;
    if (intent->outcome == ChatControlIntents::Outcome::Superseded ||
        intent->outcome == ChatControlIntents::Outcome::Failed) {
        if (!intent->admitted || intent->reservation != chat_outbound_reservation_)
            chat_control_intents_.Pop();
        return;
    }
    if (!chat_protocol_signals_ || !chat_protocol_signals_->MatchesSource(intent->source) ||
        intent->protocol_generation != protocol_generation_.load() ||
        intent->connect_generation != connect_generation_.load()) {
        chat_control_intents_.Supersede();
        RetireChatOutbound();
        chat_wake_read_pending_ = false;
        chat_wake_read_result_.reset();
        if (chat_wake_read_serial_ != UINT32_MAX)
            ++chat_wake_read_serial_;
        return;
    }
    if (now_us >= intent->job.deadline_us) {
        intent->outcome = ChatControlIntents::Outcome::Failed;
        RecoverChatPlayout(216);
        return;
    }
    if (intent->resolve_wake) {
        if (!chat_wake_read_result_) {
            if (!chat_wake_read_pending_) {
                if (chat_wake_read_serial_ == UINT32_MAX) {
                    RecoverChatPlayout(217);
                    return;
                }
                ++chat_wake_read_serial_;
                chat_wake_read_pending_ = true;
            }
            PollChatAudioCleanup();
            return;
        }
        if (!intent->job.SetPayload(chat_wake_read_result_->data(),
                                    chat_wake_read_result_->size())) {
            intent->outcome = ChatControlIntents::Outcome::Failed;
            chat_wake_read_result_.reset();
            RecoverChatPlayout(218);
            return;
        }
        chat_wake_read_result_.reset();
        intent->resolve_wake = false;
    }
    if (!intent->admitted) {
        const auto result = SubmitChatOutbound(intent->job);
        if (result == ChatOutboundMailbox::Result::Sent) {
            intent->admitted = true;
            intent->reservation = chat_outbound_reservation_;
        } else if (result != ChatOutboundMailbox::Result::Busy) {
            intent->outcome = ChatControlIntents::Outcome::Failed;
            RecoverChatPlayout(219);
        }
    }
}

bool Application::DeliverChatControl(const ChatOutboundMailbox::Completion& completion) {
    auto* intent = chat_control_intents_.Front();
    if (!intent || !intent->admitted || intent->job.request_id != completion.job.request_id ||
        intent->job.generation != completion.job.generation ||
        intent->job.protocol_generation != completion.job.protocol_generation ||
        intent->job.connection_epoch != completion.job.connection_epoch)
        return false;
    if (intent->outcome == ChatControlIntents::Outcome::Superseded ||
        intent->outcome == ChatControlIntents::Outcome::Failed)
        return true;
    intent->outcome = IsChatOutboundCompletionCurrent(completion) &&
                              completion.result == ChatOutboundMailbox::Result::Sent
                          ? ChatControlIntents::Outcome::Sent
                          : ChatControlIntents::Outcome::Failed;
    if (intent->outcome == ChatControlIntents::Outcome::Failed)
        RecoverChatPlayout(220);
    chat_control_intents_.Pop();
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND | MAIN_EVENT_STATE_CHANGED);
    return true;
}

bool Application::RetainChatActiveListen() {
    const bool listen = chat_rearm_admitted_ && !chat_rearm_delivery_;
    const bool ack = chat_playout_ack_admitted_ && !chat_playout_ready_;
    if (listen || ack) {
        ChatControlIntents::Intent active;
        active.source = chat_rearm_owner_.source;
        active.protocol_generation = chat_rearm_owner_.protocol_generation;
        active.connect_generation = chat_rearm_owner_.connect_generation;
        active.response_generation = speaking_generation_.load();
        active.job = listen ? chat_rearm_job_ : chat_playout_ack_;
        active.admitted = true;
        active.reservation = chat_outbound_reservation_;
        if (!chat_control_intents_.PrependActive(active)) {
            chat_protocol_infrastructure_fault_ = true;
            RecoverChatPlayout(221);
            return false;
        }
        chat_rearm_admitted_ = false;
        chat_playout_ack_admitted_ = false;
    }
    chat_rearm_phase_ = ChatRearmPhase::IdleComplete;
    return true;
}
