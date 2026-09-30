#include "application_internal.h"

void Application::StopLessonMessageTask() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    if (!lesson_message_task_handle_)
        return;
    lesson_message_stop_.store(true);
    while (lesson_message_producers_.load())
        vTaskDelay(pdMS_TO_TICKS(1));
    LessonQueueItem wake{LessonQueueItemKind::kAbandonTransport, nullptr, 0};
    // A full queue already wakes the worker; never wait for an extra slot.
    xQueueSendToFront(lesson_message_queue_, &wake, 0);
    // IDF eTaskGetState holds the kernel lock and reports either running core
    // before eSuspended. This task has no resume path once its locals unwind.
    while (!lesson_message_retired_.load() ||
           eTaskGetState(lesson_message_task_handle_) != eSuspended)
        vTaskDelay(pdMS_TO_TICKS(1));
    vTaskDelete(lesson_message_task_handle_);
    lesson_message_task_handle_ = nullptr;
#endif
}

void Application::EnqueueLessonVisualCompletion(
    LessonQueueItemKind kind, std::uint64_t transport_epoch, std::uint64_t visual_generation,
    std::int64_t server_sequence, const char* assignment_id, const char* session_id,
    const char* step_id, LessonVisualCompletionResult result, const char* degraded_reason,
    std::uint64_t visual_nonce) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer)
        return;
    if ((kind != LessonQueueItemKind::kVisualCompleted &&
         kind != LessonQueueItemKind::kVisualTimedOut) ||
        lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        return;
    }
    LessonQueueItem item = MakeLessonVisualQueueItem(
        kind, transport_epoch, visual_generation, server_sequence, assignment_id, session_id,
        step_id, result, degraded_reason, visual_nonce);
    if (!lesson_queue_data_admission_.TryAcquire()) {
        ESP_LOGW(TAG, "lesson visual completion dropped: data capacity full seq=%ld",
                 static_cast<long>(server_sequence));
        return;
    }
    if (xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        lesson_queue_data_admission_.Release();
        ESP_LOGW(TAG, "lesson visual completion dropped: worker queue full seq=%ld",
                 static_cast<long>(server_sequence));
    }
#else
    (void)kind;
    (void)transport_epoch;
    (void)visual_generation;
    (void)server_sequence;
    (void)assignment_id;
    (void)session_id;
    (void)step_id;
    (void)result;
    (void)degraded_reason;
    (void)visual_nonce;
#endif
}

void Application::EnqueueLessonEmbodiedCompletion(LessonQueueItemKind kind,
                                                  std::uint64_t transport_epoch,
                                                  const char* assignment_id, const char* session_id,
                                                  const char* step_id, const char* action_id,
                                                  std::uint64_t action_generation,
                                                  std::uint64_t embodied_nonce) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer)
        return;
    if ((kind != LessonQueueItemKind::kEmbodiedHoldCompleted &&
         kind != LessonQueueItemKind::kEmbodiedSettled) ||
        lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        return;
    }
    LessonQueueItem item{};
    item.kind = kind;
    item.transport_epoch = transport_epoch;
    item.action_generation = action_generation;
    item.embodied_nonce = embodied_nonce;
    std::snprintf(item.assignment_id, sizeof(item.assignment_id), "%s", assignment_id);
    std::snprintf(item.session_id, sizeof(item.session_id), "%s", session_id);
    std::snprintf(item.step_id, sizeof(item.step_id), "%s", step_id);
    std::snprintf(item.action_id, sizeof(item.action_id), "%s", action_id);
    if (!lesson_queue_data_admission_.TryAcquire())
        return;
    if (xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        lesson_queue_data_admission_.Release();
    }
#else
    (void)kind;
    (void)transport_epoch;
    (void)assignment_id;
    (void)session_id;
    (void)step_id;
    (void)action_id;
    (void)action_generation;
    (void)embodied_nonce;
#endif
}

void Application::EnqueueLessonMessage(const cJSON* root, std::uint64_t transport_epoch,
                                       ChatRequestContext context) {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer)
        return;
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
    const char* type_value = cJSON_IsString(type) ? type->valuestring : "(missing)";
    const int sequence_value = cJSON_IsNumber(sequence) ? sequence->valueint : -1;

    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr) {
        FailChatRequest(context);
        ESP_LOGW(TAG, "lesson_* dropped: worker unavailable type=%s seq=%d", type_value,
                 sequence_value);
        return;
    }

    LogLessonHeapBoundary("enqueue.before_serialize", 0);
    char* payload = cJSON_PrintUnformatted(root);
    const size_t payload_bytes = payload != nullptr ? strlen(payload) : 0;
    LogLessonHeapBoundary("enqueue.after_serialize", payload_bytes);
    if (payload == nullptr) {
        FailChatRequest(context);
        ESP_LOGW(TAG, "lesson_* dropped: serialize failed type=%s seq=%d", type_value,
                 sequence_value);
        return;
    }

    LessonQueueItem item{
        LessonQueueItemKind::kFrame,
        payload,
        transport_epoch,
    };
    if (context) {
        item.source_context = new (std::nothrow) ChatRequestContext(context);
        if (!item.source_context) {
            cJSON_free(payload);
            FailChatRequest(context);
            return;
        }
    }
    const bool queue_full =
        uxQueueMessagesWaiting(lesson_message_queue_) >= kLessonMessageQueueDepth;
    const bool admitted = !queue_full && lesson_queue_data_admission_.TryAcquire();
    if (!admitted || xQueueSend(lesson_message_queue_, &item, 0) != pdTRUE) {
        if (admitted)
            lesson_queue_data_admission_.Release();
        ESP_LOGW(TAG, "lesson_* dropped: worker queue full type=%s seq=%d", type_value,
                 sequence_value);
        cJSON_free(payload);
        delete static_cast<ChatRequestContext*>(item.source_context);
        FailChatRequest(context);
    } else {
        ESP_LOGI(TAG, "lesson_* enqueued type=%s seq=%d bytes=%u", type_value, sequence_value,
                 (unsigned)payload_bytes);
    }
#else
    (void)root;
    (void)transport_epoch;
    (void)context;
#endif
}

void Application::RequestLessonStorageAbandonment() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    LessonQueueProducer producer(lesson_message_producers_, lesson_message_stop_);
    if (!producer)
        return;
    Schedule([]() { CancelLessonRobotEntranceOnDisplay(); });
    if (lesson_message_queue_ == nullptr || lesson_message_task_handle_ == nullptr)
        return;
    const std::uint64_t terminal_epoch = lesson_transport_epoch_gate_.PublishTerminalEpoch();
    const auto terminal_request = lesson_terminal_control_.Publish(terminal_epoch);
    if (!terminal_request.enqueue_control)
        return;
    LessonQueueItem item{
        LessonQueueItemKind::kAbandonTransport,
        nullptr,
        terminal_epoch,
    };
    if (xQueueSendToFront(lesson_message_queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG,
                 "lesson abandonment wakeup enqueue failed; worker will drain published epoch");
    }
#endif
}

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
void Application::LessonMessageTask(void* arg) {
    auto* self = static_cast<Application*>(arg);
    LessonQueueItem item;
    struct LessonProtocolRead {
        std::atomic<uint32_t>& readers;
        explicit LessonProtocolRead(std::atomic<uint32_t>& value) : readers(value) {
            readers.fetch_add(1);
        }
        ~LessonProtocolRead() { readers.fetch_sub(1); }
    };
    const auto drain_terminal = [self]() {
        constexpr int kLessonStorageAbandonMaxAttempts = 4;
        constexpr uint32_t kLessonStorageAbandonRetryDelayMs = 10;
        if (!self->lesson_terminal_control_.WorkerShouldDrain()) {
            return false;
        }
        for (;;) {
            const std::uint64_t latest_epoch = self->lesson_transport_epoch_gate_.PublishedEpoch();
            if (self->lesson_transport_epoch_gate_.WorkerApplyTerminal(latest_epoch)) {
                InvalidateLessonVisualCompletionState(latest_epoch);
                self->Schedule([]() { CancelLessonRobotEntranceOnDisplay(); });
            }
            if (!self->lesson_terminal_control_.FinishWorkerDrain(
                    self->lesson_transport_epoch_gate_, latest_epoch)) {
                break;
            }
        }
        bool released = false;
        for (int attempt = 0; attempt < kLessonStorageAbandonMaxAttempts; ++attempt) {
            if (self->AbandonLessonStorageSession()) {
                released = true;
                break;
            }
            if (attempt + 1 < kLessonStorageAbandonMaxAttempts) {
                vTaskDelay(pdMS_TO_TICKS(kLessonStorageAbandonRetryDelayMs));
            }
        }
        if (!released) {
            LessonAssetStorageCoordinator::GetInstance().ForceEndLessonSession();
            self->AbandonLessonStorageSession();
        }
        return true;
    };
    const auto poll_cinematic_error = [self]() {
        const auto epoch = PendingLessonCinematicErrorEpoch();
        if (epoch == 0)
            return;
        LessonProtocolRead protocol_read(self->lesson_protocol_readers_);
        if (!self->chat_protocol_owned_.load() &&
            self->lesson_transport_epoch_gate_.WorkerAcceptFrame(epoch)) {
            try {
                DispatchPendingLessonCinematicError(self->protocol_.get());
            } catch (...) {
                // Allocation or transport exceptions leave the diagnostic pending
                // for the next bounded poll, without terminating this worker.
            }
        }
    };
    while (!self->lesson_message_stop_.load()) {
        drain_terminal();
        poll_cinematic_error();
        if (xQueueReceive(self->lesson_message_queue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        std::unique_ptr<ChatRequestContext> source_context(
            static_cast<ChatRequestContext*>(item.source_context));
        item.source_context = nullptr;
        const ChatRequestContext context = source_context ? *source_context : ChatRequestContext();
        if (item.kind == LessonQueueItemKind::kAbandonTransport) {
            drain_terminal();
            continue;
        }
        self->lesson_queue_data_admission_.Release();
        drain_terminal();
        LessonProtocolRead protocol_read(self->lesson_protocol_readers_);
        if (!self->lesson_transport_epoch_gate_.WorkerAcceptFrame(item.transport_epoch) ||
            self->chat_protocol_owned_.load() || !self->IsChatLessonRequestCurrent(context)) {
            if (item.kind == LessonQueueItemKind::kFrame && item.payload != nullptr) {
                cJSON_free(item.payload);
                item.payload = nullptr;
            }
            continue;
        }
        if (item.kind == LessonQueueItemKind::kVisualCompleted ||
            item.kind == LessonQueueItemKind::kVisualTimedOut) {
            DispatchLessonVisualCompletion(item, self->protocol_.get(), &self->robot_uart_);
            continue;
        }
        if (item.kind == LessonQueueItemKind::kEmbodiedHoldCompleted ||
            item.kind == LessonQueueItemKind::kEmbodiedSettled) {
            DispatchLessonEmbodiedCompletion(item, self->protocol_.get());
            continue;
        }
        if (item.kind != LessonQueueItemKind::kFrame || item.payload == nullptr)
            continue;
        const size_t payload_bytes = strlen(item.payload);
        LogLessonWorkerStackWatermark("before_parse");
        LogLessonHeapBoundary("worker.before_parse", payload_bytes);
        cJSON* root = cJSON_Parse(item.payload);
        LogLessonHeapBoundary("worker.after_parse", payload_bytes);
        if (root != nullptr) {
            const cJSON* type = cJSON_GetObjectItem(root, "type");
            const cJSON* sequence = cJSON_GetObjectItem(root, "sequence");
            ESP_LOGI(TAG, "lesson_worker handling type=%s seq=%d",
                     cJSON_IsString(type) ? type->valuestring : "(missing)",
                     cJSON_IsNumber(sequence) ? sequence->valueint : -1);
            SetLessonTransportEpoch(item.transport_epoch);
            try {
                self->HandleLessonMessage(root, context);
            } catch (...) {
                self->FailChatRequest(context);
            }
            LogLessonWorkerStackWatermark("after_handle");
            LogLessonHeapBoundary("worker.after_handle", payload_bytes);
            cJSON_Delete(root);
            LogLessonHeapBoundary("worker.after_delete", payload_bytes);
        } else {
            self->FailChatRequest(context);
            ESP_LOGW(TAG, "lesson_* dropped: worker parse failed");
        }
        cJSON_free(item.payload);
        item.payload = nullptr;
        LogLessonHeapBoundary("worker.after_payload_free", payload_bytes);
    }
    // Publish only after the current frame's context and protocol reader unwind.
    self->lesson_message_retired_.store(true);
    vTaskSuspend(nullptr);
}
#endif
