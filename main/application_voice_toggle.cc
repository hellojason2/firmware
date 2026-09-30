#include "application_internal.h"

void Application::StopListening() {
    const bool lesson_answer_turn =
        lesson_interactive_listen_pending_.load() || lesson_interactive_listening_active_.load();
    if (lesson_runtime_active_.load() && !lesson_answer_turn) {
        ESP_LOGI(TAG, "lesson stop listening ignored state=%d", static_cast<int>(GetDeviceState()));
        return;
    }
    if (!(GetDeviceState() == kDeviceStateSpeaking && lesson_interactive_listen_pending_.load())) {
        CancelLessonInteractiveListening();
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    if (IsWifiConfigEntryPending())
        return;
    auto state = GetDeviceState();

    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson toggle ignored state=%d", static_cast<int>(state));
        return;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        if (!audio_service_.IsRunning()) {
            ESP_LOGI(TAG, "Audio test unavailable while provisioning workers are deferred");
            return;
        }
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (pending_tbot_claim_.active) {
        ConfirmPendingTbotClaim();
        return;
    }

    // H1: "Setup expired" (CLAIM_CONFIRM_TIMEOUT) is not a dead-end. A button tap
    // while in ConfirmTimeout re-enters the bounded standby poll (the documented
    // ConfirmTimeout -> AvailableStandby recovery), so the screen returns to
    // "Ready to connect" instead of stranding. Route the tap to claim-retry here
    // rather than letting it fall through to the talk path below.
    if (claim_substate_ == TbotClaimSubstate::ConfirmTimeout) {
        ESP_LOGI(TAG, "Claim confirm timeout -> retry: re-entering claim standby poll");
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && backend_offline_.load() &&
        (state == kDeviceStateIdle || state == kDeviceStateConnecting ||
         state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap from offline retry -> reopening phone scan standby");
        backend_offline_.store(false);
        if (state != kDeviceStateIdle) {
            SetDeviceState(kDeviceStateIdle);
        }
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!IsDeviceClaimed() && state == kDeviceStateIdle &&
        (claim_substate_ == TbotClaimSubstate::AvailableStandby ||
         claim_substate_ == TbotClaimSubstate::None)) {
        ESP_LOGI(TAG, "Unclaimed BOOT tap -> refreshing claimable standby for phone scan");
        claim_substate_ = TbotClaimSubstate::AvailableStandby;
        RenderClaimSubstate(claim_substate_);
        RefreshPendingTbotClaim();
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    if (IsSelectedNormalChatRoute()) {
        if (state == kDeviceStateIdle)
            BeginChatListen(GetDefaultListeningMode(), ChatListenOrigin::User);
        else if (state == kDeviceStateSpeaking)
            HandleChatAbort(kAbortReasonNone, listening_mode_ != kListeningModeManualStop);
        else if (state == kDeviceStateListening)
            CloseAudioChannelByIntent();
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        CloseAudioChannelByIntent();
    }
}

