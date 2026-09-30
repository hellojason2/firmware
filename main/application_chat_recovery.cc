#include "application_internal.h"

bool Application::IsSelectedNormalChatRoute() const {
    return chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
           !lesson_runtime_active_.load() && !chat_protocol_owned_.load() &&
           chat_source_connect_generation_.load() == connect_generation_.load();
}

void Application::CancelChatRecovery(uint32_t outcome) {
    if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None)
        return;
    ESP_LOGI(TAG, "chat_recovery outcome=%lu", static_cast<unsigned long>(outcome));
    if (outcome == 2 && chat_recovery_.protocol_generation == protocol_generation_.load() &&
        chat_recovery_.connect_generation == connect_generation_.load()) {
        if (chat_recovery_.lesson_generation == lesson_runtime_generation_.load())
            online_intent_.store(false);
        if (chat_recovery_.opening && connect_generation_.load() != UINT32_MAX)
            ++connect_generation_;
        connect_in_flight_.store(false);
        connect_attempt_active_.store(false);
        CancelConnectWatchdog();
    }
    chat_recovery_ = {};
}

bool Application::RetainChatRecovery(ChatRecoveryIntent::Kind kind, ListeningMode mode) {
    if (IsWifiConfigEntryPending())
        return false;
    using Kind = ChatRecoveryIntent::Kind;
    const auto state = GetDeviceState();
    if (!chat_cleanup_enabled_ || !chat_protocol_signals_ ||
        !chat_protocol_signals_->SourceSelected() || lesson_runtime_active_.load() ||
        lesson_asset_sync_quiet_.load() || chat_unpair_context_ || reset_pending_.load() ||
        protocol_reinit_pending_.load() || reboot_pending_.load() ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting ||
        chat_protocol_infrastructure_fault_ || (!protocol_ && !chat_protocol_owned_.load()) ||
        (protocol_work_lifetime_.Pending() && !online_intent_.load() &&
         chat_recovery_.kind == Kind::None)) {
        ESP_LOGI(TAG, "chat_recovery outcome=5");
        return false;
    }
    auto& intent = chat_recovery_;
    if (intent.kind != Kind::None &&
        (intent.protocol_generation != protocol_generation_.load() ||
         intent.connect_generation != connect_generation_.load() ||
         intent.lesson_generation != lesson_runtime_generation_.load())) {
        CancelChatRecovery();
        if (kind == Kind::Background)
            return false;
    }
    if (intent.kind == Kind::None) {
        intent.kind = Kind::Background;
        intent.protocol_generation = protocol_generation_.load();
        intent.connect_generation = connect_generation_.load();
        intent.lesson_generation = lesson_runtime_generation_.load();
        intent.mode = mode;
    }
    if (kind != Kind::Background && intent.kind == Kind::Background) {
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
        if (now > UINT64_MAX - 10000000ULL) {
            ESP_LOGI(TAG, "chat_recovery outcome=3");
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
            return false;
        }
        intent.kind = kind;
        intent.received_us = now;
        intent.deadline_us = now + 10000000ULL;
        intent.mode = mode;
        intent.ready = !intent.opening;
    }
    online_intent_.store(true);
    passive_ws_intent_.store(false);
    microphone_uplink_authorized_.store(false);
    ESP_LOGI(TAG, "chat_recovery outcome=1");
    return true;
}

void Application::PollChatRecovery(uint64_t now_us) {
    using Kind = ChatRecoveryIntent::Kind;
    auto& intent = chat_recovery_;
    if (intent.kind == Kind::None)
        return;
    const auto state = GetDeviceState();
    if (intent.protocol_generation != protocol_generation_.load() ||
        intent.lesson_generation != lesson_runtime_generation_.load() ||
        intent.connect_generation != connect_generation_.load() || !online_intent_.load() ||
        lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load() || chat_unpair_context_ ||
        reset_pending_.load() || protocol_reinit_pending_.load() || reboot_pending_.load() ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting ||
        chat_protocol_infrastructure_fault_) {
        CancelChatRecovery();
        return;
    }
    if (intent.kind != Kind::Background &&
        (now_us < intent.received_us || now_us >= intent.deadline_us)) {
        ESP_LOGI(TAG, "chat_recovery outcome=3");
        intent.kind = Kind::Background;
        intent.received_us = intent.deadline_us = 0;
        SetDeviceState(kDeviceStateIdle);
        RearmClaimedIdleWakeWord();
    }
    if (intent.opening || protocol_work_lifetime_.Pending() || chat_protocol_owned_.load() ||
        chat_protocol_state_.load() || connect_in_flight_.load() || !protocol_)
        return;
    ConnectionSource source;
    if (intent.adopted && chat_protocol_signals_->TrySource(source) &&
        chat_source_connect_generation_.load() == intent.connect_generation &&
        protocol_->CurrentConnectionEpoch() == source.connection_epoch) {
        if (intent.kind == Kind::Background) {
            chat_recovery_ = {};
            SetDeviceState(kDeviceStateIdle);
            RearmClaimedIdleWakeWord();
            return;
        }
        // Old controls keep their original source. Wait for bounded retirement,
        // then construct new controls with the original explicit deadline.
        if (chat_control_intents_.Size() ||
            (chat_outbound_reservation_ && !chat_outbound_generation_))
            return;
        const auto accepted = intent;
        chat_recovery_ = {};
        ESP_LOGI(TAG, "chat_recovery outcome=6");
        SetDeviceState(kDeviceStateIdle);
        try {
            if (accepted.kind == Kind::Wake)
                HandleChatWake(std::string(accepted.wake_text.data(), accepted.wake_size),
                               accepted.read_wake);
            else
                BeginChatListen(accepted.mode, ChatListenOrigin::User);
        } catch (...) {
            RequestChatAudioCleanup(speaking_generation_.load(), false, false, true);
            ESP_LOGI(TAG, "chat_recovery outcome=5");
            return;
        }
        if (auto* wake = chat_control_intents_.Front())
            wake->job.deadline_us = accepted.deadline_us;
        if (chat_rearm_phase_ == ChatRearmPhase::Pending) {
            chat_listen_received_us_ = accepted.received_us;
            chat_rearm_job_.deadline_us = accepted.deadline_us;
        }
        return;
    }
    if (!intent.ready || protocol_work_lifetime_.Busy())
        return;
    if (protocol_->IsAudioChannelOpened()) {
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        PollChatProtocolCleanup();
        return;
    }
    if (connect_generation_.load() == UINT32_MAX) {
        CancelChatRecovery();
        return;
    }
    intent.ready = false;
    intent.opening = true;
    intent.attempted = true;
    intent.adopted = false;
    intent.connect_generation = ++connect_generation_;
    connect_in_flight_.store(true);
    connect_attempt_active_.store(true);
    reconnect_resume_listening_.store(false);
    SetDeviceState(kDeviceStateConnecting);
    ArmConnectWatchdog();
    auto* context = new (std::nothrow)
        ConnectContext{this, intent.mode, intent.connect_generation, std::string(), false, false};
    if (!context || !StartOpenChannelWorker(context)) {
        delete context;
        CompleteChatRecoveryOpen(intent.connect_generation, false);
    }
}

bool Application::CompleteChatRecoveryOpen(uint32_t generation, bool success) {
    if (chat_recovery_.kind == ChatRecoveryIntent::Kind::None || !chat_recovery_.opening ||
        chat_recovery_.connect_generation != generation ||
        generation != connect_generation_.load() ||
        chat_recovery_.protocol_generation != protocol_generation_.load())
        return false;
    if (chat_recovery_.lesson_generation != lesson_runtime_generation_.load()) {
        CancelChatRecovery();
        return true;
    }
    chat_recovery_.opening = false;
    connect_in_flight_.store(false);
    connect_attempt_active_.store(false);
    CancelConnectWatchdog();
    if (success) {
        chat_recovery_.ready = false;
        reconnect_attempt_ = 0;
        PollChatRecovery(static_cast<uint64_t>(esp_timer_get_time()));
    } else {
        backend_offline_.store(true);
        SetDeviceState(kDeviceStateIdle);
        ScheduleReconnect(chat_recovery_.mode, false);
    }
    return true;
}

bool Application::RequestChatLessonCapture() {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->SourceSelected() ||
        !IsLessonVoiceRoute())
        return false;
    microphone_uplink_authorized_.store(false);
    ConnectionSource source;
    if (!chat_protocol_signals_->TrySource(source))
        return true;
    chat_lesson_capture_owner_ = {source, protocol_generation_.load(), connect_generation_.load()};
    chat_lesson_capture_epoch_ = lesson_transport_epoch_gate_.PublishedEpoch();
    chat_lesson_capture_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    chat_audio_fault_ = false;
    chat_lesson_capture_token_ = RequestChatAudioCleanup(
        speaking_generation_.load(), false, true, false, false, false, ChatWakePolicy::Listening);
    return true;
}

void Application::PollChatLessonCapture(uint64_t now_us) {
    const auto token = chat_lesson_capture_token_;
    if (!token)
        return;
    if (!IsLessonVoiceRoute() || GetDeviceState() != kDeviceStateListening ||
        lesson_asset_sync_quiet_.load() ||
        chat_lesson_capture_epoch_ != lesson_transport_epoch_gate_.PublishedEpoch() ||
        !IsChatConnectionCurrent(chat_lesson_capture_owner_.source,
                                 chat_lesson_capture_owner_.protocol_generation,
                                 chat_lesson_capture_owner_.connect_generation) ||
        chat_audio_desired_.revoked != token) {
        chat_lesson_capture_token_ = 0;
        return;
    }
    if (now_us >= chat_lesson_capture_deadline_us_ || chat_audio_fault_) {
        chat_lesson_capture_token_ = 0;
        ESP_LOGW(TAG, "chat_source_fault reason=lesson_capture");
        chat_protocol_signals_->PublishConnectionFault(
            chat_lesson_capture_owner_.source, chat_lesson_capture_owner_.connect_generation,
            ChatProtocolSignals::Error);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        return;
    }
    if (chat_audio_prepared_ != token || !audio_service_.ArmChatUplink(token, false))
        return;
    chat_lesson_capture_token_ = 0;
    microphone_uplink_authorized_.store(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
}

void Application::PollChatInboundMessages() {
    for (size_t count = 0; count < 4; ++count) {
        // A START can arrive during the previous display update. Admit it
        // before another update consumes the receiver's 250 ms deadline.
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
        auto next = chat_inbound_messages_.TryTake();
        if (next.status == ChatInboundMessages::ReadStatus::Busy) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            return;
        }
        if (next.status == ChatInboundMessages::ReadStatus::Empty)
            return;
        auto context = std::move(next.context);
        if (!IsChatRequestCurrent(context))
            continue;
        if (static_cast<uint64_t>(esp_timer_get_time()) >= context->deadline_us) {
            FailChatRequest(context);
            continue;
        }
        try {
            DispatchIncomingJson(context->root.get(), context->lesson_epoch, true, context);
        } catch (...) {
            FailChatRequest(context);
        }
        PollChatStart(static_cast<uint64_t>(esp_timer_get_time()));
    }
    if (chat_inbound_messages_.Pending())
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::FailChatRequest(const ChatRequestContext& context) {
    if (!context || !IsChatRequestCurrent(context))
        return;
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    if (!signals || !signals->MatchesSource(context->owner.source))
        return;
    ESP_LOGW(TAG, "chat_source_fault reason=request_failed");
    signals->PublishConnectionFault(context->owner.source, context->owner.connect_generation,
                                    ChatProtocolSignals::Error);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

bool Application::IsChatConnectionCurrent(ConnectionSource source, uint64_t protocol_generation,
                                          uint32_t connect_generation) const {
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    return signals && signals->MatchesSource(source) && !chat_protocol_owned_.load() &&
           protocol_generation == protocol_generation_.load() &&
           connect_generation == connect_generation_.load();
}

bool Application::TakeChatCaption(ChatCaptionMailbox::Message& caption) {
#if CONFIG_TBOT_VOICE_DEMO
    const auto signals = std::atomic_load(&chat_protocol_signals_);
    if (!signals || !signals->captions.TryTake(caption))
        return false;
    const auto state = GetDeviceState();
    return !IsLessonVoiceRoute() && !lesson_asset_sync_quiet_.load() &&
           (state == kDeviceStateSpeaking || state == kDeviceStateListening) &&
           caption.owner.response_generation == speaking_generation_.load() &&
           IsChatConnectionCurrent(caption.owner.source, caption.owner.protocol_generation,
                                   caption.owner.connect_generation);
#else
    (void)caption;
    return false;
#endif
}

bool Application::IsChatRequestCurrent(const ChatRequestContext& context) const {
    return !context ||
           IsChatConnectionCurrent(context->owner.source, context->owner.protocol_generation,
                                   context->owner.connect_generation);
}

uint64_t Application::RequestChatConnectionText(const std::string& text, ChatRequestContext context,
                                                uint64_t received_us,
                                                std::shared_ptr<std::atomic<bool>> authorization) {
    ChatConnectionMessages::Owner owner;
    if (context)
        owner = context->owner;
    else {
        if (!chat_protocol_signals_ || !chat_protocol_signals_->TrySource(owner.source))
            return 0;
        owner.protocol_generation = protocol_generation_.load();
        owner.connect_generation = connect_generation_.load();
    }
    if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation, owner.connect_generation))
        return 0;
    const auto id = chat_connection_messages_.Admit(
        owner, text, received_us ? received_us : static_cast<uint64_t>(esp_timer_get_time()),
        std::move(authorization));
    if (!id) {
        ESP_LOGW(TAG, "chat_source_fault reason=reply_admission");
        chat_protocol_signals_->PublishConnectionFault(owner.source, owner.connect_generation,
                                                       ChatProtocolSignals::Error);
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return id;
}

void Application::PollChatConnectionMessages(uint64_t now_us) {
    auto* record = chat_connection_messages_.Front();
    if (!record) {
        if (passive_ws_intent_.load() && !chat_control_intents_.Size() &&
            chat_rearm_phase_ != ChatRearmPhase::Pending && !chat_playout_stamp_)
            RetireChatOutbound();
        return;
    }
    using Outcome = ChatConnectionMessages::Outcome;
    using Result = ChatOutboundMailbox::Result;
    if (!IsChatConnectionCurrent(record->owner.source, record->owner.protocol_generation,
                                 record->owner.connect_generation) ||
        (record->outcome == Outcome::Pending && record->authorization &&
         !record->authorization->load(std::memory_order_acquire))) {
        record->outcome = Outcome::Cancelled;
        if (record->submitted)
            RetireChatOutbound();
    }
    if (record->outcome == Outcome::Pending && now_us >= record->deadline_us) {
        record->outcome = Outcome::Failed;
        if (record->authorization)
            record->authorization->store(false, std::memory_order_release);
        if (record->submitted)
            RetireChatOutbound();
    }
    if (record->outcome != Outcome::Pending) {
        if (record->outcome == Outcome::Failed && record->authorization && !record->submitted) {
            ESP_LOGW(TAG, "chat_source_fault reason=playout_receipt_delivery");
            chat_protocol_signals_->PublishConnectionFault(
                record->owner.source, record->owner.connect_generation, ChatProtocolSignals::Error);
        }
        if (record->id == chat_unpair_id_)
            chat_unpair_completed_ = !record->submitted;
        if (!record->submitted && record->id == chat_passive_ping_id_) {
            chat_passive_ping_id_ = 0;
            if (record->outcome == Outcome::Failed) {
                ESP_LOGW(TAG, "chat_source_fault reason=ping_delivery");
                chat_protocol_signals_->PublishConnectionFault(record->owner.source,
                                                               record->owner.connect_generation,
                                                               ChatProtocolSignals::Error);
            }
        }
        if (!record->submitted)
            chat_connection_messages_.Pop();
        return;
    }
    if (record->submitted || chat_control_intents_.Size() || chat_start_obsolete_reservation_)
        return;
    if (!chat_outbound_generation_) {
        if (chat_outbound_reservation_)
            return;
        if (ActivateChatOutbound(record->owner.source.connection_epoch) != Result::Sent)
            return;
    }
    auto& job = record->physical;
    job.kind = ChatOutboundMailbox::Kind::FullText;
    job.source = record->owner.source;
    job.connect_generation = record->owner.connect_generation;
    job.full_text = record->payload;
    job.authorization = record->authorization;
    job.deadline_us = record->deadline_us;
    const auto result = SubmitChatOutbound(job);
    if (result == Result::Sent) {
        record->submitted = true;
        record->reservation = chat_outbound_reservation_;
    } else if (result == Result::Busy) {
        // No admission occurred; a newer control may overtake this attempt.
        record->physical = {};
    } else {
        record->outcome = Outcome::Failed;
        if (record->authorization)
            record->authorization->store(false, std::memory_order_release);
    }
}

bool Application::MaintainChatPassiveLiveness() {
    ConnectionSource source;
    if (!protocol_ || !chat_protocol_signals_ || !chat_protocol_signals_->TrySource(source))
        return false;
    if (chat_passive_ping_id_)
        return true;
    const int state = protocol_->ObserveChatPassiveLiveness(source);
    if (state < 0) {
        ESP_LOGW(TAG, "chat_source_fault reason=passive_liveness");
        chat_protocol_signals_->PublishConnectionFault(
            source, chat_source_connect_generation_.load(), ChatProtocolSignals::Error);
        return false;
    }
    if (state == 0 || chat_connection_messages_.Size() >= 2)
        return true;
    chat_passive_ping_id_ = RequestChatConnectionText("{\"type\":\"ping\"}");
    return chat_passive_ping_id_ != 0;
}

void Application::BeginChatUnpair(const cJSON* root, ChatRequestContext context) {
    if (!context || !IsChatRequestCurrent(context) || chat_unpair_context_ ||
        lesson_runtime_active_.load() || lesson_asset_sync_quiet_.load())
        return;
    chat_unpair_context_ = context;
    CancelChatRecovery();
    const auto received_us = static_cast<uint64_t>(esp_timer_get_time());
    chat_unpair_deadline_us_ = received_us + 10000000ULL;
    chat_unpair_id_ = 0;
    chat_unpair_completed_ = false;
    const auto* request_id = cJSON_GetObjectItem(root, "request_id");
    if (!cJSON_IsString(request_id) || !request_id->valuestring || !request_id->valuestring[0] ||
        std::strlen(request_id->valuestring) > 64) {
        chat_unpair_completed_ = true;
        return;
    }
    try {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> ack(cJSON_CreateObject(), cJSON_Delete);
        if (!ack || !cJSON_AddStringToObject(ack.get(), "type", "system_ack") ||
            !cJSON_AddStringToObject(ack.get(), "command", "unpair") ||
            !cJSON_AddStringToObject(ack.get(), "request_id", request_id->valuestring))
            return;
        std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(ack.get()),
                                                             cJSON_free);
        if (encoded)
            chat_unpair_id_ = RequestChatConnectionText(encoded.get(), context, received_us);
    } catch (...) {
        // Keep the original teardown deadline when ACK allocation/admission fails.
    }
}

void Application::PollChatUnpair(uint64_t now_us) {
    if (!chat_unpair_context_)
        return;
    if (!IsChatRequestCurrent(chat_unpair_context_) || lesson_runtime_active_.load() ||
        lesson_asset_sync_quiet_.load()) {
        chat_unpair_context_.reset();
        chat_unpair_id_ = 0;
        return;
    }
    if (!chat_unpair_completed_ && now_us < chat_unpair_deadline_us_)
        return;
    auto context = std::move(chat_unpair_context_);
    chat_unpair_id_ = 0;
    EnterRepairPairingMode(std::move(context));
}
