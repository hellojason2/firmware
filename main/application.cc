#include "application_internal.h"

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (chat_protocol_signals_ && chat_protocol_signals_->SourceSelected() &&
        !lesson_runtime_active_.load()) {
        if (lesson_asset_sync_quiet_.load())
            return;
        if (GetDeviceState() == kDeviceStateSpeaking)
            HandleChatAbort(kAbortReasonNone, listening_mode_ != kListeningModeManualStop);
        else if (GetDeviceState() == kDeviceStateListening)
            CloseAudioChannelByIntent();
        else
            HandleChatWake(wake_word);
        return;
    }
    if (lesson_asset_sync_quiet_.load()) {
        ESP_LOGI(TAG, "lesson asset sync quiet ignored direct wake");
        return;
    }
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson direct wake ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
            return;
        }
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                CloseAudioChannelByIntent();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson sleep mode blocked");
        return false;
    }

    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    return true;
}

void Application::LogPeriodicMetrics() {
    if (clock_ticks_ % 10 == 0) {
        // Audio realtime metrics snapshot: queue depths + drop/stale
        // counters. Cheap, on the app task (NOT the audio hot path), so
        // it never jitters capture/playback. Lets us measure backpressure
        // (Patch 3.1/3.2) and barge-in stale-frame drops (Patch 3.3).
        uint32_t decode_q = 0, send_q = 0, playback_q = 0;
        audio_service_.GetQueueDepths(decode_q, send_q, playback_q);
        auto audio_stats = audio_service_.GetDebugStatistics();
        auto wake_progress = audio_service_.GetWakeWordProgress();
        ESP_LOGI(TAG, "audio_metrics decode_q=%lu send_q=%lu playback_q=%lu input_count=%lu wake_running=%d wake_feed=%lu wake_fetch=%lu wake_gen=%lu vp_running=%d decode_drop=%lu encode_drop=%lu stale_frames=%lu interrupts=%lu reconnects=%lu "
                 "wake_chunks=%lu wake_rms_min=%lu wake_rms_max=%lu wake_peak_max=%lu wake_above_floor=%lu wake_above_total=%lu wake_last_above_us=%lld "
                 "wn_none=%lu wn_transition=%lu wn_detected=%lu wn_other=%lu wn_model=%ld wn_bad_model=%lu",
                 (unsigned long)decode_q, (unsigned long)send_q,
                 (unsigned long)playback_q, (unsigned long)audio_stats.input_count,
                 audio_service_.IsWakeWordRunning() ? 1 : 0,
                 (unsigned long)wake_progress.feed_count,
                 (unsigned long)wake_progress.fetch_count,
                 (unsigned long)wake_progress.run_generation,
                 audio_service_.IsAudioProcessorRunning() ? 1 : 0,
                 (unsigned long)audio_stats.decode_drop_count,
                 (unsigned long)audio_stats.encode_drop_count,
                 (unsigned long)audio_stats.stale_frame_count,
                 (unsigned long)interrupt_count_.load(),
                 (unsigned long)reconnect_count_.load(),
                 (unsigned long)wake_progress.telemetry.chunk_count,
                 (unsigned long)wake_progress.telemetry.rms_min,
                 (unsigned long)wake_progress.telemetry.rms_max,
                 (unsigned long)wake_progress.telemetry.peak_max,
                 (unsigned long)wake_progress.telemetry.above_floor_count,
                 (unsigned long)wake_progress.telemetry.above_floor_total,
                 (long long)wake_progress.telemetry.last_above_floor_us,
                 (unsigned long)wake_progress.telemetry.state_none,
                 (unsigned long)wake_progress.telemetry.state_transition,
                 (unsigned long)wake_progress.telemetry.state_detected,
                 (unsigned long)wake_progress.telemetry.state_other,
                 (long)wake_progress.telemetry.last_valid_model_index,
                 (unsigned long)wake_progress.telemetry.invalid_model_index_count);
        // Stack high-water snapshots are sampled off the audio hot path.
        auto stack_hwm = audio_service_.GetTaskStackHighWaterMarks();
        ESP_LOGI(TAG, "sys_metrics stack_main_min=%u stack_audio_input_min=%ld "
                 "stack_audio_output_min=%ld stack_opus_codec_min=%ld "
                 "stack_afe_detection_min=%ld psram_free_b=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                 (long)stack_hwm.audio_input, (long)stack_hwm.audio_output,
                 (long)stack_hwm.opus_codec, (long)stack_hwm.afe_detection,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

