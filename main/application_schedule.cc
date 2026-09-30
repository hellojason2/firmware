#include "application_internal.h"

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ScheduleDeferredProtocolClose(Protocol* expected, uint32_t connection_epoch) {
    const uint64_t expected_generation = protocol_generation_.load(std::memory_order_acquire);
    Schedule([this, expected, expected_generation, connection_epoch]() {
        if (ProtocolLifetimeMatches(protocol_.get(), expected,
                                    protocol_generation_.load(std::memory_order_acquire),
                                    expected_generation)) {
            RetireChatOutbound();
            if (chat_cleanup_enabled_) {
                deferred_close_generation_ = expected_generation;
                deferred_close_epoch_ = connection_epoch;
                protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                PollChatProtocolCleanup();
                return;
            }
            if (protocol_work_lifetime_.Busy()) {
                deferred_close_generation_ = expected_generation;
                deferred_close_epoch_ = connection_epoch;
                protocol_work_lifetime_.Request(ProtocolWorkLifetime::Action::kClose);
                return;
            }
            protocol_->CompleteDeferredClose(connection_epoch);
        }
    });
}

bool Application::ScheduleAndWait(std::function<bool()>&& callback, int timeout_ms) {
    struct WaitState {
        enum Status { kPending, kRunning, kDone, kCancelled };
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        std::atomic<Status> status{kPending};
        std::atomic<bool> result{false};
        ~WaitState() {
            if (done != nullptr)
                vSemaphoreDelete(done);
        }
    };
    auto state = std::make_shared<WaitState>();
    if (state->done == nullptr)
        return false;
    Schedule([state, callback = std::move(callback)]() mutable {
        auto expected = WaitState::kPending;
        if (!state->status.compare_exchange_strong(expected, WaitState::kRunning))
            return;
        state->result.store(callback());
        state->status.store(WaitState::kDone);
        xSemaphoreGive(state->done);
    });
    if (xSemaphoreTake(state->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return state->result.load();
    }
    auto expected = WaitState::kPending;
    if (state->status.compare_exchange_strong(expected, WaitState::kCancelled))
        return false;
    if (expected == WaitState::kRunning) {
        xSemaphoreTake(state->done, portMAX_DELAY);
    }
    return state->status.load() == WaitState::kDone && state->result.load();
}

void Application::RunScheduledTasks() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto tasks = std::move(main_tasks_);
    lock.unlock();
    for (auto& task : tasks) {
        task();
    }
}
