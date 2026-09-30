#include "application_internal.h"

void Application::SendMcpMessage(const std::string& payload, ChatRequestContext context) {
    const auto received_us = static_cast<uint64_t>(esp_timer_get_time());
    try {
        Schedule([this, payload = std::move(payload), context, received_us]() {
            if (!IsChatRequestCurrent(context))
                return;
            if (context) {
                try {
                    RequestChatConnectionText(context->EncodeMcpReply(payload), context,
                                              received_us);
                } catch (...) {
                    FailChatRequest(context);
                }
                return;
            }
            if (protocol_) {
                protocol_->SendMcpMessage(payload);
            }
        });
    } catch (...) {
        if (!context)
            throw;
        FailChatRequest(context);
    }
}

void Application::SetAecMode(AecMode mode) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson aec mode ignored");
        return;
    }
    Schedule([this, mode]() {
        if (lesson_runtime_active_.load()) {
            ESP_LOGI(TAG, "scheduled lesson aec mode ignored");
            return;
        }
        aec_mode_ = mode;
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            CloseAudioChannelByIntent();
        }
    });
}

void Application::CloseAudioChannelByIntent() {
    if (xTaskGetCurrentTaskHandle() != application_task_) {
        Schedule([this]() { CloseAudioChannelByIntent(); });
        return;
    }
    CancelChatRecovery();
    RetireChatOutbound();
    deferred_wake_word_.clear();
    passive_ws_intent_.store(false);
    reconnect_passive_.store(false);
    online_intent_.store(false);
    microphone_uplink_authorized_.store(false);
    reconnect_attempt_ = 0;
    passive_reconnect_attempt_ = 0;
    backend_recovery_window_.Reset();
    connect_attempt_active_.store(false);
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
    }
    if (chat_cleanup_enabled_) {
        ++connect_generation_;
        connect_close_deferral_.Request(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        PollChatProtocolCleanup();
        return;
    }
    if (!connect_close_deferral_.Request(protocol_work_lifetime_.Busy())) {
        ++connect_generation_;
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
        ESP_LOGI(TAG, "channel_close_deferred_until_connect_worker_exit");
        return;
    }
    if (protocol_) {
        protocol_->CloseAudioChannel();
    }
}

bool Application::CompletePendingProtocolWork() {
    if (chat_cleanup_enabled_)
        return PollChatProtocolCleanup();
    if (protocol_work_lifetime_.Pending())
        RetireChatOutbound();
    using Action = ProtocolWorkLifetime::Action;
    const auto action = protocol_work_lifetime_.TakeReady();
    if (action == Action::kNone)
        return protocol_work_lifetime_.Pending();
    connect_in_flight_.store(false);
    CancelConnectWatchdog();
    const bool intentional_close = connect_close_deferral_.TakeAfterWorker();
    reset_pending_.store(false);
    protocol_reinit_pending_.store(false);
    if (action == Action::kReboot) {
        reboot_pending_.store(false);
        protocol_activation_pending_ = ProtocolActivation::kNone;
        claim_protocol_completion_pending_ = false;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        CompleteReboot();
        return true;
    }
    if (action == Action::kReinitialize || action == Action::kReset) {
        protocol_start_pending_generation_ = 0;
        deferred_close_generation_ = 0;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        DoResetProtocol();
        if (action == Action::kReinitialize) {
            InitializeProtocol();
        } else {
            protocol_activation_pending_ = ProtocolActivation::kNone;
            claim_protocol_completion_pending_ = false;
        }
        return true;
    }
    if (protocol_) {
        if (intentional_close)
            protocol_->CloseAudioChannel();
        else if (deferred_close_generation_ == protocol_generation_.load()) {
            protocol_->CompleteDeferredClose(deferred_close_epoch_);
        }
    }
    deferred_close_generation_ = 0;
    if (protocol_start_pending_generation_ == 0)
        CompleteProtocolActivation();
    return true;
}

void Application::DoResetProtocol() {
    RetireChatOutbound();
    if (chat_cleanup_enabled_) {
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        PollChatProtocolCleanup();
        return;
    }
    if (protocol_work_lifetime_.Busy()) {
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        return;
    }
    RequestLessonStorageAbandonment();
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        CloseAudioChannelByIntent();
    }
    CancelLessonRobotEntranceOnDisplay();
    protocol_.reset();
    protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
}

void Application::ResetProtocol() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "ResetProtocol ignored during lesson");
        return;
    }
    Schedule([this]() {
        ++connect_generation_;
        reset_pending_.store(true);
        protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kReset);
        CompletePendingProtocolWork();
    });
}

void Application::PlaySound(const std::string_view& sound) {
    if (chat_cleanup_enabled_.load() && !IsLessonVoiceRoute()) {
        if (!RequestChatCue(sound))
            ESP_LOGW(TAG, "chat_cue_busy_or_unavailable");
        return;
    }
    audio_service_.PlaySound(sound);
}
