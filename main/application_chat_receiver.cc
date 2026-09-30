#include "application_internal.h"

Protocol::SourceCallbacks Application::MakeChatSourceCallbacks(
    uint64_t protocol_generation, std::shared_ptr<ChatProtocolSignals> signals) {
    // Install the complete immutable callback set before any Start/Open.
    Protocol::SourceCallbacks callbacks;
    callbacks.opened = [this, protocol_generation, signals, callback_protocol = protocol_.get()](
                           ConnectionSource source, uint64_t deadline_us) {
        const auto connect_generation = protocol_callback_connect_generation_.load();
        if (!signals || protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load() || connect_generation != connect_generation_.load())
            return;
        signals->PublishOpened(source, connect_generation, callback_protocol->server_sample_rate(),
                               deadline_us);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.adopted = [this, protocol_generation](ConnectionSource source) {
        return IsChatConnectionCurrent(source, protocol_generation,
                                       chat_source_connect_generation_.load()) &&
               (IsLessonVoiceRoute() || passive_ws_intent_.load() ||
                (!connect_in_flight_.load() && GetDeviceState() != kDeviceStateConnecting));
    };
    auto publish = [this, protocol_generation, signals](ConnectionSource source, uint32_t flag) {
        if (!signals || protocol_generation != protocol_generation_.load() ||
            chat_protocol_owned_.load(std::memory_order_acquire) ||
            protocol_callback_connect_generation_.load() != connect_generation_.load())
            return;
        ESP_LOGW(TAG, "chat_source_fault reason=transport flags=%lu",
                 static_cast<unsigned long>(flag));
        if (signals->PublishConnectionFault(source, protocol_callback_connect_generation_.load(),
                                            flag))
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    };
    callbacks.error = [publish](ConnectionSource source, const std::string&) {
        publish(source, ChatProtocolSignals::Error);
    };
    callbacks.closed = [publish](ConnectionSource source) {
        publish(source, ChatProtocolSignals::Closed);
    };
    callbacks.json = [this, protocol_generation, signals, callback_protocol = protocol_.get()](
                         ConnectionSource source, const cJSON* root, uint64_t lesson_epoch,
                         ConnectionReceipt receipt) {
        const ChatConnectionMessages::Owner owner{source, protocol_generation,
                                                  chat_source_connect_generation_.load()};
        if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation,
                                     owner.connect_generation))
            return;
        bool queued_json = false;
        try {
            const auto* message_type = cJSON_GetObjectItem(root, "type");
            if (lesson_asset_sync_quiet_.load() && cJSON_IsString(message_type) &&
                (strcmp(message_type->valuestring, "tts") == 0 ||
                 strcmp(message_type->valuestring, "stt") == 0))
                return;
            const auto* message_state = cJSON_GetObjectItem(root, "state");
#if CONFIG_TBOT_VOICE_DEMO
            // Presentation bursts must not occupy the bounded control queue.
            if (!IsLessonVoiceRoute() && cJSON_IsString(message_type) &&
                (strcmp(message_type->valuestring, "stt") == 0 ||
                 strcmp(message_type->valuestring, "llm") == 0 ||
                 (strcmp(message_type->valuestring, "tts") == 0 && cJSON_IsString(message_state) &&
                  strcmp(message_state->valuestring, "sentence_start") == 0))) {
                const auto* text = cJSON_GetObjectItem(root, "text");
                if (strcmp(message_type->valuestring, "llm") != 0 && cJSON_IsString(text)) {
                    signals->captions.Publish(
                        {source, protocol_generation, owner.connect_generation,
                         speaking_generation_.load(), 0},
                        strcmp(message_type->valuestring, "tts") == 0, text->valuestring);
                }
                return;
            }
#endif
            if (IsLessonVoiceRoute() && cJSON_IsString(message_type) &&
                strcmp(message_type->valuestring, "tts") == 0 && cJSON_IsString(message_state) &&
                (strcmp(message_state->valuestring, "start") == 0 ||
                 strcmp(message_state->valuestring, "stop") == 0)) {
                auto context = chat_inbound_messages_.Own(
                    root, {source, protocol_generation, chat_source_connect_generation_.load()},
                    receipt.received_us, lesson_epoch, callback_protocol->session_id());
                if (!context) {
                    const auto snapshot = chat_inbound_messages_.TrySnapshot();
                    ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
                             static_cast<unsigned>(snapshot.available),
                             static_cast<unsigned>(snapshot.queued),
                             static_cast<unsigned>(snapshot.outstanding));
                    ESP_LOGW(TAG, "chat_source_fault reason=lesson_json_admission");
                    signals->PublishConnectionFault(source, chat_source_connect_generation_.load(),
                                                    ChatProtocolSignals::Error);
                    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                    return;
                }
                if (!IsChatLessonRequestCurrent(context))
                    return;
                signals->lesson_audio_epoch = lesson_epoch;
                try {
                    DispatchIncomingJson(context->root.get(), lesson_epoch, true, context);
                } catch (...) {
                    FailChatRequest(context);
                }
                return;
            }
            HandleChatStart(signals, protocol_generation, source, root, receipt);
            HandleChatTerminalStop(signals, protocol_generation, source, root, receipt.received_us);
            const auto* type = cJSON_GetObjectItem(root, "type");
            const auto* state = cJSON_GetObjectItem(root, "state");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0 &&
                cJSON_IsString(state) &&
                (strcmp(state->valuestring, "start") == 0 ||
                 strcmp(state->valuestring, "stop") == 0))
                return;
            queued_json = true;
            using Admission = ChatInboundMessages::Admission;
            const auto session_id = callback_protocol->session_id();
            Admission outcome = Admission::Invalid;
            uint32_t retries = 0;
            const auto deadline_us = receipt.received_us <= UINT64_MAX - 250000ULL
                                         ? receipt.received_us + 250000ULL
                                         : UINT64_MAX;
            const auto admission_deadline_us =
                receipt.admission_deadline_us && receipt.admission_deadline_us < deadline_us
                    ? receipt.admission_deadline_us
                    : deadline_us;
            const auto current = [this, owner]() {
                return IsChatConnectionCurrent(owner.source, owner.protocol_generation,
                                               owner.connect_generation);
            };
            const std::function<bool()> can_publish = [&]() {
                const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
                return current() && now_us >= receipt.received_us && now_us < admission_deadline_us;
            };
            for (;;) {
                if (!current())
                    return;
                if (!can_publish())
                    break;
                outcome = chat_inbound_messages_.TryAdmit(root, owner, receipt.received_us,
                                                          lesson_epoch, session_id, can_publish);
                if (outcome == Admission::Accepted) {
                    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                    if (retries) {
                        const auto waited_us =
                            static_cast<uint64_t>(esp_timer_get_time()) - receipt.received_us;
                        ESP_LOGW(TAG,
                                 "chat_json_admission reason=json_admission outcome=%u retries=%lu "
                                 "wait_us_hi=%lu wait_us_lo=%lu",
                                 static_cast<unsigned>(outcome),
                                 static_cast<unsigned long>(retries),
                                 static_cast<unsigned long>(waited_us >> 32),
                                 static_cast<unsigned long>(static_cast<uint32_t>(waited_us)));
                    }
                    return;
                }
                if (outcome != Admission::Busy && outcome != Admission::Full)
                    break;
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
                ++retries;
                vTaskDelay(1);
            }
            if (!current())
                return;
            const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
            const auto waited_us = now_us >= receipt.received_us ? now_us - receipt.received_us : 0;
            const auto wait_hi = static_cast<unsigned long>(waited_us >> 32);
            const auto wait_lo = static_cast<unsigned long>(static_cast<uint32_t>(waited_us));
            const auto snapshot = chat_inbound_messages_.TrySnapshot();
            ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
                     static_cast<unsigned>(snapshot.available),
                     static_cast<unsigned>(snapshot.queued),
                     static_cast<unsigned>(snapshot.outstanding));
            ESP_LOGW(TAG, "chat_source_fault reason=json_admission outcome=%u retries=%lu "
                     "wait_us_hi=%lu wait_us_lo=%lu",
                     static_cast<unsigned>(outcome), static_cast<unsigned long>(retries), wait_hi,
                     wait_lo);
            signals->PublishConnectionFault(owner.source, owner.connect_generation,
                                            ChatProtocolSignals::Error);
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        } catch (...) {
            if (!IsChatConnectionCurrent(owner.source, owner.protocol_generation,
                                         owner.connect_generation))
                return;
            if (queued_json) {
                const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
                const auto waited_us =
                    now_us >= receipt.received_us ? now_us - receipt.received_us : 0;
                const auto snapshot = chat_inbound_messages_.TrySnapshot();
                ESP_LOGW(TAG, "chat_json_queue available=%u queued=%u outstanding=%u",
                         static_cast<unsigned>(snapshot.available),
                         static_cast<unsigned>(snapshot.queued),
                         static_cast<unsigned>(snapshot.outstanding));
                ESP_LOGW(TAG, "chat_source_fault reason=json_admission outcome=%u retries=0 "
                         "wait_us_hi=%lu wait_us_lo=%lu",
                         static_cast<unsigned>(ChatInboundMessages::Admission::NoMemory),
                         static_cast<unsigned long>(waited_us >> 32),
                         static_cast<unsigned long>(static_cast<uint32_t>(waited_us)));
            } else
                ESP_LOGW(TAG, "chat_source_fault reason=json_exception");
            signals->PublishConnectionFault(owner.source, owner.connect_generation,
                                            ChatProtocolSignals::Error);
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        }
    };
    callbacks.audio = [this, protocol_generation, signals](
                          ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet) {
        if (IsLessonVoiceRoute()) {
            HandleChatLessonAudio(signals, protocol_generation, source, std::move(packet));
            return;
        }
        HandleChatAudio(signals, protocol_generation, source, std::move(packet));
    };
    return callbacks;
}

bool Application::IsLessonVoiceRoute() const {
    return lesson_runtime_active_.load() || lesson_interactive_listen_pending_.load() ||
           lesson_interactive_listening_active_.load() ||
           lesson_terminal_audio_generation_.load() != 0;
}

bool Application::IsChatLessonRequestCurrent(const ChatRequestContext& context) const {
    return !context || (IsChatRequestCurrent(context) &&
                        context->lesson_epoch == lesson_transport_epoch_gate_.PublishedEpoch());
}

void Application::ScheduleChatLesson(ChatRequestContext context, std::function<void()> callback) {
    if (!IsChatLessonRequestCurrent(context))
        return;
    try {
        Schedule([this, context, callback = std::move(callback)]() {
            if (!IsChatLessonRequestCurrent(context))
                return;
            try {
                callback();
            } catch (...) {
                if (!context)
                    throw;
                FailChatRequest(context);
            }
        });
    } catch (...) {
        if (!context)
            throw;
        FailChatRequest(context);
    }
}

void Application::HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
                                        uint64_t protocol_generation, ConnectionSource source,
                                        std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet || !signals ||
        !IsChatConnectionCurrent(source, protocol_generation,
                                 chat_source_connect_generation_.load()) ||
        signals->lesson_audio_epoch != lesson_transport_epoch_gate_.PublishedEpoch() ||
        lesson_asset_sync_quiet_.load())
        return;
    if (!lesson_audio_playout_.AllowsAudio(speaking_generation_.load()))
        return;
    if (GetDeviceState() == kDeviceStateSpeaking || tts_audio_accepting_.load()) {
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
        packet->generation = speaking_generation_.load();
        packet->conversation_audio = true;
        audio_service_.PushPacketToDecodeQueue(std::move(packet));
    }
}

void Application::HandleChatStart(const std::shared_ptr<ChatProtocolSignals>& signals,
                                  uint64_t protocol_generation, ConnectionSource source,
                                  const cJSON* root, ConnectionReceipt receipt) {
    const auto* type = cJSON_GetObjectItem(root, "type");
    const auto* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "tts") != 0 || !cJSON_IsString(state) ||
        strcmp(state->valuestring, "start") != 0 || !signals || !signals->MatchesSource(source) ||
        chat_protocol_owned_.load() || protocol_generation != protocol_generation_.load() ||
        chat_source_connect_generation_.load() != connect_generation_.load())
        return;
    signals->start_audio = {};
    ChatStartHandoff::Request request{source, protocol_generation, connect_generation_.load(),
                                      0,      receipt.received_us, receipt.admission_deadline_us};
    if (!signals->start.Publish(request)) {
        ESP_LOGW(TAG, "chat_source_fault reason=start_publication");
        signals->PublishConnectionFault(source, chat_source_connect_generation_.load(),
                                        ChatProtocolSignals::Error);
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
        return;
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    // The receiver gate serializes following audio. Only app admission is
    // awaited here: decoder cleanup/readiness is deliberately not a condition.
    for (;;) {
        const auto now = static_cast<uint64_t>(esp_timer_get_time());
        ChatStartHandoff::Admission admission;
        if (signals->start.TryAdmission(request, now, admission) &&
            signals->start.Confirm(request, now)) {
            signals->start_audio = {source, protocol_generation, request.connect_generation,
                                    admission.response_generation, admission.reset_token};
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
            return;
        }
        const bool expired = signals->start.Expired(request, now);
        if (expired || !signals->MatchesSource(source) ||
            request.connect_generation != connect_generation_.load() ||
            protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load()) {
            unsigned termination_site = 302;
            if (expired)
                termination_site = 301;
            const auto elapsed_us = now >= request.received_us ? now - request.received_us : 0;
            ESP_LOGW(TAG, "chat_start_receiver_end site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                     termination_site, static_cast<unsigned long>(elapsed_us >> 32),
                     static_cast<unsigned long>(static_cast<uint32_t>(elapsed_us)));
            break;
        }
        vTaskDelay(1);
    }
    signals->start.Expire(request);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::HandleChatAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
                                  uint64_t protocol_generation, ConnectionSource source,
                                  std::unique_ptr<AudioStreamPacket> packet) {
    if (!signals || !packet)
        return;
    const auto response = signals->start_audio;
    if (!signals->MatchesSource(source) || chat_protocol_owned_.load() ||
        response.source.source_id != source.source_id ||
        response.source.connection_epoch != source.connection_epoch ||
        response.protocol_generation != protocol_generation ||
        protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        !response.response_generation ||
        response.response_generation != speaking_generation_.load() ||
        !tts_audio_accepting_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(response.reset_token))
        return;
    packet->generation = response.response_generation;
    packet->conversation_audio = true;
    if (audio_service_.PushChatPacketToDecodeQueue(std::move(packet), response.reset_token) &&
        response.response_generation == speaking_generation_.load() &&
        audio_service_.IsCurrentChatPlaybackReset(response.reset_token)) {
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
    }
}

