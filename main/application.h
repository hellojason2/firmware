#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include "lesson_transport_epoch_gate.h"
#include "lesson_handler.h"
#include "lesson_embodied_action.h"
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <cstdint>
#include <optional>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "robot_uart.h"
#include "speaking_arm_dispatch.h"
#include "claim_confirmation_reporter.h"
#include "tbot_connect_mapper.h"
#include "connect_close_deferral.h"
#include "protocol_work_lifetime.h"
#include "chat_outbound_worker.h"
#include "chat_protocol_signals.h"
#include "chat_control_intents.h"
#include "chat_inbound_messages.h"
#include "audio/conversation_playout_controller.h"
#include "lesson_audio_playout.h"
#include "backend_recovery_window.h"
#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
#include "course_mode_hil_diagnostic.h"
#endif

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_CHAT_OUTBOUND        (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    struct WifiConfigEntryPreparation {
        DeviceState original_state = kDeviceStateUnknown;
        ListeningMode resume_mode = kListeningModeAutoStop;
        bool resume_realtime = false;
        bool resume_listening = true;
        bool valid = false;
    };

    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();
    void LogPeriodicMetrics();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool TakeChatCaption(ChatCaptionMailbox::Message& caption);
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);
    bool PrepareWifiConfigEntry(WifiConfigEntryPreparation& preparation);
    bool IsWifiConfigEntryPending() const { return wifi_config_preparation_.valid; }
    bool PublishWifiConfigEntry(const WifiConfigEntryPreparation& preparation);
    bool RollbackWifiConfigEntry(const WifiConfigEntryPreparation& preparation);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);
    void ScheduleChatLesson(ChatRequestContext context, std::function<void()> callback);
    void ScheduleDeferredProtocolClose(Protocol* expected, uint32_t connection_epoch);
    bool ScheduleAndWait(std::function<bool()>&& callback, int timeout_ms);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();
    uint32_t BeginLessonInteractiveListeningRequest();
    void PrepareLessonInteractiveListening();
    void PrepareLessonInteractiveListening(uint32_t generation);
    void CancelLessonInteractiveListening();
    void BeginLessonTerminalAudioQuiet();
    void SetLessonRuntimeActive(bool active);
    bool IsLessonRuntimeActive() const;
    // Runtime authority only: wire assignment/session/step/action validation stays
    // in LessonHandler. Tokens rotate across every lesson start/stop transition.
    LessonRuntimeToken GetLessonRuntimeToken() const;
    LessonEmbodiedMotionResult ApplyLessonEmbodiedPreset(
        const LessonRuntimeToken& token,
        const LessonEmbodiedPreset& preset);
    LessonEmbodiedMotionResult CancelLessonEmbodiedAction(
        const LessonRuntimeToken& token);
    LessonEmbodiedMotionResult RestoreLessonRestPose(
        const LessonRuntimeToken& token);
    void BeginLessonNetworkRenderQuiet();
    void EndLessonNetworkRenderQuiet();
    bool IsLessonNetworkRenderQuiet() const;
    bool BeginLessonAssetSyncQuiet();
    void EndLessonAssetSyncQuiet();
    bool IsLessonAssetSyncQuiet() const { return lesson_asset_sync_quiet_.load(); }
#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
    bool RunCourseModeHilTftPattern();
    CourseModeHilSdEvidence RunCourseModeHilSdRead(
        const std::string& relative_path, const std::string& expected_sha256);
    bool RunCourseModeHilAudioDrain();
    bool RunCourseModeHilSafeMotion(int duration_ms);
    bool RunCourseModeHilStopAndRest();
#endif

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot(ChatRequestContext context = {});
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload, ChatRequestContext context = {});
    bool IsChatRequestCurrent(const ChatRequestContext& context) const;
    // Lesson workers also fence retained callbacks by their original lesson epoch.
    bool IsChatLessonRequestCurrent(const ChatRequestContext& context) const;
    void FailChatRequest(const ChatRequestContext& context);
    bool SendLeftArmRaise();
    bool SendRightArmRaise();
    bool SendLeftArmLower();
    bool SendRightArmLower();
    bool SendBothArmsRaise();
    bool SendBothArmsLower();
    bool SendLeftArmSetPercent(int percent);
    bool SendRightArmSetPercent(int percent);
    bool SendBothArmsSetPercent(int percent);
    bool SendHeadTurnLeft();
    bool SendHeadTurnRight();
    bool SendHeadCenter();
    bool SendHeadSetAngle(int angle);
    bool SendHeadSetPercent(int percent);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();
    void SchedulePendingTbotClaimRefresh(uint32_t expected_setup_generation);
    void CompleteCardputerWifiProvisioning(uint64_t ui_generation);
    void PromoteCourseModeFromWifiConfigAfterProvisioning();
    void EnsureBleAdvertisingForUnclaimedSavedWifi();
    // True once the device has been claimed by PersistTbotClaimConfirmationResponse.
    // Claimed robots suppress claim polling and keep normal online BLE off;
    // explicit BOOT Wi-Fi-config mode reopens BluFi for owner reconnect/setup.
    bool IsDeviceClaimed() const;
    bool HasStaleRevokedClaimIdentity() const;

    // BOOT long-press "re-pair": forget the current claim/ownership and re-enter
    // BLE pairing standby so a (possibly different) parent phone can connect and
    // re-claim the robot. Thread-safe (marshals all work onto the Application task).
    // Does NOT block on Wi-Fi/cloud at press time — cloud ownership release is
    // deferred via backend.release_pending (see MaybeDispatchDeferredCloudRelease).
    void EnterRepairPairingMode(ChatRequestContext context = {});

    // Renderer callbacks may run after the inbound cJSON frame is gone. This bridge
    // copies all correlation data into the lesson worker queue by value.
    void EnqueueLessonVisualCompletion(
        LessonQueueItemKind kind,
        std::uint64_t transport_epoch,
        std::uint64_t visual_generation,
        std::int64_t server_sequence,
        const char* assignment_id,
        const char* session_id,
        const char* step_id,
        LessonVisualCompletionResult result,
        const char* degraded_reason = nullptr,
        std::uint64_t visual_nonce = 0);
    void EnqueueLessonEmbodiedCompletion(
        LessonQueueItemKind kind,
        std::uint64_t transport_epoch,
        const char* assignment_id,
        const char* session_id,
        const char* step_id,
        const char* action_id,
        std::uint64_t action_generation,
        std::uint64_t embodied_nonce);

private:
    WifiConfigEntryPreparation wifi_config_preparation_;
    uint32_t wifi_config_audio_revoked_ = 0;
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    std::atomic<uint64_t> protocol_generation_{0};
    std::atomic<uint64_t> cardputer_wifi_completion_generation_{0};
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::atomic<bool> lesson_runtime_active_{false};
    std::atomic<std::uint64_t> lesson_runtime_generation_{0};
    std::atomic<std::uint64_t> lesson_terminal_audio_generation_{0};
    LessonAudioPlayout lesson_audio_playout_;
    std::mutex lesson_playout_mutex_;
    std::atomic<bool> lesson_playout_pending_{false};
    ChatRequestContext lesson_playout_context_;
    std::string lesson_playout_id_, lesson_playout_drain_id_;
    uint32_t lesson_playout_generation_ = 0;
    uint64_t lesson_playout_protocol_generation_ = 0, lesson_playout_epoch_ = 0;
    int64_t lesson_playout_stop_ms_ = 0;
    uint64_t lesson_playout_drained_at_ms_ = 0;
    bool lesson_playout_start_sent_ = false;
    bool lesson_playout_stop_sent_ = false;
    std::shared_ptr<std::atomic<bool>> lesson_playout_authorization_;
    bool QueueLessonPlayoutAck(const char* state, uint64_t at_ms, bool drain = false);
    bool HandleLessonPlayoutTts(const cJSON* root, ChatRequestContext context);
    void PollLessonAudioPlayout();
    std::atomic<uint32_t> lesson_interactive_listen_generation_{0};
    std::atomic<bool> lesson_interactive_listen_pending_{false};
    std::atomic<bool> lesson_interactive_listening_active_{false};
    std::atomic<bool> lesson_idle_repaint_suppressed_{false};
    std::atomic<int> lesson_network_render_quiet_{0};
    std::atomic<bool> lesson_asset_sync_quiet_{false};
    esp_timer_handle_t lesson_asset_sync_wake_rearm_timer_ = nullptr;
    bool lesson_asset_sync_wake_pending_ = false;
    std::atomic<bool> lesson_asset_sync_wake_invalidated_{false};
    uint64_t lesson_asset_sync_wake_deadline_us_ = 0;
    uint32_t lesson_asset_sync_wake_revoked_ = 0;
    bool HasLessonAssetSyncWakeOpportunity();
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    RobotUart robot_uart_;
    SpeakingArmDispatch speaking_arm_dispatch_;
    std::unique_ptr<Ota> ota_;
    // OQ1: main-task-only. pending_tbot_claim_ / claim_substate_ (and the
    // pending_tbot_claim_*_ companions below) are read and written ONLY from the
    // Application task — timer callbacks never touch them directly, they post
    // work back via Schedule(). That serialization is the lock; do not access
    // these from another task without re-routing through Schedule().
    PendingTbotClaim pending_tbot_claim_;
    std::string pending_tbot_claim_api_url_;
    std::string pending_tbot_claim_token_;
    // A 2xx response with unusable credentials may mean the backend committed
    // the one-time claim but the firmware cannot recover device_secret. Freeze
    // this attempt until explicit reset/expiry instead of retrying into 401/409.
    bool claim_confirmation_ambiguous_ = false;

    // TBOT claim runtime FSM sub-state (drives the connect-state mapper).
    // main-task-only (see OQ1 note above).
    TbotClaimSubstate claim_substate_ = TbotClaimSubstate::None;

    // Claim poll (C4): re-fetch /device/config every few seconds while the robot
    // is unclaimed. The backend's 5-minute cap applies only after a pending
    // claim exists; standby keeps polling so late phone scans still work. Posts
    // work to the Application task via Schedule().
    esp_timer_handle_t claim_poll_timer_ = nullptr;       // periodic, ~4s
    int64_t claim_poll_started_ms_ = 0;                   // monotonic window start
    bool claim_poll_active_ = false;
    // Interval the periodic claim poll timer is currently armed at. StartClaimPoll
    // picks the 10s confirm cadence while offline/confirming and a backed-off 60s
    // cadence once the realtime WS is up, so a residual poll can never materially
    // starve live audio. Initialized in StartClaimPoll (the .cc owns the actual
    // kClaimPollInterval*Us values). main-task-only (see OQ1 note above).
    uint64_t claim_poll_interval_us_ = 0;
    // "Hi ESP needs many tries" fix: single-flight guard for the off-task claim
    // fetch worker. The 10s timer must never stack overlapping ClaimFetchTask
    // workers when the flaky backend is slow (leaked tasks/heap). Set true when a
    // worker is spawned, cleared when its continuation runs (or the spawn fails).
    std::atomic<bool> claim_poll_inflight_{false};
    std::atomic<bool> claim_confirm_inflight_{false};
    // Single-flight guard for the deferred BOOT re-pair cloud-ownership release
    // worker (CloudReleaseTask), mirroring claim_poll_inflight_.
    std::atomic<bool> cloud_release_inflight_{false};
    // L2: consecutive /device/config fetch failures. After a couple of misses the
    // standby copy becomes "Server unavailable. Retrying..." instead of the
    // misleading "Ready to connect"; reset to 0 on any successful fetch.
    int claim_fetch_failures_ = 0;

    // Local claim-expiry timer (C4): fires when the parsed expires_at elapses
    // (or the poll window cap is reached) -> CLAIM_CONFIRM_TIMEOUT / "Setup
    // expired". Separate one-shot so the deadline is enforced even between polls.
    esp_timer_handle_t claim_expiry_timer_ = nullptr;     // one-shot
    esp_timer_handle_t claim_assets_retry_timer_ = nullptr; // one-shot, local only

    // Heartbeat (C5): periodic POST /v1/device/heartbeat while claimed/online,
    // carrying backend DTO fields plus ble_state/ap_state/temp from board status.
    esp_timer_handle_t heartbeat_timer_ = nullptr;        // periodic, ~20s
    bool heartbeat_active_ = false;
    std::atomic<int> deferred_heartbeat_auth_failure_status_{0};
    // "Hi ESP needs many tries" fix: single-flight guard for the off-task
    // heartbeat worker, same pattern as claim_poll_inflight_. The 20s timer must
    // never stack overlapping HeartbeatTask workers when the backend is slow.
    std::atomic<bool> heartbeat_inflight_{false};

    // T1/SM-1: connect runs on the persistent network worker so the long blocking
    // OpenAudioChannel() (TCP+TLS handshake + server hello, up to ~20s) never
    // freezes the app task. connect_generation_ invalidates a stale result;
    // connect_in_flight_ describes the watchdog-visible attempt, not ownership.
    // protocol_work_lifetime_ retains queued/running work until app completion.
    std::atomic<uint32_t> connect_generation_{0};
    std::atomic<uint32_t> protocol_callback_connect_generation_{0};
    TaskHandle_t application_task_ = nullptr;
    std::atomic<bool> connect_in_flight_{false};
    std::atomic<bool> reset_pending_{false};
    std::atomic<bool> protocol_reinit_pending_{false};
    std::atomic<bool> reboot_pending_{false};
    ConnectCloseDeferral connect_close_deferral_;
    ProtocolWorkLifetime protocol_work_lifetime_;
    ChatOutboundWorker chat_outbound_worker_;
    TaskHandle_t chat_outbound_task_ = nullptr;
    uint64_t chat_outbound_reservation_ = 0;
    uint64_t chat_outbound_request_id_ = 0;
    uint64_t chat_outbound_last_admitted_id_ = 0;
    uint64_t chat_outbound_protocol_generation_ = 0;
    uint32_t chat_outbound_generation_ = 0;
    uint32_t chat_outbound_connection_epoch_ = 0;
    bool chat_outbound_fault_ = false;
    // Task 4 owns normal-chat routing and current-intent completion effects.
    bool InitializeChatOutboundWorker();
    // Sent from these two admission APIs means ownership was accepted, not that
    // bytes were sent. Only a collected Sent completion, fresh identity, and
    // Task 4's current intent/audio readiness may authorize listening effects.
    ChatOutboundMailbox::Result ActivateChatOutbound(uint32_t connection_epoch);
    ChatOutboundMailbox::Result SubmitChatOutbound(ChatOutboundMailbox::Job& job);
    void RetireChatOutbound();
    bool PollChatOutbound(ChatOutboundMailbox::Completion* completion = nullptr);
    bool IsChatOutboundCompletionCurrent(const ChatOutboundMailbox::Completion& completion) const;
    void NotifyChatOutbound();
    void PollChatOutboundEvents(uint32_t bits);
    static void ChatOutboundTask(void* context);
    enum class ChatWakePolicy { Explicit, Listening };
    struct ChatAudioCleanup {
        uint32_t revoked = 0;
        uint32_t reset_serial = 0;
        bool processing = false, wake = false, chat_scope = true;
        bool reset = false, prepared = false, reset_done = false, stop_service = false;
        bool playback_only = false;
        ChatWakePolicy wake_policy = ChatWakePolicy::Explicit;
        bool read_wake = false;
        uint32_t wake_serial = 0;
        std::string wake_text;
        bool cue = false;
        uint32_t cue_serial = 0, cue_generation = 0;
        uint64_t cue_deadline_us = 0;
        std::string_view cue_sound;
        std::shared_ptr<const std::string> cue_owner;
        int cue_result = 1;
    };
    ChatAudioCleanup chat_audio_desired_{}, chat_audio_work_{};
    // App publishes 1; worker publishes 2 after its final work access.
    std::atomic<uint32_t> chat_audio_state_{0};
    uint32_t chat_audio_reset_serial_ = 0, chat_audio_reset_completed_ = 0;
    uint32_t chat_audio_prepared_ = 0, chat_audio_completed_revoked_ = 0;
    uint32_t chat_playback_desired_ = 0, chat_playback_attempted_ = 0;
    bool chat_playback_fault_ = false;
    bool chat_wake_read_pending_ = false;
    uint32_t chat_wake_read_serial_ = 0;
    std::optional<std::string> chat_wake_read_result_;
    std::string_view chat_cue_sound_;
    std::shared_ptr<const std::string> chat_cue_owner_;
    std::mutex chat_cue_mutex_;
    uint32_t chat_cue_serial_ = 0, chat_cue_generation_ = 0, chat_cue_reset_ = 0;
    uint64_t chat_cue_deadline_us_ = 0;
    bool chat_cue_pending_ = false, chat_cue_retry_ = false;
    int chat_cue_result_ = 1;
    bool RequestChatCue(std::string_view sound);
    TaskHandle_t chat_audio_task_ = nullptr;
    bool chat_audio_fault_ = false, chat_audio_exhausted_ = false;
    bool InitializeChatAudioCleanupWorker();
    bool InitializeChatSourceRoute(bool is_websocket_protocol);
    uint32_t RequestChatPlaybackCleanup(uint32_t playback_generation);
    uint32_t RequestChatAudioCleanup(uint32_t playback_generation, bool reset,
                                     bool processing, bool wake, bool chat_scope = true,
                                     bool stop_service = false,
                                     ChatWakePolicy wake_policy = ChatWakePolicy::Explicit);
    void PollChatAudioCleanup();
    void RunChatAudioCleanup();
    void RetryChatAudioCleanup();
    static void ChatAudioCleanupTask(void* context);
    // Sticky once the new chat path is selected; Task 4b owns selection.
    std::atomic<bool> chat_cleanup_enabled_{false};
    struct ChatProtocolCleanup {
        std::unique_ptr<Protocol> protocol;
        ProtocolWorkLifetime::Action action = ProtocolWorkLifetime::Action::kNone;
        uint32_t epoch = 0;
        bool intentional = false, success = false, destructive_prepared = false;
    } chat_protocol_work_;
    // 0 idle, 1 retained awaiting admission, 2 worker owns, 3 completed.
    std::atomic<uint32_t> chat_protocol_state_{0};
    std::atomic<bool> chat_protocol_owned_{false};
    bool chat_protocol_fault_ = false;
    bool chat_protocol_infrastructure_fault_ = false;
    uint64_t chat_protocol_fault_generation_ = 0;
    uint32_t chat_protocol_fault_era_ = 0;
    std::shared_ptr<ChatProtocolSignals> chat_protocol_signals_;
    void PollChatProtocolSignals();
    Protocol::SourceCallbacks MakeChatSourceCallbacks(
        uint64_t protocol_generation, std::shared_ptr<ChatProtocolSignals> signals);
    bool SelectChatProtocolSource(ConnectionSource source, uint64_t protocol_generation,
                                  uint32_t connect_generation);
    std::atomic<uint32_t> chat_source_connect_generation_{0};
    uint32_t chat_source_open_handled_ = 0;
    uint32_t chat_source_failure_handled_ = 0;
    void PollChatSourceOpen(uint64_t now_us);
    void HandleChatSourceFailure();
    bool EstablishChatPlayoutResponse(const ChatPlayoutIntake::Response& response);
    struct ChatRecoveryIntent {
        // App-owned continuation, never a source-bound wire job. Outcome markers:
        // 1 deferred, 2 cancelled, 3 expired, 4 adopted, 5 rejected, 6 accepted.
        enum class Kind { None, Background, Wake, Listen };
        Kind kind = Kind::None;
        uint64_t protocol_generation = 0, received_us = 0, deadline_us = 0, retry_at_us = 0;
        uint64_t lesson_generation = 0;
        uint32_t connect_generation = 0;
        ListeningMode mode = kListeningModeAutoStop;
        bool ready = false, opening = false, adopted = false, attempted = false;
        bool read_wake = true;
        std::array<char, ChatOutboundMailbox::kMaxPayloadSize + 1> wake_text{};
        size_t wake_size = 0;
    } chat_recovery_;
    void CancelChatRecovery(uint32_t outcome = 2);
    bool RetainChatRecovery(ChatRecoveryIntent::Kind kind, ListeningMode mode);
    void PollChatRecovery(uint64_t now_us);
    bool CompleteChatRecoveryOpen(uint32_t generation, bool success);
    ChatControlIntents chat_control_intents_;
    ChatConnectionMessages chat_connection_messages_;
    ChatInboundMessages chat_inbound_messages_;
    bool IsChatConnectionCurrent(ConnectionSource source, uint64_t protocol_generation, uint32_t connect_generation) const;
    uint64_t RequestChatConnectionText(const std::string& text, ChatRequestContext context = {}, uint64_t received_us = 0,
        std::shared_ptr<std::atomic<bool>> authorization = {});
    void PollChatConnectionMessages(uint64_t now_us);
    bool MaintainChatPassiveLiveness();
    uint64_t chat_passive_ping_id_ = 0;
    ChatConnectionMessages::Owner chat_lesson_capture_owner_;
    uint64_t chat_lesson_capture_epoch_ = 0, chat_lesson_capture_deadline_us_ = 0;
    uint32_t chat_lesson_capture_token_ = 0;
    bool RequestChatLessonCapture();
    void PollChatLessonCapture(uint64_t now_us);
    void BeginChatUnpair(const cJSON* root, ChatRequestContext context);
    void PollChatUnpair(uint64_t now_us);
    ChatRequestContext chat_unpair_context_;
    uint64_t chat_unpair_id_ = 0, chat_unpair_deadline_us_ = 0;
    bool chat_unpair_completed_ = false;
    void PollChatInboundMessages();
    void DispatchIncomingJson(const cJSON* root, uint64_t lesson_epoch, bool is_websocket, ChatRequestContext context = {});
    bool IsLessonVoiceRoute() const;
    void HandleChatLessonAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
        uint64_t protocol_generation, ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet);
    bool IsSelectedNormalChatRoute() const;
    bool HandleChatStopListening();
    bool RetainChatActiveListen();
    enum class ChatListenOrigin { Drain, User, Wake, Abort };
    ChatListenOrigin chat_listen_origin_ = ChatListenOrigin::Drain;
    uint64_t chat_listen_received_us_ = 0;
    bool BeginChatListen(ListeningMode mode, ChatListenOrigin origin);
    bool HandleChatAbort(AbortReason reason, bool resume);
    bool HandleChatWake(const std::string& wake_word, bool read_worker = false);
    bool RequestChatControl(ChatOutboundMailbox::Kind kind, int32_t argument = 0, const std::string& payload = {}, bool read_wake = false);
    void PollChatControls(uint64_t now_us);
    bool DeliverChatControl(const ChatOutboundMailbox::Completion& completion);
    enum class ChatRearmPhase { None, Pending, Armed, IdleComplete, Recovery };
    ChatRearmPhase chat_rearm_phase_ = ChatRearmPhase::None;
    ChatRearmPhase chat_rearm_rendered_phase_ = ChatRearmPhase::None;
    uint32_t chat_rearm_rendered_reset_ = 0;
    bool chat_rearm_rendered_offline_ = false;
    ChatPlayoutIntake::Response chat_rearm_owner_;
    std::shared_ptr<ChatProtocolSignals> chat_rearm_signals_;
    uint32_t chat_rearm_source_era_ = 0;
    ChatOutboundMailbox::Job chat_rearm_job_;
    std::optional<ChatOutboundMailbox::Completion> chat_rearm_delivery_;
    uint32_t chat_rearm_prepared_ = 0;
    bool chat_rearm_admitted_ = false;
    bool chat_rearm_voice_intent_ = false;
    ListeningMode chat_rearm_mode_ = kListeningModeAutoStop;
    bool AdvanceChatRearm(uint64_t now_us);
    bool IsChatRearmRecoveryCurrent() const;
    void RenderChatRearm();
    void HandleChatStart(const std::shared_ptr<ChatProtocolSignals>& signals,
        uint64_t protocol_generation, ConnectionSource source, const cJSON* root, ConnectionReceipt receipt = {});
    void HandleChatAudio(const std::shared_ptr<ChatProtocolSignals>& signals,
        uint64_t protocol_generation, ConnectionSource source, std::unique_ptr<AudioStreamPacket> packet);
    void PollChatStart(uint64_t now_us);
    void RecoverChatStart(const ChatStartHandoff::Request& request, uint32_t site = 0);
    uint32_t chat_start_handled_serial_ = 0, chat_start_failed_serial_ = 0;
    uint32_t chat_start_effects_serial_ = 0;
    uint32_t chat_start_obsolete_generation_ = 0;
    ChatOutboundMailbox::Job chat_start_obsolete_ack_;
    uint64_t chat_start_obsolete_reservation_ = 0;
    void HandleChatTerminalStop(const std::shared_ptr<ChatProtocolSignals>& signals,
        uint64_t protocol_generation, ConnectionSource source, const cJSON* root, uint64_t received_us = 0);
    void PollChatPlayout(uint64_t now_us);
    void RecoverChatPlayout(uint32_t site = 0);
    ConversationPlayoutController chat_playout_controller_;
    ChatPlayoutIntake::Response chat_playout_response_;
    ChatPlayoutIntake::Stop chat_playout_stop_;
    uint32_t chat_playout_stamp_ = 0;
    bool chat_playout_begun_ = false, chat_playout_ready_ = false, chat_playout_recovery_ = false;
    bool chat_playout_cancelled_ = false;
    // Ready is a retained, identity/deadline-bearing handoff, never mic authorization.
    ChatOutboundMailbox::Job chat_playout_ack_;
    uint64_t chat_playout_ack_controller_id_ = 0;
    bool chat_playout_ack_admitted_ = false;
    std::optional<ChatOutboundMailbox::Completion> chat_playout_unhandled_completion_;
    bool PollChatProtocolCleanup();
    void RunChatProtocolCleanup();
    void BeginChatRebootAudioCleanup();
    void PollChatReboot();
    bool chat_reboot_audio_requested_ = false;
    uint64_t chat_reboot_deadline_us_ = 0;
    enum class ProtocolActivation { kNone, kNormal, kWifiReprovision };
    ProtocolActivation protocol_activation_pending_ = ProtocolActivation::kNone;
    bool protocol_heap_monitor_pending_ = false;
    bool claim_protocol_completion_pending_ = false;
    uint64_t protocol_start_pending_generation_ = 0;
    uint64_t deferred_close_generation_ = 0;
    uint32_t deferred_close_epoch_ = 0;
    // WSS-8: true from the start of a wake/listen/reconnect connect cycle until it
    // succeeds or the user/system cancels the online intent. While true, a per-attempt SetError is a
    // RECOVERABLE transient (the wake loop / ScheduleReconnect backoff retries),
    // so MAIN_EVENT_ERROR keeps the calm "connecting/idle" view instead of flashing
    // "Server unavailable. Retrying...". Wake-open exhaustion still surfaces a
    // terminal error; listen-mode reconnect stays in long-horizon auto-reconnect.
    // Cleared on OnAudioChannelOpened (success).
    std::atomic<bool> connect_attempt_active_{false};
    esp_timer_handle_t connect_watchdog_timer_ = nullptr;   // one-shot (SM-3)
    // T2/WSS-4: long-horizon auto-reconnect with fast backoff, then slow periodic retry.
    esp_timer_handle_t reconnect_timer_ = nullptr;          // one-shot
    int reconnect_attempt_ = 0;
    int passive_reconnect_attempt_ = 0;
    BackendRecoveryWindow backend_recovery_window_{60000};
    ListeningMode reconnect_mode_ = kListeningModeAutoStop;
    std::atomic<bool> reconnect_passive_{false};
    std::atomic<bool> reconnect_resume_listening_{true};
    // Sustained operation: true while we WANT an open audio channel. Set on
    // OnAudioChannelOpened; cleared by CloseAudioChannelByIntent() for every
    // user/system-initiated close. An UNEXPECTED drop leaves it true ->
    // OnAudioChannelClosed auto-reconnects so long talks aren't cut off.
    std::atomic<bool> online_intent_{false};
    // Passive lesson/nudge WebSocket: keep a claimed robot reachable by ESP
    // server without entering Listening or scheduling listen-mode reconnects.
    std::atomic<bool> passive_ws_intent_{false};
    std::string deferred_wake_word_;
    QueueHandle_t lesson_message_queue_ = nullptr;
    TaskHandle_t lesson_message_task_handle_ = nullptr;
    std::atomic<bool> lesson_message_stop_{false}, lesson_message_retired_{false};
    std::atomic<unsigned> lesson_message_producers_{0};
    void StopLessonMessageTask();
    std::atomic<uint32_t> lesson_protocol_readers_{0};
    LessonTransportEpochGate lesson_transport_epoch_gate_;
    LessonTransportTerminalControl lesson_terminal_control_;
    LessonQueueDataAdmission lesson_queue_data_admission_{kLessonMessageDataQueueDepth};

    // Set on a backend/ws error, cleared on (re)connect. Lets the connect mapper
    // tell ONLINE apart from OFFLINE_RETRY ("Server unavailable. Retrying...").
    std::atomic<bool> backend_offline_{false};

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    std::atomic<bool> tts_audio_accepting_{false};
    std::atomic<uint32_t> speaking_generation_{0};
    std::atomic<uint32_t> speaking_timeout_generation_{0};
    esp_timer_handle_t speaking_timeout_timer_ = nullptr; // one-shot
    std::atomic<int64_t> last_speaking_activity_ms_{0};
    std::atomic<int64_t> listening_started_ms_{0};
    std::atomic<int64_t> last_listening_activity_ms_{0};
    std::atomic<bool> microphone_uplink_authorized_{false};
    std::atomic<uint32_t> interrupt_count_{0};   // OBS-2: barge-in / abort count
    std::atomic<uint32_t> reconnect_count_{0};   // OBS-2: WS reconnect attempts

    // Monotonic ms timestamp of last VAD speech-start. Used to gate wake-word
    // transitions: if no human-voice VAD trigger occurred recently, drop the
    // wake-word event as a likely false-positive on ambient noise.
    int64_t last_vad_speech_ms_ = 0;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void RearmClaimedIdleWakeWord();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ScheduleLessonAssetSyncWakeRearm();
    void ScheduleLessonAssetSyncWakeRearm(uint64_t delay_us);
    void RunScheduledTasks();
    void ArmSpeakingTimeout();
    void HandleSpeakingTimeout(uint32_t generation);
    void HandleListeningWatchdogTick();
    bool IsMicrophoneUplinkAuthorized() const;
    void ContinueOpenAudioChannel(ListeningMode mode);
    void StartPassiveLessonWebsocket();
    bool StartOpenChannelWorker(void* context);
    static void OpenChannelTask(void* arg);            // T1: blocking connect, off app task
    void DoResetProtocol();                            // T1: actual reset (worker-safe)
    void ArmConnectWatchdog();                         // SM-3: bound CONNECTING
    void CancelConnectWatchdog();
    void HandleConnectWatchdog(uint32_t generation);
    void ScheduleReconnect(ListeningMode mode, bool resume_listening = true);        // T2/WSS-4: long-horizon backoff+jitter
    void SchedulePassiveLessonReconnect();             // passive lesson/nudge socket retry
    void HandleReconnectTick();
    void CloseAudioChannelByIntent();                  // intentional close -> clears online_intent_
    bool IsConnectSuccessPublicationSuppressed() const;
    void CompleteReboot();
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void FinishWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();
    void CompleteUnclaimedProtocolOnlyActivation();
    void CompleteClaimedWifiReprovisionActivation();
    bool EnsureLocalAssetsAppliedForClaim();
    bool FinishClaimActivationAfterLocalAssetsReady();
    void ScheduleClaimLocalAssetsRetry();
    void HandleClaimLocalAssetsRetry();
    void ReloadProtocolAfterClaimCredentials();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void RequestInitializeProtocol(ProtocolActivation activation = ProtocolActivation::kNone);
    void InitializeProtocol();
    bool CompletePendingProtocolWork();
    void CompleteProtocolActivation();
    void CompleteClaimProtocolActivation();
    void StartProtocolWorker();
    void RefreshPendingTbotClaim();
    void DispatchPendingTbotClaimRefreshForSetupGeneration(
        uint32_t expected_setup_generation);
    // BluFi reported STA-connected success after a (re-)provisioning. The BLE
    // build never leaves kDeviceStateWifiConfiguring on its own (no ConfigModeExit
    // and HandleNetworkConnectedEvent ignores Connected in that state), so the
    // claim FSM would otherwise dead-end in setup and the claim would only
    // complete after an extra manual power-cycle. This drives the FSM out of
    // WifiConfiguring via a lightweight activation that reuses persisted runtime
    // configuration on the genuine provisioning-success entry point only; the
    // stale-event guards on HandleNetworkConnectedEvent/RefreshPendingTbotClaim
    // are left untouched.
    void PromoteFromWifiConfigAfterProvisioning();
    enum class ClaimBleLifecycleIntent {
        kNone,
        kEnsureAdvertising,
        kStopAdvertising,
        kCompleteSuccessfulTeardown,
    };
    struct ClaimDeferredEffects {
        ClaimBleLifecycleIntent ble_intent = ClaimBleLifecycleIntent::kNone;
        bool dispatch_confirmation = false;
        bool dispatch_refresh = false;
        bool restore_standby_after_dispatch_failure = false;
    };
    // Claim fetch and provisioning-promotion results first commit their state under
    // RunIfSetupGenerationCurrent. A post-gate BOOT restart must still suppress the
    // stale confirmation or refresh worker, so ExecuteClaimDeferredEffects reserves
    // the lifecycle, briefly validates finalization, and commits bounded dispatch
    // plus any dispatch-failure poll/standby fallback before unlock. Worker result
    // application retains its own generation gate.
    void ExecuteClaimDeferredEffects(
        const ClaimDeferredEffects& effects, uint32_t expected_setup_generation,
        WakeWordLifecycleController::ProvisioningToken provisioning_token = {});
    bool RunClaimDispatchForSetupGeneration(
        uint32_t expected_setup_generation, const std::function<void()>& action);
    bool ConfirmPendingTbotClaim(bool trust_backend_expiry = false);
    bool DispatchPendingTbotClaimConfirmation(uint32_t expected_setup_generation,
                                              bool enforce_setup_generation);
    static void ClaimConfirmationTask(void* arg);
    bool ApplyPendingTbotClaimConfirmationResult(
        ClaimConfirmationResult result,
        WakeWordLifecycleController::ProvisioningToken provisioning_token,
        bool defer_successful_teardown = false,
        ClaimDeferredEffects* deferred_effects = nullptr);
    // "Hi ESP needs many tries" fix: the blocking ~3s /device/config HTTP/TLS
    // fetch is split out of RefreshPendingTbotClaim() so it can run on a
    // dedicated low-priority worker instead of the priority-10 Application task
    // (which starved the wake-word AFE fetch/feed pipeline during the
    // boot-to-first-WS window). DispatchPendingTbotClaimFetch() spawns the
    // worker (single-flight); ClaimFetchTask() does ONLY the blocking fetch and
    // Schedule()s ApplyPendingTbotClaimFetchResult() back onto the Application
    // task, where all claim_substate_/BLE/SetDeviceState mutation stays serialized.
    bool DispatchPendingTbotClaimFetch(const std::string& api_url, const std::string& token,
                                       bool apply_when_poll_inactive = false,
                                       uint32_t expected_setup_generation = 0,
                                       bool enforce_setup_generation = false);
    static void ClaimFetchTask(void* arg);
    void ApplyPendingTbotClaimFetchResult(const std::string& api_url,
                                          const std::string& token,
                                          const PendingTbotClaim& pending_claim,
                                          bool fetched, int device_config_status,
                                          bool defer_confirmation = false,
                                          uint32_t expected_setup_generation = 0,
                                          ClaimDeferredEffects* deferred_effects = nullptr);

    // Deferred cloud-ownership release for the BOOT re-pair flow.
    // EnterRepairPairingMode() sets backend.release_pending and KEEPS the device
    // credentials; once the robot is online + unclaimed,
    // MaybeDispatchDeferredCloudRelease() (called from RefreshPendingTbotClaim)
    // spawns a low-prio worker that POSTs /v1/devices/:id/factory-reset off the
    // prio-10 task (same single-flight pattern as the claim fetch), then clears
    // release_pending + device_secret so a different parent account can re-claim.
    void MaybeDispatchDeferredCloudRelease();
    static void CloudReleaseTask(void* arg);

    // --- TBOT claim runtime FSM helpers (C4) ---
    // Render the screen copy for the current claim sub-state from the connect
    // mapper (single source of truth = kTbotConnectStateSpecs).
    void RenderClaimSubstate(TbotClaimSubstate substate);
    // Begin / stop the bounded /device/config poll for the claim window.
    void StartClaimPoll();
    void StopClaimPoll();
    void PollPendingTbotClaimTick();
    // Arm / cancel the local expires_at deadline for a pending claim.
    void ArmClaimExpiryTimer();
    void CancelClaimExpiryTimer();
    void HandleClaimConfirmTimeout();

    // --- BLE discoverability for unclaimed standby / explicit setup ---
    // The mobile app pairs BLE-first: it scans for the BluFi advertisement
    // "TBOT-<MAC>" to find the robot, then drives claim or Wi-Fi setup. Keep
    // advertising available for unclaimed standby. Claimed online uses the BOOT
    // Wi-Fi-config path to reopen BluFi so BLE does not contend with AFE audio.
    // No-ops in non-BluFi builds.
    void EnsureBleAdvertisingForStandby();
    bool EnsureBleAdvertisingForStandbyForSetupGeneration(
        uint32_t expected_generation, const std::function<void()>& on_current = {});
    bool EnsureBleAdvertisingForStandbyImpl(
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current = {});
    void StopBleAdvertising();
    bool StopBleAdvertisingForSetupGeneration(
        uint32_t expected_generation, const std::function<void()>& on_current = {});
    bool StopBleAdvertisingImpl(
        std::optional<uint32_t> expected_generation,
        const std::function<void()>& on_current = {});

    // --- Heartbeat (C5) ---
    bool ShouldKeepManagementHeartbeat() const;
    void StartHeartbeat();
    void StopHeartbeat();
    void HandleHeartbeatAuthFailure(int status_code);
    // "Hi ESP needs many tries" fix: the heartbeat does a blocking ~5s HTTP/TLS
    // POST. Like the claim poll, run it OFF the priority-10 Application task so a
    // slow backend can never freeze the core-0 wake-word AFE feed/fetch pipeline.
    // DispatchDeviceHeartbeat() runs on the Application task (gating + body build),
    // HeartbeatTask() runs one blocking POST on the shared persistent network
    // worker, then Schedule()s auth-failure handling back onto the Application task.
    void DispatchDeviceHeartbeat();
    static void HeartbeatTask(void* arg);
    // Off-task worker body: the blocking POST. Returns the HTTP status code (0 on
    // transport failure); the caller Schedule()s auth-failure handling back.
    int SendDeviceHeartbeat(const std::string& url, const std::string& device_secret,
                            std::string body);

    // Current BLE setup sub-state for the connect mapper (Off in AP/other builds).
    TbotBleSubstate GetBleSubstate() const;
    bool HandleRobotActionMessage(const cJSON* root, ChatRequestContext context = {});
    void EnqueueLessonMessage(const cJSON* root, std::uint64_t transport_epoch, ChatRequestContext context = {});
    void RequestLessonStorageAbandonment();
    static void LessonMessageTask(void* arg);
    // US-006 Slice-01 (S10): additive lesson_* renderer entry — see lesson_handler.cc.
    // Reached only via the additive `lesson_` branch in OnIncomingJson, above the
    // unknown-type no-op. Never touches the 8 legacy types / voice / MCP arm tools.
    void HandleLessonMessage(const cJSON* root, ChatRequestContext context = {});
    bool AbandonLessonStorageSession();
    void HandleEmotionGesture(const char* emotion);
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
