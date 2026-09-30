#include "application_internal.h"

bool Application::InitializeChatOutboundWorker() {
    if (chat_outbound_task_)
        return true;
    chat_outbound_task_ = xTaskCreateStatic(
        &Application::ChatOutboundTask, "chat_outbound", kChatOutboundWorkerStackDepth, this,
        tskIDLE_PRIORITY + 3, chat_outbound_task_stack, &chat_outbound_task_buffer);
    return chat_outbound_task_ != nullptr;
}

bool Application::InitializeChatAudioCleanupWorker() {
    if (chat_audio_task_)
        return true;
    chat_audio_task_ = xTaskCreateStatic(
        &Application::ChatAudioCleanupTask, "chat_audio_cleanup", kChatAudioCleanupWorkerStackDepth,
        this, tskIDLE_PRIORITY + 3, chat_audio_cleanup_task_stack, &chat_audio_cleanup_task_buffer);
    return chat_audio_task_ != nullptr;
}

bool Application::InitializeChatSourceRoute(bool is_websocket_protocol) {
    if (!is_websocket_protocol)
        return true;
    chat_cleanup_enabled_.store(false);
    const auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || !codec->SupportsChatOutputDrain())
        return true;
    try {
        if (protocol_ && chat_protocol_signals_ && chat_outbound_task_ && open_channel_queue &&
            open_channel_task && InitializeChatAudioCleanupWorker()) {
            auto callbacks =
                MakeChatSourceCallbacks(protocol_generation_.load(), chat_protocol_signals_);
            if (callbacks.audio && callbacks.json && callbacks.closed && callbacks.opened &&
                callbacks.adopted && callbacks.error) {
                protocol_->SetSourceCallbacks(std::move(callbacks));
                chat_protocol_signals_->SelectDeferred();
                chat_cleanup_enabled_.store(true);
                static_cast<WebsocketProtocol*>(protocol_.get())
                    ->SetConversationAudioDrainAck(true);
                chat_protocol_fault_ = chat_protocol_infrastructure_fault_ = false;
                return true;
            }
        }
    } catch (...) {
    }
    chat_protocol_fault_ = chat_protocol_infrastructure_fault_ = true;
    return false;
}

uint32_t Application::RequestChatPlaybackCleanup(uint32_t playback_generation) {
    if (chat_reboot_audio_requested_)
        return 0;
    chat_audio_reset_serial_ = audio_service_.RequestChatPlaybackReset();
    if (chat_audio_reset_serial_ == UINT32_MAX)
        chat_audio_exhausted_ = true;
    audio_service_.SetPlaybackGeneration(playback_generation);
    chat_playback_desired_ = chat_audio_reset_serial_;
    chat_playback_fault_ = false;
    // Preserve an outstanding capture preparation, but let it reset the latest
    // stream instead of endlessly retrying a superseded decoder token.
    if (chat_audio_desired_.revoked != chat_audio_completed_revoked_)
        chat_audio_desired_.reset_serial = chat_audio_reset_serial_;
    PollChatAudioCleanup();
    return chat_audio_reset_serial_;
}

bool Application::RequestChatCue(std::string_view sound) {
    std::unique_lock<std::mutex> lock(chat_cue_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || chat_cue_pending_ || chat_cue_serial_ == UINT32_MAX || sound.empty() ||
        sound.size() > 65536)
        return false;
    try {
        chat_cue_owner_ = std::make_shared<const std::string>(sound);
    } catch (...) {
        return false;
    }
    ++chat_cue_serial_;
    chat_cue_sound_ = *chat_cue_owner_;
    chat_cue_generation_ = speaking_generation_.load();
    chat_cue_reset_ = audio_service_.ChatPlaybackResetToken();
    chat_cue_deadline_us_ = static_cast<uint64_t>(esp_timer_get_time()) + 10000000ULL;
    chat_cue_pending_ = true;
    chat_cue_retry_ = false;
    lock.unlock();
    if (xTaskGetCurrentTaskHandle() == application_task_)
        PollChatAudioCleanup();
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
    return true;
}

uint32_t Application::RequestChatAudioCleanup(uint32_t playback_generation, bool reset,
                                              bool processing, bool wake, bool chat_scope,
                                              bool stop_service, ChatWakePolicy wake_policy) {
    if (chat_reboot_audio_requested_ && !stop_service)
        return chat_audio_desired_.revoked;
    const uint32_t revoked = audio_service_.RevokeChatUplink();
    chat_audio_prepared_ = 0;
    if (reset) {
        chat_playback_desired_ = 0;
        chat_audio_reset_serial_ = audio_service_.RequestChatPlaybackReset();
        if (chat_audio_reset_serial_ == UINT32_MAX)
            chat_audio_exhausted_ = true;
    }
    audio_service_.SetPlaybackGeneration(playback_generation);
    chat_audio_desired_ = {};
    chat_audio_desired_.revoked = revoked;
    chat_audio_desired_.reset_serial = chat_audio_reset_serial_;
    chat_audio_desired_.processing = processing;
    chat_audio_desired_.wake = wake;
    chat_audio_desired_.chat_scope = chat_scope;
    chat_audio_desired_.stop_service = stop_service;
    chat_audio_desired_.wake_policy = wake_policy;
    PollChatAudioCleanup();
    return revoked;
}

void Application::PollChatAudioCleanup() {
    std::unique_lock<std::mutex> cue_lock(chat_cue_mutex_, std::try_to_lock);
    if (!cue_lock.owns_lock())
        return;
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.cue) {
            if (chat_audio_work_.cue_serial == chat_cue_serial_) {
                chat_cue_result_ = chat_audio_work_.cue_result;
                chat_cue_pending_ = chat_cue_result_ == 1;
                chat_cue_retry_ = chat_cue_pending_;
            }
            chat_audio_state_.store(0, std::memory_order_release);
        }
    }
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.read_wake) {
            if (chat_audio_work_.wake_serial == chat_wake_read_serial_) {
                chat_wake_read_result_ = std::move(chat_audio_work_.wake_text);
                chat_wake_read_pending_ = false;
            }
            chat_audio_state_.store(0, std::memory_order_release);
        }
    }
    if (chat_audio_state_.load(std::memory_order_acquire) == 2) {
        if (chat_audio_work_.reset_done)
            chat_audio_reset_completed_ = chat_audio_work_.reset_serial;
        if (chat_audio_work_.playback_only) {
            chat_playback_attempted_ = chat_audio_work_.reset_serial;
            if (chat_audio_work_.reset_serial == chat_playback_desired_)
                chat_playback_fault_ = !chat_audio_work_.reset_done;
        } else {
            chat_audio_completed_revoked_ = chat_audio_work_.revoked;
            if (chat_audio_work_.revoked == chat_audio_desired_.revoked) {
                // Preparation completed before this newer reset was collected.
                // Its readiness was never published, so retain the obligation
                // until the same revoked capture token has the latest reset.
                if (chat_audio_work_.reset_serial != chat_audio_desired_.reset_serial)
                    chat_audio_completed_revoked_ = 0;
                chat_audio_fault_ = !chat_audio_work_.prepared;
                if (chat_audio_work_.prepared && chat_audio_work_.processing &&
                    chat_audio_work_.reset_serial == chat_audio_reset_serial_) {
                    chat_audio_prepared_ = chat_audio_work_.revoked;
                }
                if (chat_audio_work_.prepared && chat_audio_work_.stop_service) {
                    chat_reboot_deadline_us_ =
                        static_cast<uint64_t>(esp_timer_get_time()) + 1000000ULL;
                }
            }
        }
        chat_audio_state_.store(0, std::memory_order_release);
    }
    if (chat_audio_state_.load(std::memory_order_acquire) != 0)
        return;
    if (chat_cue_pending_ && static_cast<uint64_t>(esp_timer_get_time()) >= chat_cue_deadline_us_) {
        chat_cue_pending_ = chat_cue_retry_ = false;
        chat_cue_result_ = 3;
    }
    const bool prepare =
        chat_audio_desired_.revoked && chat_audio_completed_revoked_ != chat_audio_desired_.revoked;
    const bool playback = chat_playback_desired_ &&
                          chat_playback_desired_ != chat_audio_reset_completed_ &&
                          chat_playback_desired_ != chat_playback_attempted_;
    const bool cue = chat_cue_pending_ && !chat_cue_retry_;
    if (!prepare && !playback && !chat_wake_read_pending_ && !cue)
        return;
    if (!chat_audio_task_ || chat_audio_exhausted_) {
        if (prepare)
            chat_audio_fault_ = true;
        if (playback)
            chat_playback_fault_ = true;
        return;
    }
    chat_audio_work_ = prepare ? chat_audio_desired_ : ChatAudioCleanup{};
    if (!prepare && playback) {
        chat_audio_work_.playback_only = true;
        chat_audio_work_.reset_serial = chat_playback_desired_;
    }
    if (!prepare && !playback && chat_wake_read_pending_) {
        chat_audio_work_.read_wake = true;
        chat_audio_work_.wake_serial = chat_wake_read_serial_;
    }
    if (!prepare && !playback && !chat_wake_read_pending_ && cue) {
        chat_audio_work_.cue = true;
        chat_audio_work_.cue_serial = chat_cue_serial_;
        chat_audio_work_.cue_generation = chat_cue_generation_;
        chat_audio_work_.cue_deadline_us = chat_cue_deadline_us_;
        chat_audio_work_.cue_sound = chat_cue_sound_;
        chat_audio_work_.cue_owner = chat_cue_owner_;
        chat_audio_work_.reset_serial = chat_cue_reset_;
    }
    chat_audio_work_.reset = chat_audio_reset_completed_ != chat_audio_work_.reset_serial;
    chat_audio_state_.store(1, std::memory_order_release);
    xTaskNotifyGive(chat_audio_task_);
}

void Application::RunChatAudioCleanup() {
    if (chat_audio_state_.load(std::memory_order_acquire) != 1)
        return;
    try {
        if (chat_audio_work_.cue) {
            chat_audio_work_.cue_result = static_cast<int>(audio_service_.TryPlayChatCue(
                chat_audio_work_.cue_sound, chat_audio_work_.cue_generation,
                chat_audio_work_.reset_serial, chat_audio_work_.cue_deadline_us));
        } else if (chat_audio_work_.read_wake) {
            chat_audio_work_.wake_text = audio_service_.GetLastWakeWord();
        } else if (chat_audio_work_.stop_service) {
            audio_service_.Stop();
            chat_audio_work_.prepared = true;
        } else {
            if (chat_audio_work_.wake_policy == ChatWakePolicy::Listening) {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
                chat_audio_work_.wake = audio_service_.IsAfeWakeWord();
#else
                chat_audio_work_.wake = false;
#endif
            }
            if (chat_audio_work_.reset) {
                chat_audio_work_.reset_done =
                    audio_service_.ResetChatDecoder(chat_audio_work_.reset_serial);
            }
            chat_audio_work_.prepared = (!chat_audio_work_.reset || chat_audio_work_.reset_done) &&
                                        (chat_audio_work_.playback_only ||
                                         audio_service_.PrepareChatAudioTransition(
                                             chat_audio_work_.revoked, chat_audio_work_.processing,
                                             chat_audio_work_.wake, chat_audio_work_.chat_scope));
        }
    } catch (...) {
        chat_audio_work_.prepared = false;
        if (chat_audio_work_.cue)
            chat_audio_work_.cue_result = 3;
    }
    chat_audio_state_.store(2, std::memory_order_release);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}

void Application::RetryChatAudioCleanup() {
    std::unique_lock<std::mutex> cue_lock(chat_cue_mutex_, std::try_to_lock);
    if (!cue_lock.owns_lock())
        return;
    // Called once per application clock tick, not on failure notification.
    const bool retry_cue = chat_cue_retry_;
    if (chat_cue_retry_) {
        if (static_cast<uint64_t>(esp_timer_get_time()) >= chat_cue_deadline_us_) {
            chat_cue_pending_ = false;
            chat_cue_result_ = 3;
        }
        chat_cue_retry_ = false;
    }
    cue_lock.unlock();
    if (retry_cue)
        PollChatAudioCleanup();
    if (chat_playback_fault_ && !chat_audio_exhausted_ &&
        chat_audio_state_.load(std::memory_order_acquire) == 0) {
        chat_playback_attempted_ = 0;
        PollChatAudioCleanup();
    }
    if (chat_audio_fault_ && !chat_audio_exhausted_ &&
        chat_audio_state_.load(std::memory_order_acquire) == 0 &&
        (chat_audio_reset_completed_ != chat_audio_desired_.reset_serial ||
         chat_audio_desired_.stop_service)) {
        chat_audio_completed_revoked_ = 0;
        PollChatAudioCleanup();
    }
}

void Application::BeginChatRebootAudioCleanup() {
    if (chat_reboot_audio_requested_)
        return;
    chat_reboot_audio_requested_ = true;
    RequestChatAudioCleanup(speaking_generation_.load(), false, false, false, true, true);
}

void Application::PollChatReboot() {
    if (chat_reboot_deadline_us_ &&
        static_cast<uint64_t>(esp_timer_get_time()) >= chat_reboot_deadline_us_) {
        chat_reboot_deadline_us_ = 0;
        esp_restart();
    }
}

void Application::ChatAudioCleanupTask(void* context) {
    auto* app = static_cast<Application*>(context);
    for (;;) {
        app->RunChatAudioCleanup();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

bool Application::PollChatProtocolCleanup() {
    using Action = ProtocolWorkLifetime::Action;
    if (!chat_cleanup_enabled_)
        return false;
    if (chat_reboot_audio_requested_)
        return true;
    auto state = chat_protocol_state_.load(std::memory_order_acquire);
    if (state == 2)
        return true;
    if (state == 3) {
        const auto pending = protocol_work_lifetime_.TakeReady();
        if (!chat_protocol_work_.success) {
            chat_protocol_fault_ = true;
            chat_protocol_infrastructure_fault_ = true;
            protocol_work_lifetime_.Request(pending);
            if (pending <= chat_protocol_work_.action)
                return true;
        }
        if (pending > chat_protocol_work_.action) {
            chat_protocol_work_.action = pending;
            chat_protocol_work_.success = false;
            protocol_work_lifetime_.Request(pending);
            chat_protocol_state_.store(1, std::memory_order_release);
            state = 1;
        } else {
            const auto action = chat_protocol_work_.action;
            protocol_ = std::move(chat_protocol_work_.protocol);
            if (action != Action::kClose) {
                protocol_generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            chat_protocol_state_.store(0, std::memory_order_release);
            chat_protocol_owned_.store(false, std::memory_order_release);
            deferred_close_generation_ = 0;
            reset_pending_ = false;
            protocol_reinit_pending_ = false;
            if (action == Action::kReboot) {
                protocol_work_lifetime_.Request(Action::kReboot);
                BeginChatRebootAudioCleanup();
            } else if (action == Action::kReinitialize) {
                InitializeProtocol();
            } else if (action == Action::kReset) {
                protocol_activation_pending_ = ProtocolActivation::kNone;
                claim_protocol_completion_pending_ = false;
            } else if (!protocol_start_pending_generation_) {
                CompleteProtocolActivation();
            }
            return true;
        }
    }
    if (state == 0) {
        if (!protocol_work_lifetime_.Pending())
            return false;
        chat_protocol_owned_.store(true);
        if (lesson_protocol_readers_.load() != 0)
            return true;
        RetireChatOutbound();
        const auto action = protocol_work_lifetime_.TakeReady();
        if (action == Action::kNone)
            return true;
        // TakeReady proved that every real user retired. Restore the barrier
        // before zero-wait admission so a full network queue cannot reopen it.
        protocol_work_lifetime_.Request(action);
        if (chat_protocol_signals_)
            chat_protocol_signals_->Disable();
        chat_protocol_owned_.store(true, std::memory_order_release);
        chat_protocol_work_.protocol = std::move(protocol_);
        chat_protocol_work_.action = action;
        chat_protocol_work_.epoch = deferred_close_epoch_;
        chat_protocol_work_.intentional = connect_close_deferral_.TakeAfterWorker() ||
                                          deferred_close_generation_ != protocol_generation_.load();
        chat_protocol_work_.success = false;
        chat_protocol_work_.destructive_prepared = false;
        connect_in_flight_ = false;
        CancelConnectWatchdog();
        chat_protocol_state_.store(1, std::memory_order_release);
    }
    // Escalation while waiting for capacity updates the owned action in place.
    const auto pending = protocol_work_lifetime_.TakeReady();
    if (pending > chat_protocol_work_.action)
        chat_protocol_work_.action = pending;
    protocol_work_lifetime_.Request(chat_protocol_work_.action);
    if (chat_protocol_work_.action != Action::kClose && !chat_protocol_work_.destructive_prepared) {
        RequestLessonStorageAbandonment();
        CancelLessonRobotEntranceOnDisplay();
        protocol_start_pending_generation_ = 0;
        if (protocol_heap_monitor_pending_) {
            SystemInfo::StopHeapPhaseMonitor();
            protocol_heap_monitor_pending_ = false;
        }
        chat_protocol_work_.destructive_prepared = true;
    }
    if (!open_channel_queue || !open_channel_task) {
        chat_protocol_fault_ = true;
        chat_protocol_infrastructure_fault_ = true;
        return true;
    }
    const NetworkWorkItem work{NetworkWorkKind::kProtocolCleanup, this};
    chat_protocol_state_.store(2, std::memory_order_release);
    if (xQueueSend(open_channel_queue, &work, 0) != pdTRUE) {
        chat_protocol_state_.store(1, std::memory_order_release);
    }
    return true;
}

void Application::RunChatProtocolCleanup() {
    if (chat_protocol_state_.load(std::memory_order_acquire) != 2)
        return;
    try {
        if (chat_protocol_work_.protocol) {
            if (chat_protocol_work_.action == ProtocolWorkLifetime::Action::kClose) {
                if (chat_protocol_work_.intentional)
                    chat_protocol_work_.protocol->CloseAudioChannel();
                else
                    chat_protocol_work_.protocol->CompleteDeferredClose(chat_protocol_work_.epoch);
            } else {
                chat_protocol_work_.protocol.reset();
            }
        }
        chat_protocol_work_.success = true;
    } catch (...) {
        chat_protocol_work_.success = false;
    }
    chat_protocol_state_.store(3, std::memory_order_release);
    xEventGroupSetBits(event_group_, MAIN_EVENT_CHAT_OUTBOUND);
}
