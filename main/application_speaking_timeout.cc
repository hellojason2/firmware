#include "application_internal.h"

void Application::ArmSpeakingTimeout() {
    auto current_generation = speaking_generation_.load();
    speaking_timeout_generation_.store(current_generation, std::memory_order_relaxed);
    if (speaking_timeout_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    auto generation =
                        app->speaking_timeout_generation_.load(std::memory_order_relaxed);
                    app->Schedule([app, generation]() { app->HandleSpeakingTimeout(generation); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "speaking_timer",
            .skip_unhandled_events = true};
        auto err = esp_timer_create(&timer_args, &speaking_timeout_timer_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speaking_timeout_timer_create_failed err=%s generation=%lu",
                     esp_err_to_name(err), (unsigned long)current_generation);
            return;
        }
    }
    esp_timer_stop(speaking_timeout_timer_);
    auto err = esp_timer_start_once(speaking_timeout_timer_, kSpeakingTimeoutMs * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "speaking_timeout_timer_start_failed err=%s generation=%lu",
                 esp_err_to_name(err), (unsigned long)current_generation);
    }
}

void Application::HandleSpeakingTimeout(uint32_t generation) {
    if (generation != speaking_generation_.load() || GetDeviceState() != kDeviceStateSpeaking) {
        return;
    }

    auto now_ms = esp_timer_get_time() / 1000;
    auto last_activity_ms = last_speaking_activity_ms_.load();
    if (last_activity_ms > 0 && now_ms - last_activity_ms < kSpeakingTimeoutMs) {
        ArmSpeakingTimeout();
        return;
    }
    if (HandleChatAbort(kAbortReasonNone, listening_mode_ == kListeningModeRealtime)) {
        if (listening_mode_ != kListeningModeRealtime &&
            !RequestChatCue(Lang::Sounds::OGG_EXCLAMATION))
            ESP_LOGW(TAG, "chat_timeout_cue_busy");
        return;
    }

    ESP_LOGW(TAG, "speaking_timeout generation=%lu idle_ms=%ld", (unsigned long)generation,
             static_cast<long>(last_activity_ms > 0 ? now_ms - last_activity_ms : -1));
    speaking_arm_dispatch_.Cancel();
    tts_audio_accepting_.store(false);
    ++speaking_generation_;
    audio_service_.SetPlaybackGeneration(speaking_generation_.load());
    last_speaking_activity_ms_.store(0);
    aborted_ = true;
    audio_service_.ResetDecoder();
    if (protocol_) {
        protocol_->SendAbortSpeaking(kAbortReasonNone);
    }
    const bool lesson_answer_turn =
        lesson_runtime_active_.load() && lesson_interactive_listen_pending_.load();
    if (!lesson_answer_turn) {
        CancelLessonInteractiveListening();
    }
    auto show_timeout_cue = [this]() {
        auto display = Board::GetInstance().GetDisplay();
        if (lesson_runtime_active_.load()) {
            display->SetStatus(Lang::Strings::PLEASE_WAIT);
        } else {
            display->SetStatus(Lang::Strings::SERVER_TIMEOUT);
            display->SetEmotion("thinking");
            audio_service_.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        }
    };
    if (listening_mode_ == kListeningModeManualStop) {
        if (lesson_answer_turn) {
            SetDeviceState(kDeviceStateListening);
            ESP_LOGI(TAG, "lesson prompt timeout -> listening");
            return;
        }
        SetDeviceState(kDeviceStateIdle);
        show_timeout_cue();
    } else if (listening_mode_ == kListeningModeAutoStop) {
        SetDeviceState(kDeviceStateIdle);
        show_timeout_cue();
    } else {
        SetDeviceState(kDeviceStateListening);
        const uint64_t resumed_ms = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "mic_loop_resumed ts=%lu%03lu reason=speaking_timeout",
                 static_cast<unsigned long>(resumed_ms / 1000),
                 static_cast<unsigned long>(resumed_ms % 1000));
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    if (HandleChatAbort(reason, listening_mode_ != kListeningModeManualStop))
        return;
    speaking_arm_dispatch_.Cancel();
    ESP_LOGI(TAG, "Abort speaking");
    interrupt_count_.fetch_add(1, std::memory_order_relaxed);
    aborted_ = true;
    tts_audio_accepting_.store(false);
    ++speaking_generation_;
    audio_service_.SetPlaybackGeneration(speaking_generation_.load());
    last_speaking_activity_ms_.store(0);
    audio_service_.ResetDecoder();
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        SetDeviceState(listening_mode_ == kListeningModeManualStop ? kDeviceStateIdle
                                                                   : kDeviceStateListening);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !lesson_runtime_active_.load()) {
        BeginChatListen(mode, ChatListenOrigin::User);
        return;
    }
    passive_ws_intent_.store(false);
    online_intent_.store(true);
    const bool already_listening = GetDeviceState() == kDeviceStateListening;
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
    if (already_listening) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    }
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}
