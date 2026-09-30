#ifndef APPLICATION_INTERNAL_H_
#define APPLICATION_INTERNAL_H_

#include "application.h"
#include "app_manager.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "chat_runtime_timing.h"
#include "display.h"
#include "display/lvgl_display/lvgl_display.h"
#include "lesson_queue_producer.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "robot_uart.h"
#include "settings.h"
#include "system_info.h"
#include "tbot_connect_mapper.h"
#include "websocket_protocol.h"
#include "wifi_config_entry_policy.h"
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
#include "lesson_asset_storage_coordinator.h"
#include "lesson_heap_probe.h"
#endif
#include "esp_build_identity.h"
#include "passive_reconnect_policy.h"
#include "protocol_lifetime_token.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include <ssid_manager.h>
#include <wifi_manager.h>
#include "boards/common/blufi.h"
#include "boards/common/system_reset.h"
#include "boards/common/wifi_board.h"
#endif

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <font_awesome.h>
#include <freertos/idf_additions.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include "course_mode_hil_sd_reader.h"
#endif

#ifndef TAG
#define TAG "Application"
#endif

static constexpr uint32_t kListenPlaybackDrainTimeoutMs = 650;
static constexpr uint32_t kSpeakingTimeoutMs = 12000;
static constexpr uint32_t kTtsStopPlaybackDrainTimeoutMs = 2000;
static constexpr uint32_t kListeningNoSpeechTimeoutMs = 15000;
static constexpr uint32_t kListeningAutoStopMaxTurnMs = 10000;
static constexpr uint32_t kListeningMaxTurnMs = 60000;
static constexpr uint32_t kListeningRealtimeNoSpeechTimeoutMs = 15000;

static inline void CancelLessonRobotEntranceOnDisplay() {
    Display* display = Board::GetInstance().GetDisplay();
    if (auto* lvgl_display = dynamic_cast<LvglDisplay*>(display)) {
        lvgl_display->CancelLessonRobotEntrance();
    }
}

inline void SecureClearString(std::string& value) {
    if (!value.empty()) {
        volatile char* bytes = &value[0];
        for (std::size_t i = 0; i < value.size(); ++i) {
            bytes[i] = '\0';
        }
    }
    value.clear();
}

class SecureStringScope {
public:
    explicit SecureStringScope(std::string& value) : value_(value) {}
    ~SecureStringScope() { SecureClearString(value_); }

    SecureStringScope(const SecureStringScope&) = delete;
    SecureStringScope& operator=(const SecureStringScope&) = delete;

private:
    std::string& value_;
};

struct ConnectContext {
    Application* app;
    ListeningMode mode;
    uint32_t generation;
    std::string wake_word;
    bool wake_word_invoke = false;
    bool passive_preconnect = false;
    Protocol* protocol = nullptr;
    uint64_t protocol_generation = 0;
    uint64_t reservation = 0;
    bool start_protocol = false;
};
struct HeartbeatContext {
    Application* app;
    std::string url;
    std::string device_secret;
    std::string body;
};


static constexpr int kWakeWordAudioChannelOpenMaxAttempts = 3;
static constexpr uint32_t kWakeWordAudioChannelRetryDelayMs = 700;
static constexpr uint64_t kConnectWatchdogTimeoutUs = 35ULL * 1000000ULL;
static constexpr uint32_t kMaxAudioPacketsPerMainLoop = 4;
static constexpr uint32_t kOpenChannelWorkerStackDepth = 8192;
static constexpr uint32_t kChatOutboundWorkerStackDepth = 8192;
static constexpr uint32_t kChatAudioCleanupWorkerStackDepth = 8192;
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
static constexpr UBaseType_t kLessonMessageQueueDepth = kLessonMessageDataQueueDepth;
static constexpr uint32_t kLessonMessageWorkerStackDepth = 32768;
static constexpr uint32_t kLessonMessageWorkerMinimumFreeStackBytes = 4096;
#endif

enum class NetworkWorkKind : uint8_t {
    kOpenChannel,
    kHeartbeat,
    kProtocolCleanup,
};

struct NetworkWorkItem {
    NetworkWorkKind kind;
    void* context;
};

extern StaticTask_t open_channel_task_buffer;
extern DRAM_ATTR StackType_t open_channel_task_stack[kOpenChannelWorkerStackDepth];
extern StaticQueue_t open_channel_queue_buffer;
extern NetworkWorkItem open_channel_queue_storage[2];
extern QueueHandle_t open_channel_queue;
extern TaskHandle_t open_channel_task;
extern StaticTask_t chat_outbound_task_buffer;
extern EXT_RAM_BSS_ATTR StackType_t chat_outbound_task_stack[kChatOutboundWorkerStackDepth];
extern StaticTask_t chat_audio_cleanup_task_buffer;
extern EXT_RAM_BSS_ATTR StackType_t chat_audio_cleanup_task_stack[kChatAudioCleanupWorkerStackDepth];

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
extern StaticTask_t lesson_message_task_buffer;
extern StaticQueue_t lesson_message_queue_buffer;
extern DRAM_ATTR StackType_t lesson_message_task_stack[kLessonMessageWorkerStackDepth];
extern uint8_t* lesson_message_queue_storage;

void LogLessonWorkerStackWatermark(const char* stage);
#endif

static constexpr int kOtaCheckMaxAttempts = 3;
static constexpr int kOtaRetryDelaysSeconds[] = {2, 4};
static constexpr int kOtaCheckPhaseBudgetMs =
    kOtaCheckMaxAttempts * Ota::kHttpTimeoutMs +
    (kOtaRetryDelaysSeconds[0] + kOtaRetryDelaysSeconds[1]) * 1000;
static_assert(kOtaCheckPhaseBudgetMs <= 60000,
              "OTA activation check must remain inside the boot phase budget");

static constexpr uint64_t kClaimPollIntervalUs = 10ULL * 1000000ULL;
static constexpr uint64_t kClaimPollIntervalIdleUs = 60ULL * 1000000ULL;
static constexpr int64_t kClaimPollWindowMs = 5LL * 60LL * 1000LL;
static constexpr int64_t kClaimVisibilityRetryWindowMs = 20LL * 1000LL;
static constexpr uint64_t kHeartbeatIntervalUs = 20ULL * 1000000ULL;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
static const char* ConnectStateScreenCopy(const TbotConnectStateSpec* spec) {
    switch (spec->state) {
        case TbotConnectState::BACKEND_CONNECTING:
            return Lang::Strings::CONNECTING;
        case TbotConnectState::ONLINE:
            return Lang::Strings::CONNECTED;
        case TbotConnectState::OFFLINE_RETRY:
            return Lang::Strings::SERVER_UNAVAILABLE_RETRYING;
        case TbotConnectState::OTA_UPDATING:
            return Lang::Strings::UPGRADING;
        case TbotConnectState::CLAIM_AVAILABLE:
            return Lang::Strings::READY_TO_CONNECT;
        case TbotConnectState::CLAIM_CONFIRM_TIMEOUT:
            return Lang::Strings::SETUP_EXPIRED;
        case TbotConnectState::BLE_SETUP_ADVERTISING:
            return Lang::Strings::SEARCHING_FOR_DEVICE;
        default:
            return spec->screen_text;
    }
}
#pragma GCC diagnostic pop

#endif // APPLICATION_INTERNAL_H_
