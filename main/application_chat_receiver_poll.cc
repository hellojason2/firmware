#include "application_internal.h"

void Application::RecoverChatStart(const ChatStartHandoff::Request& request, uint32_t site) {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->start.Current(request) ||
        !chat_protocol_signals_->MatchesSource(request.source) || chat_protocol_owned_.load() ||
        request.protocol_generation != protocol_generation_.load() ||
        request.connect_generation != connect_generation_.load() ||
        chat_start_failed_serial_ == request.serial) return;
    ESP_LOGW(TAG, "chat_recovery site=%u", static_cast<unsigned>(site));
    chat_start_failed_serial_ = request.serial;
    chat_rearm_voice_intent_ = false;
    chat_playout_controller_.Cancel();
    chat_playout_recovery_ = true;
    chat_playout_ready_ = false;
    RetireChatOutbound();
    tts_audio_accepting_.store(false);
    auto generation = speaking_generation_.load();
    if (generation != UINT32_MAX)
        speaking_generation_.store(++generation);
    RequestChatAudioCleanup(generation, true, false, false);
    chat_rearm_phase_ = ChatRearmPhase::Recovery;
    chat_rearm_owner_ = {request.source, request.protocol_generation, request.connect_generation,
                         generation, chat_audio_reset_serial_};
    chat_rearm_signals_ = chat_protocol_signals_;
    chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    microphone_uplink_authorized_.store(false);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::PollChatStart(uint64_t now_us) {
    if (!chat_protocol_signals_)
        return;
    auto& handoff = chat_protocol_signals_->start;
    ChatStartHandoff::Request request;
    if (!handoff.TryRequest(request) || !chat_protocol_signals_->MatchesSource(request.source) ||
        request.protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load() ||
        request.connect_generation != connect_generation_.load())
        return;
    // The receiver may publish START after the caller samples the poll clock.
    if (now_us < request.received_us)
        now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (request.serial == UINT32_MAX ||
        (!handoff.Confirmed(request) && handoff.Expired(request, now_us))) {
        RecoverChatStart(request, 101);
        return;
    }
    if (handoff.Confirmed(request) && chat_start_handled_serial_ == request.serial &&
        chat_start_failed_serial_ != request.serial &&
        chat_start_effects_serial_ != request.serial) {
        const auto state = GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateListening &&
            state != kDeviceStateSpeaking) {
            RecoverChatStart(request, 102);
            return;
        }
        chat_start_effects_serial_ = request.serial;
        speaking_arm_dispatch_.BeginResponse(speaking_generation_.load());
        aborted_ = false;
        last_speaking_activity_ms_.store(now_us / 1000);
        SetDeviceState(kDeviceStateSpeaking);
        ArmSpeakingTimeout();
    }
    if (chat_start_handled_serial_ == request.serial || chat_start_failed_serial_ == request.serial)
        return;
    chat_start_handled_serial_ = request.serial;
    chat_control_intents_.Supersede();
    if (chat_wake_read_serial_ != UINT32_MAX)
        ++chat_wake_read_serial_;
    chat_wake_read_pending_ = false;
    chat_wake_read_result_.reset();
    if (chat_protocol_infrastructure_fault_ || chat_outbound_fault_ ||
        chat_reboot_audio_requested_ || protocol_work_lifetime_.Pending() ||
        (chat_protocol_fault_ && chat_protocol_fault_generation_ == request.protocol_generation &&
          chat_protocol_fault_era_ == chat_protocol_signals_->Capture())) {
        RecoverChatStart(request, 103);
        return;
    }
    // Do not collect outbound here: final retirement can invoke protocol work.
    // A single retired generation bounds storage across arbitrarily many STARTs.
    if (chat_outbound_generation_ && chat_outbound_reservation_) {
        chat_start_obsolete_generation_ = chat_outbound_generation_;
        chat_start_obsolete_reservation_ = chat_outbound_reservation_;
        chat_start_obsolete_ack_ = chat_rearm_admitted_ ? chat_rearm_job_ : chat_playout_ack_;
        RetireChatOutbound();
    }
    auto generation = speaking_generation_.load();
    if (generation >= UINT32_MAX - 1) {
        RecoverChatStart(request, 104);
        return;
    }
    speaking_generation_.store(++generation);
    const auto reset = RequestChatPlaybackCleanup(generation);
    if (listening_mode_ != kListeningModeRealtime) {
        microphone_uplink_authorized_.store(false);
        RequestChatAudioCleanup(generation, false, false, false);
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
    }
    const ChatPlayoutIntake::Response response{request.source, request.protocol_generation,
                                               request.connect_generation, generation, reset};
    tts_audio_accepting_.store(true);
    if (!reset || reset == UINT32_MAX || !EstablishChatPlayoutResponse(response) ||
        !handoff.Admit(request, generation, reset, static_cast<uint64_t>(esp_timer_get_time()))) {
        RecoverChatStart(request, 105);
        return;
    }
}

bool Application::EstablishChatPlayoutResponse(const ChatPlayoutIntake::Response& response) {
    if (!chat_protocol_signals_ || !chat_protocol_signals_->MatchesSource(response.source) ||
        chat_protocol_owned_.load() ||
        response.protocol_generation != protocol_generation_.load() ||
        response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load() ||
        !audio_service_.IsCurrentChatPlaybackReset(response.reset_token) ||
        chat_playout_unhandled_completion_ ||
        (chat_playout_ack_admitted_ && !chat_playout_ready_ && chat_outbound_reservation_ &&
         chat_start_obsolete_reservation_ != chat_outbound_reservation_))
        return false;
    const auto stamp = chat_protocol_signals_->intake.Establish(response);
    if (!stamp)
        return false;
    const auto state = GetDeviceState();
    const bool transfer_voice_intent =
        chat_rearm_voice_intent_ &&
        (chat_rearm_phase_ == ChatRearmPhase::Pending ||
         chat_rearm_phase_ == ChatRearmPhase::None || chat_rearm_phase_ == ChatRearmPhase::Armed) &&
        chat_rearm_signals_ == chat_protocol_signals_ &&
        chat_rearm_owner_.source.source_id == response.source.source_id &&
        chat_rearm_owner_.source.connection_epoch == response.source.connection_epoch &&
        chat_rearm_owner_.protocol_generation == response.protocol_generation &&
        chat_rearm_owner_.connect_generation == response.connect_generation &&
        online_intent_.load() && !passive_ws_intent_.load() && !lesson_runtime_active_.load() &&
        (state == kDeviceStateSpeaking || state == kDeviceStateListening);
    chat_rearm_voice_intent_ = transfer_voice_intent;
    chat_rearm_phase_ = ChatRearmPhase::None;
    chat_listen_origin_ = ChatListenOrigin::Drain;
    chat_rearm_owner_ = response;
    chat_rearm_signals_ = chat_protocol_signals_;
    chat_rearm_source_era_ = chat_protocol_signals_->Capture();
    chat_rearm_job_ = {};
    chat_rearm_delivery_.reset();
    chat_rearm_prepared_ = 0;
    chat_rearm_admitted_ = false;
    chat_playout_controller_ = {};
    chat_playout_response_ = response;
    chat_playout_stamp_ = stamp;
    chat_playout_begun_ = chat_playout_ready_ = chat_playout_recovery_ = false;
    chat_playout_cancelled_ = false;
    chat_playout_stop_ = {};
    chat_playout_ack_ = {};
    chat_playout_ack_controller_id_ = 0;
    chat_playout_ack_admitted_ = false;
    return true;
}

void Application::HandleChatTerminalStop(const std::shared_ptr<ChatProtocolSignals>& signals,
                                         uint64_t protocol_generation, ConnectionSource source,
                                         const cJSON* root, uint64_t received_us) {
    if (!received_us)
        received_us = static_cast<uint64_t>(esp_timer_get_time());
    const auto* type = cJSON_GetObjectItem(root, "type");
    const auto* state = cJSON_GetObjectItem(root, "state");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "tts") != 0 || !cJSON_IsString(state) ||
        strcmp(state->valuestring, "stop") != 0)
        return;
    ChatPlayoutIntake::Stop stop;
    if (!signals || !signals->MatchesSource(source) ||
        protocol_generation != protocol_generation_.load() || chat_protocol_owned_.load() ||
        !signals->intake.TryCapture(stop.capture))
        return;
    const auto& response = stop.capture.response;
    if (response.source.source_id != source.source_id ||
        response.source.connection_epoch != source.connection_epoch ||
        response.protocol_generation != protocol_generation ||
        response.connect_generation != connect_generation_.load() ||
        response.response_generation != speaking_generation_.load())
        return;
    stop.received_us = received_us;
    const auto* reason = cJSON_GetObjectItem(root, "reason");
    stop.interrupt = cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0;
    const auto* resume = cJSON_GetObjectItem(root, "continue_listening");
    const auto* mode = cJSON_GetObjectItem(root, "listen_mode");
    stop.continue_listening = cJSON_IsTrue(resume);
    stop.realtime = cJSON_IsString(mode) && strcmp(mode->valuestring, "realtime") == 0;
    stop.explicit_manual_stop =
        cJSON_IsFalse(resume) && cJSON_IsString(mode) && strcmp(mode->valuestring, "manual") == 0;
    const auto* id = cJSON_GetObjectItem(root, "drainId");
    // A no-audio server keepalive refreshes an already active listener. It has
    // no drain identity and must not invalidate the previous completed reply.
    if (!id && !reason && stop.continue_listening && stop.realtime &&
        GetDeviceState() == kDeviceStateListening && microphone_uplink_authorized_.load() &&
        !signals->start_audio.reset_token &&
        audio_service_.IsCurrentChatPlaybackReset(response.reset_token))
        return;
    // Seal only receiver-owned audio admission. Keep the application-published
    // intake identity intact so duplicate/conflicting STOPs retain their clock.
    signals->start_audio = {};
    if (cJSON_IsString(id)) {
        const size_t size = strnlen(id->valuestring, stop.drain_id.size());
        stop.valid = size > 5 && size <= 128 && strncmp(id->valuestring, "chat:", 5) == 0;
        if (stop.valid) {
            memcpy(stop.drain_id.data(), id->valuestring, size);
            stop.drain_id_size = size;
        }
    }
    stop.reset_captured = audio_service_.IsCurrentChatPlaybackReset(response.reset_token) &&
                          audio_service_.TryGetPlaybackResetEpoch(stop.reset_epoch) &&
                          audio_service_.IsCurrentChatPlaybackReset(response.reset_token);
    signals->intake.PublishStop(stop);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}
