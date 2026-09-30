#include "application_internal.h"

uint32_t Application::BeginLessonInteractiveListeningRequest() {
    return lesson_interactive_listen_generation_.fetch_add(1) + 1;
}

void Application::PrepareLessonInteractiveListening() {
    PrepareLessonInteractiveListening(lesson_interactive_listen_generation_.load());
}

void Application::PrepareLessonInteractiveListening(uint32_t generation) {
    if (generation != lesson_interactive_listen_generation_.load()) {
        ESP_LOGI(TAG, "stale lesson interactive listen prepare ignored");
        return;
    }
    if (!lesson_runtime_active_.load()) {
        lesson_interactive_listen_pending_.store(false);
        return;
    }
    lesson_interactive_listen_pending_.store(true);
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->ClearChatMessages();
        display->SetStatus("Sắp đến lượt con...");
    }
    StartListening();
}

void Application::CancelLessonInteractiveListening() {
    lesson_interactive_listen_generation_.fetch_add(1);
    const bool had_pending = lesson_interactive_listen_pending_.exchange(false);
    const bool had_active = lesson_interactive_listening_active_.exchange(false);
    const bool had_lesson_listen = had_pending || had_active;
    const bool lesson_runtime_cancel = lesson_runtime_active_.load();
    if (!lesson_runtime_cancel && !had_lesson_listen) {
        return;
    }
    const DeviceState state = GetDeviceState();
    if (state == kDeviceStateConnecting) {
        lesson_idle_repaint_suppressed_.store(true);
        ++connect_generation_;
        connect_attempt_active_.store(false);
        passive_ws_intent_.store(false);
        online_intent_.store(false);
        CancelConnectWatchdog();
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    if (state != kDeviceStateListening) {
        return;
    }
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

void Application::SetLessonRuntimeActive(bool active) {
    if (active)
        speaking_arm_dispatch_.Cancel();
    const bool was_active = lesson_runtime_active_.load();
    if (was_active != active) {
        if (active) {
            lesson_runtime_generation_.store(
                NextLessonRuntimeGeneration(lesson_runtime_generation_.load(), was_active, active));
            lesson_runtime_active_.store(true);
        } else {
            lesson_runtime_active_.store(false);
            lesson_runtime_generation_.store(
                NextLessonRuntimeGeneration(lesson_runtime_generation_.load(), was_active, active));
        }
    }
    if (active) {
        lesson_terminal_audio_generation_.store(0);
    }
    if (!active) {
        lesson_interactive_listen_generation_.fetch_add(1);
        lesson_interactive_listen_pending_.store(false);
        lesson_interactive_listening_active_.store(false);
        const int status_code = deferred_heartbeat_auth_failure_status_.exchange(0);
        if (status_code != 0) {
            Schedule([this, status_code]() { HandleHeartbeatAuthFailure(status_code); });
        }
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        static_cast<WifiBoard&>(Board::GetInstance()).ResumePendingWifiConfigMode();
#endif
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
}

void Application::BeginLessonTerminalAudioQuiet() {
    std::lock_guard<std::mutex> lock(lesson_playout_mutex_);
    if (auto token = std::atomic_load(&lesson_playout_authorization_))
        token->store(false);
    lesson_audio_playout_.Cancel();
    lesson_terminal_audio_generation_.store(
        static_cast<std::uint64_t>(speaking_generation_.load()) + 1);
    tts_audio_accepting_.store(false);
    speaking_arm_dispatch_.Cancel();
    // Terminal errors may have no subsequent TTS STOP. Fence in-flight decode
    // before clearing queued output, without rearming a child listening turn.
    audio_service_.SetPlaybackGeneration(++speaking_generation_);
    audio_service_.ResetDecoder();
    last_speaking_activity_ms_.store(0);
    CancelLessonInteractiveListening();
    if (GetDeviceState() == kDeviceStateSpeaking) {
        lesson_idle_repaint_suppressed_.store(true);
        SetDeviceState(kDeviceStateIdle);
    }
}

bool Application::IsLessonRuntimeActive() const { return lesson_runtime_active_.load(); }
