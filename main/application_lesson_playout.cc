#include "application_internal.h"


void Application::BeginLessonNetworkRenderQuiet() {
    int depth = lesson_network_render_quiet_.fetch_add(1) + 1;
    if (depth == 1) {
        ESP_LOGI(TAG, "lesson_network_render_quiet begin");
    }
}

void Application::EndLessonNetworkRenderQuiet() {
    int previous = lesson_network_render_quiet_.fetch_sub(1);
    if (previous <= 1) {
        lesson_network_render_quiet_.store(0);
        ESP_LOGI(TAG, "lesson_network_render_quiet end");
    }
}

bool Application::IsLessonNetworkRenderQuiet() const {
    return lesson_network_render_quiet_.load() > 0;
}

bool Application::HasLessonAssetSyncWakeOpportunity() {
    // State changes may originate in board callbacks; only App owns the deadline.
    if (lesson_asset_sync_wake_invalidated_.exchange(false))
        lesson_asset_sync_wake_deadline_us_ = 0;
    if (!IsDeviceClaimed()) {
        lesson_asset_sync_wake_pending_ = false;
        lesson_asset_sync_wake_deadline_us_ = 0;
        return true;
    }
    if (!lesson_asset_sync_wake_pending_)
        return true;
    // A deferred stop/restart may complete between clock observations.
    if (lesson_asset_sync_wake_revoked_ != chat_audio_desired_.revoked) {
        lesson_asset_sync_wake_revoked_ = chat_audio_desired_.revoked;
        lesson_asset_sync_wake_deadline_us_ = 0;
    }
    if (GetDeviceState() != kDeviceStateIdle || !audio_service_.IsWakeWordRunning()) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    const auto now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (!lesson_asset_sync_wake_deadline_us_)
        lesson_asset_sync_wake_deadline_us_ = now_us + 3000000ULL;
    if (lesson_asset_sync_wake_invalidated_.exchange(false)) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    return now_us >= lesson_asset_sync_wake_deadline_us_;
}

bool Application::BeginLessonAssetSyncQuiet() {
#if CONFIG_TBOT_VOICE_DEMO
    return false;
#endif
    // Busy retries must not stop the settling timer or revoke the wake window.
    if (!HasLessonAssetSyncWakeOpportunity())
        return false;
    const DeviceState state = GetDeviceState();
    const bool passive_listening = state == kDeviceStateListening &&
                                   !chat_cleanup_enabled_.load() && passive_ws_intent_.load() &&
                                   !online_intent_.load() &&
                                   !microphone_uplink_authorized_.load() && !IsVoiceDetected();
    if ((state != kDeviceStateIdle && !passive_listening) || lesson_runtime_active_.load() ||
        connect_in_flight_.load() || reset_pending_.load())
        return false;
    if (lesson_asset_sync_wake_pending_ && lesson_asset_sync_wake_invalidated_.exchange(false)) {
        lesson_asset_sync_wake_deadline_us_ = 0;
        return false;
    }
    bool expected = false;
    if (!lesson_asset_sync_quiet_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "lesson asset sync quiet already active");
        return false;
    }

    lesson_asset_sync_wake_pending_ = false;
    lesson_asset_sync_wake_deadline_us_ = 0;
    if (lesson_asset_sync_wake_rearm_timer_ != nullptr) {
        esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
    }

    if (passive_listening) {
        lesson_idle_repaint_suppressed_.store(true);
        if (protocol_) {
            protocol_->SendStopListening();
        }
        listening_started_ms_.store(0);
        last_listening_activity_ms_.store(0);
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        SetDeviceState(kDeviceStateIdle);
    }
    tts_audio_accepting_.store(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(false);
    audio_service_.ResetDecoder();
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {
    }
    ESP_LOGI(TAG, "lesson asset sync quiet begin");
    return true;
}

void Application::EndLessonAssetSyncQuiet() {
    if (!lesson_asset_sync_quiet_.load()) {
        return;
    }

    tts_audio_accepting_.store(false);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.ResetDecoder();
    while (audio_service_.PopPacketFromSendQueue() != nullptr) {
    }
    if (!lesson_asset_sync_quiet_.exchange(false)) {
        return;
    }
    lesson_asset_sync_wake_pending_ = IsDeviceClaimed();
    lesson_asset_sync_wake_deadline_us_ = 0;

    // Passive-liveness polling was suspended during the sync (see the
    // IsLessonAssetSyncQuiet() gate in the CLOCK_TICK handler). Reset the ping
    // timer so it does not immediately fire a stale pong-timeout now that the WS
    // receive path is free again.
    if (protocol_ != nullptr) {
        protocol_->ResetPassiveLiveness();
    }

    if (IsDeviceClaimed() && audio_service_.IsRunning() && GetDeviceState() == kDeviceStateIdle &&
        !lesson_runtime_active_.load() && !connect_in_flight_.load() && !reset_pending_.load()) {
        ScheduleLessonAssetSyncWakeRearm();
    }
    ESP_LOGI(TAG, "lesson asset sync quiet end");
}

void Application::ScheduleLessonAssetSyncWakeRearm() {
    ScheduleLessonAssetSyncWakeRearm(1500ULL * 1000ULL);
}

void Application::ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us) {
    if (lesson_asset_sync_wake_rearm_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* self = static_cast<Application*>(arg);
            self->Schedule([self]() {
                self->RearmClaimedIdleWakeWord();
                self->HasLessonAssetSyncWakeOpportunity();
            });
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "asset_wake";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &lesson_asset_sync_wake_rearm_timer_) != ESP_OK) {
            lesson_asset_sync_wake_rearm_timer_ = nullptr;
            ESP_LOGE(TAG, "Failed to create lesson asset wake rearm timer");
            return;
        }
    }

    esp_timer_stop(lesson_asset_sync_wake_rearm_timer_);
    esp_timer_start_once(lesson_asset_sync_wake_rearm_timer_, delay_us);
}

bool Application::HandleLessonPlayoutTts(const cJSON* root, ChatRequestContext context) {
    std::lock_guard<std::mutex> lock(lesson_playout_mutex_);
    const auto* id = cJSON_GetObjectItem(root, "playoutId");
    const auto* state = cJSON_GetObjectItem(root, "state");
    const auto* reason = cJSON_GetObjectItem(root, "reason");
    const bool interrupt = cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0 &&
                           cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0;
    if (!id && lesson_playout_id_.empty())
        return false;
    if (!id && !interrupt)
        return true;
    const std::string playout_id = id && cJSON_IsString(id) ? id->valuestring
                                   : !id                    ? lesson_playout_id_
                                                            : std::string{};
    if (!IsChatLessonRequestCurrent(context) || !lesson_runtime_active_.load() ||
        lesson_terminal_audio_generation_.load() != 0 || !LessonAudioPlayout::ValidId(playout_id))
        return true;
    if (!cJSON_IsString(state))
        return true;
    if (strcmp(state->valuestring, "start") == 0) {
        lesson_audio_playout_.SetScope(protocol_generation_.load(),
                                       lesson_transport_epoch_gate_.PublishedEpoch());
        if (lesson_audio_playout_.Seen(playout_id))
            return true;
        if (auto token = std::atomic_load(&lesson_playout_authorization_))
            token->store(false);
        tts_audio_accepting_.store(false);
        lesson_audio_playout_.Cancel();
        lesson_playout_id_.clear();
        lesson_playout_pending_.store(false);
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateSpeaking && listening_mode_ != kListeningModeRealtime)
            SetDeviceState(kDeviceStateIdle);
        audio_service_.SetPlaybackGeneration(++speaking_generation_);
        audio_service_.ResetDecoder();
        uint64_t reset_epoch = 0;
        if (!audio_service_.TryGetPlaybackResetEpoch(reset_epoch) ||
            !lesson_audio_playout_.Begin(speaking_generation_.load(), playout_id, reset_epoch))
            return true;
        lesson_playout_context_ = context;
        lesson_playout_protocol_generation_ = protocol_generation_.load();
        lesson_playout_epoch_ = lesson_transport_epoch_gate_.PublishedEpoch();
        lesson_playout_generation_ = speaking_generation_.load();
        lesson_playout_id_ = playout_id;
        lesson_playout_pending_.store(true);
        lesson_playout_drain_id_.clear();
        lesson_playout_stop_ms_ = 0;
        lesson_playout_drained_at_ms_ = 0;
        lesson_playout_start_sent_ = false;
        lesson_playout_stop_sent_ = false;
        std::atomic_store(&lesson_playout_authorization_,
                          std::make_shared<std::atomic<bool>>(true));
        aborted_ = false;
        if (GetDeviceState() == kDeviceStateListening &&
            listening_mode_ != kListeningModeRealtime) {
            audio_service_.EnableVoiceProcessing(false);
            listening_started_ms_.store(0);
            last_listening_activity_ms_.store(0);
        }
        last_speaking_activity_ms_.store(esp_timer_get_time() / 1000);
        // START admits bytes. The output callback below owns the speech cue.
        lesson_idle_repaint_suppressed_.store(true);
        if (GetDeviceState() == kDeviceStateSpeaking && listening_mode_ != kListeningModeRealtime)
            SetDeviceState(kDeviceStateIdle);
        tts_audio_accepting_.store(true);
    } else if (strcmp(state->valuestring, "stop") == 0 &&
               lesson_audio_playout_.Current(playout_id, speaking_generation_.load())) {
        if (cJSON_IsString(reason) && strcmp(reason->valuestring, "interrupt") == 0) {
            if (auto token = std::atomic_load(&lesson_playout_authorization_))
                token->store(false);
            lesson_audio_playout_.Cancel();
            lesson_playout_id_.clear();
            lesson_playout_pending_.store(false);
            tts_audio_accepting_.store(false);
            audio_service_.SetPlaybackGeneration(++speaking_generation_);
            audio_service_.ResetDecoder();
            speaking_arm_dispatch_.Cancel();
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateSpeaking)
                SetDeviceState(kDeviceStateIdle);
        } else if (lesson_playout_stop_ms_ == 0 && lesson_audio_playout_.Stop(playout_id)) {
            const auto* drain = cJSON_GetObjectItem(root, "drainId");
            if (cJSON_IsString(drain) && strlen(drain->valuestring) <= 64)
                lesson_playout_drain_id_ = drain->valuestring;
            lesson_playout_stop_ms_ = esp_timer_get_time() / 1000;
            tts_audio_accepting_.store(false);
        }
    }
    return true;
}

bool Application::QueueLessonPlayoutAck(const char* state, uint64_t at_ms, bool drain) {
    if (!lesson_playout_context_ || chat_connection_messages_.Size() >= 2 ||
        !IsChatLessonRequestCurrent(lesson_playout_context_))
        return false;
    const auto authorization = std::atomic_load(&lesson_playout_authorization_);
    if (!authorization || !authorization->load(std::memory_order_acquire))
        return false;
    const auto text = drain ? Protocol::EncodeTtsDrainAck(lesson_playout_drain_id_,
                                                          lesson_playout_context_->session_id)
                            : Protocol::EncodeLessonPlayoutAck(lesson_playout_id_, state, at_ms,
                                                               lesson_playout_context_->session_id);
    return !text.empty() &&
           RequestChatConnectionText(text, lesson_playout_context_, 0, authorization) != 0;
}

void Application::PollLessonAudioPlayout() {
    std::unique_lock<std::mutex> lock(lesson_playout_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    if (lesson_playout_id_.empty())
        return;
    const auto now_ms = esp_timer_get_time() / 1000;
    const auto owned = [this]() {
        const auto authorization = std::atomic_load(&lesson_playout_authorization_);
        return authorization && authorization->load(std::memory_order_acquire) && protocol_ &&
               lesson_runtime_active_.load() &&
               IsChatLessonRequestCurrent(lesson_playout_context_) &&
               lesson_playout_protocol_generation_ == protocol_generation_.load() &&
               lesson_playout_epoch_ == lesson_transport_epoch_gate_.PublishedEpoch() &&
               lesson_audio_playout_.Current(lesson_playout_id_, speaking_generation_.load());
    };
    const bool timed_out = lesson_playout_stop_ms_
                               ? now_ms - lesson_playout_stop_ms_ >= kTtsStopPlaybackDrainTimeoutMs
                               : now_ms - last_speaking_activity_ms_.load() >= kSpeakingTimeoutMs;
    auto retire = [this](bool flush) {
        if (flush) {
            const auto authorization = std::atomic_load(&lesson_playout_authorization_);
            if (authorization)
                authorization->store(false, std::memory_order_release);
        }
        lesson_audio_playout_.Cancel();
        lesson_playout_id_.clear();
        lesson_playout_pending_.store(false);
        lesson_playout_context_.reset();
        if (lesson_playout_generation_ == speaking_generation_.load()) {
            tts_audio_accepting_.store(false);
            if (flush) {
                audio_service_.SetPlaybackGeneration(++speaking_generation_);
                audio_service_.ResetDecoder();
            }
            last_speaking_activity_ms_.store(0);
            lesson_idle_repaint_suppressed_.store(true);
            if (GetDeviceState() == kDeviceStateSpeaking)
                SetDeviceState(kDeviceStateIdle);
        }
    };
    if (!owned()) {
        retire(true);
        return;
    }
    if (timed_out || lesson_audio_playout_.Failed()) {
        const auto context = lesson_playout_context_;
        retire(true);
        if (context)
            FailChatRequest(context);
        return;
    }
    if (!lesson_playout_start_sent_ && chat_connection_messages_.Size() >= 2)
        return;
    if (const auto start = lesson_audio_playout_.TakeStart()) {
        if (start->generation != lesson_playout_generation_ ||
            !QueueLessonPlayoutAck("start", start->at_ms)) {
            retire(true);
            return;
        }
        if (!owned()) {
            retire(true);
            return;
        }
        lesson_playout_start_sent_ = true;
        SetDeviceState(kDeviceStateSpeaking);
        ArmSpeakingTimeout();
    }
    if (!lesson_playout_stop_ms_ || !lesson_playout_start_sent_)
        return;
    PlaybackDrainSnapshot snapshot;
    if (!audio_service_.TryGetPlaybackDrainSnapshot(snapshot) ||
        !lesson_audio_playout_.Drained(snapshot))
        return;
    // Revalidate after the audio lock boundary and before sending either receipt.
    if (!protocol_ || !lesson_runtime_active_.load() ||
        lesson_playout_protocol_generation_ != protocol_generation_.load() ||
        !IsChatLessonRequestCurrent(lesson_playout_context_) ||
        lesson_playout_epoch_ != lesson_transport_epoch_gate_.PublishedEpoch() ||
        !lesson_audio_playout_.Current(lesson_playout_id_, speaking_generation_.load())) {
        retire(true);
        return;
    }
    if (!lesson_playout_drained_at_ms_)
        lesson_playout_drained_at_ms_ = static_cast<uint64_t>(now_ms);
    lesson_idle_repaint_suppressed_.store(true);
    if (GetDeviceState() == kDeviceStateSpeaking) {
        const bool lesson_interactive_turn = lesson_interactive_listen_pending_.load() ||
                                             lesson_interactive_listening_active_.load();
        SetDeviceState(lesson_interactive_turn && listening_mode_ != kListeningModeAutoStop
                           ? kDeviceStateListening
                           : kDeviceStateIdle);
    }
    if (!lesson_playout_stop_sent_) {
        if (chat_connection_messages_.Size() >= 2)
            return;
        if (!QueueLessonPlayoutAck("stop", lesson_playout_drained_at_ms_)) {
            retire(true);
            return;
        }
        lesson_playout_stop_sent_ = true;
    }
    if (!owned()) {
        retire(true);
        return;
    }
    if (!lesson_playout_drain_id_.empty()) {
        if (chat_connection_messages_.Size() >= 2)
            return;
        if (!QueueLessonPlayoutAck("stop", lesson_playout_drained_at_ms_, true)) {
            retire(true);
            return;
        }
    }
    retire(false);
}
