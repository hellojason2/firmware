#include "lesson_cinematic_renderer.h"
#include "lesson_storage_hil_u64_format.h"
#include "lesson_flattened_cinematic_renderer.h"
#include "lesson_layered_cinematic_renderer.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>

#ifdef ESP_PLATFORM
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "lesson_mjpeg_mp4.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace tbot {
namespace {

std::atomic<bool> g_capability_ready{false};
std::atomic<LessonCinematicRenderer*> g_active_renderer{nullptr};
std::atomic<std::uint64_t> g_completed_phase_sequence{0};
std::mutex g_active_renderer_mutex;

void AppendFingerprintString(std::string& destination, const char* value) {
    const std::string_view text = value != nullptr ? std::string_view(value) : std::string_view();
    destination += std::to_string(text.size());
    destination += ":";
    destination.append(text.data(), text.size());
    destination += ";";
}

std::string ControlFingerprint(const char* command, const char* phase_id) {
    std::string value;
    AppendFingerprintString(value, command);
    AppendFingerprintString(value, phase_id);
    return value;
}

std::string PrepareFingerprint(const LessonCinematicPhaseConfig& config) {
    std::string value = ControlFingerprint("prepare", config.phase_id);
    AppendFingerprintString(value, config.renderer_id);
    AppendFingerprintString(value, config.template_id);
    for (const auto& layer : config.layers) {
        AppendFingerprintString(value, layer.sd_path);
        for (const auto number : {static_cast<std::int64_t>(layer.rect.x),
                                  static_cast<std::int64_t>(layer.rect.y),
                                  static_cast<std::int64_t>(layer.rect.width),
                                  static_cast<std::int64_t>(layer.rect.height),
                                  static_cast<std::int64_t>(layer.chroma.color.red),
                                  static_cast<std::int64_t>(layer.chroma.color.green),
                                  static_cast<std::int64_t>(layer.chroma.color.blue),
                                  static_cast<std::int64_t>(layer.chroma.tolerance),
                                  static_cast<std::int64_t>(layer.chroma.feather)}) {
            value += ":" + std::to_string(number);
        }
    }
    return value;
}

bool LocalPath(const char* path) {
    return path != nullptr && path[0] == '/' && std::strstr(path, "://") == nullptr;
}

bool SameTiming(const LessonCinematicStreamMetadata& left,
                const LessonCinematicStreamMetadata& right) {
    return left.fps == right.fps && left.frame_count == right.frame_count &&
           left.duration_ms == right.duration_ms;
}

bool ValidRect(const LessonCinematicRect& rect) {
    if (rect.width == 0 || rect.height == 0 || rect.width > 240 || rect.height > 240) return false;
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
    return right > 0 && bottom > 0 && rect.x < kLessonCinematicWidth &&
           rect.y < kLessonCinematicHeight;
}

}  // namespace

bool LessonCinematicRendererCapabilityReady() {
    return g_capability_ready.load(std::memory_order_acquire);
}

void SetLessonCinematicRendererCapabilityReady(bool ready) {
    g_capability_ready.store(ready, std::memory_order_release);
}

void SetActiveLessonCinematicRenderer(LessonCinematicRenderer* renderer) {
    std::lock_guard<std::mutex> lock(g_active_renderer_mutex);
    g_active_renderer.store(renderer, std::memory_order_release);
    if (renderer != nullptr) g_completed_phase_sequence.store(0, std::memory_order_release);
    SetLessonCinematicRendererCapabilityReady(renderer != nullptr && renderer->initialized());
}

LessonCinematicResponse TickActiveLessonCinematicRenderer(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(g_active_renderer_mutex);
    LessonCinematicRenderer* renderer = g_active_renderer.load(std::memory_order_acquire);
    if (renderer == nullptr) {
        return {LessonCinematicResponseType::kFailure, false, 0, "",
                LessonCinematicError::kInvalidState};
    }
    const LessonCinematicResponse response = renderer->Tick(now_ms);
    if (response.type == LessonCinematicResponseType::kPhaseComplete && response.accepted) {
        g_completed_phase_sequence.store(response.command_sequence_id, std::memory_order_release);
    }
    return response;
}

std::uint64_t LessonCinematicCompletedSequence() {
    return g_completed_phase_sequence.load(std::memory_order_acquire);
}

LessonCinematicRenderer* ActiveLessonCinematicRenderer() {
    return g_active_renderer.load(std::memory_order_acquire);
}

#ifdef ESP_PLATFORM
namespace {

struct ProductionRendererContext {
    ::LcdDisplay* display = nullptr;
    std::string assignment_id;
    std::string session_id;
    std::uint64_t generation = 0;
    std::array<LessonMjpegMp4File, 3> files;
    jpeg_reusable_decoder_t decoder{};
    std::uint8_t* jpeg_input = nullptr;
    std::size_t jpeg_input_capacity = kLessonMjpegMp4MaxSampleBytes;
    esp_timer_handle_t frame_timer = nullptr;
    TaskHandle_t frame_task = nullptr;
    SemaphoreHandle_t frame_task_stopped = nullptr;
    std::atomic<bool> stop_frame_task{false};
    LessonCinematicError last_error = LessonCinematicError::kNone;
};

std::unique_ptr<ProductionRendererContext> g_production_context;
std::unique_ptr<LessonCinematicRenderer> g_production_renderer;

constexpr uint32_t kProductionRendererStackDepth = 32 * 1024;
DRAM_ATTR StaticTask_t g_production_renderer_task_buffer;
DRAM_ATTR StackType_t g_production_renderer_task_stack[kProductionRendererStackDepth];

void* ProductionAllocate(void*, std::size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ProductionFree(void*, void* pointer) {
    heap_caps_free(pointer);
}

bool ProductionOpen(void* raw, const char* path, LessonCinematicStreamMetadata* metadata,
                    void** handle) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    if (context == nullptr || context->generation == 0 || metadata == nullptr || handle == nullptr) {
        return false;
    }
    context->last_error = LessonCinematicError::kNone;
    if (context->jpeg_input == nullptr) {
        context->jpeg_input = static_cast<std::uint8_t*>(heap_caps_malloc(
            context->jpeg_input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        // The ROM decoder repeatedly accesses this 3.1 KB workspace during every MCU row.
        if (context->jpeg_input == nullptr ||
            jpeg_reusable_decoder_prepare_workspace(
                &context->decoder, kLessonCinematicWidth, kLessonCinematicHeight,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != ESP_OK) {
            if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
            context->jpeg_input = nullptr;
            jpeg_reusable_decoder_destroy(&context->decoder);
            context->last_error = LessonCinematicError::kInsufficientPsram;
            return false;
        }
    }
    LessonMjpegMp4File* file = nullptr;
    for (auto& candidate : context->files) {
        if (!candidate.is_open()) {
            file = &candidate;
            break;
        }
    }
    if (file == nullptr || file->OpenUnderLessonSession(
            path, context->assignment_id, context->session_id, context->generation) !=
            LessonMjpegMp4Status::kOk) {
        context->last_error = LessonCinematicError::kFileOpen;
        return false;
    }
    const LessonMjpegMp4Metadata parsed = file->metadata();
    if (parsed.timescale == 0 || parsed.fps_milli % 1000 != 0 ||
        parsed.duration_ticks > UINT64_MAX / 1000) {
        file->Close();
        context->last_error = LessonCinematicError::kMetadataMismatch;
        return false;
    }
    *metadata = {parsed.width, parsed.height,
                 static_cast<std::uint16_t>(parsed.fps_milli / 1000),
                 static_cast<std::uint32_t>(parsed.frame_count),
                 static_cast<std::uint32_t>(parsed.duration_ticks * 1000 / parsed.timescale),
                 kLessonMjpegMp4MaxSampleBytes};
    *handle = file;
    return true;
}

void ProductionClose(void* raw, void* handle) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    if (handle != nullptr) static_cast<LessonMjpegMp4File*>(handle)->Close();
    bool any_open = false;
    for (const auto& file : context->files) any_open = any_open || file.is_open();
    if (!any_open) {
        jpeg_reusable_decoder_destroy(&context->decoder);
        if (context->jpeg_input != nullptr) heap_caps_free(context->jpeg_input);
        context->jpeg_input = nullptr;
    }
}

bool ProductionDecode(void* raw, void* handle, std::size_t index,
                      std::uint8_t* destination, std::size_t capacity,
                      std::uint16_t* width, std::uint16_t* height, std::size_t* stride) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    if (context != nullptr) context->last_error = LessonCinematicError::kNone;
    std::size_t jpeg_size = 0;
    if (context == nullptr || handle == nullptr ||
        static_cast<LessonMjpegMp4File*>(handle)->ReadFrame(
            index, context->jpeg_input, context->jpeg_input_capacity, &jpeg_size) !=
            LessonMjpegMp4Status::kOk) {
        return false;
    }
    std::size_t decoded_size = 0;
    std::size_t decoded_width = 0;
    std::size_t decoded_height = 0;
    if (jpeg_reusable_decoder_decode_into(
            &context->decoder, context->jpeg_input, jpeg_size, destination, capacity,
            &decoded_size, &decoded_width, &decoded_height, stride) != ESP_OK ||
        decoded_width > UINT16_MAX || decoded_height > UINT16_MAX) {
        context->last_error = LessonCinematicError::kDecodeFailed;
        return false;
    }
    *width = static_cast<std::uint16_t>(decoded_width);
    *height = static_cast<std::uint16_t>(decoded_height);
    return decoded_size == decoded_height * *stride;
}

LessonCinematicError ProductionLastError(void* raw) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    return context != nullptr ? context->last_error : LessonCinematicError::kNone;
}

std::uint64_t ProductionMonotonicMs(void*) {
    return static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
}

bool ProductionPresent(void* raw, const std::uint16_t* pixels, std::uint16_t width,
                       std::uint16_t height, std::size_t) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    return context != nullptr && context->display != nullptr &&
           context->display->PresentLessonFramebuffer(pixels, width, height);
}

void ProductionRendererTimerCallback(void* raw) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    if (context != nullptr && context->frame_task != nullptr &&
        !context->stop_frame_task.load(std::memory_order_acquire)) {
        xTaskNotifyGive(context->frame_task);
    }
}

void ProductionRendererTask(void* raw) {
    auto* context = static_cast<ProductionRendererContext*>(raw);
    while (context != nullptr) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (context->stop_frame_task.load(std::memory_order_acquire)) break;

        const std::uint64_t now_ms =
            static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
        const auto response = LessonCinematicTimerRoutesV5()
            ? TickActiveLessonLayeredCinematicRenderer(now_ms)
            : LessonCinematicTimerRoutesV4()
                ? TickActiveLessonFlattenedCinematicRenderer(now_ms)
                : TickActiveLessonCinematicRenderer(now_ms);
        if (response.type == LessonCinematicResponseType::kPhaseComplete) {
            ESP_LOGI("LessonCinematic",
                     "phase complete at command sequence %s stack_min=%u",
                     FormatLessonStorageHilUint64(response.command_sequence_id).c_str(),
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        } else if (!response.accepted &&
                   response.error != LessonCinematicError::kInvalidState) {
            ESP_LOGW("LessonCinematic", "renderer tick failed: %u",
                     static_cast<unsigned>(response.error));
        }
    }

    if (context != nullptr && context->frame_task_stopped != nullptr) {
        xSemaphoreGive(context->frame_task_stopped);
    }
    // Static TCB/stack: park here and let Shutdown delete us from another task,
    // so the buffers are fully released before any re-create.
    vTaskSuspend(nullptr);
}

}  // namespace
#endif

bool InitializeProductionLessonCinematicRenderer(::LcdDisplay* display) {
#ifdef ESP_PLATFORM
    constexpr std::size_t kRequiredPsram =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2 +
        static_cast<std::size_t>(240) * 240 * 2 + kLessonMjpegMp4MaxSampleBytes + 128 * 1024;
    if (display == nullptr || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kRequiredPsram) {
        return false;
    }
    void* framebuffer_probe = heap_caps_malloc(
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* foreground_probe = heap_caps_malloc(static_cast<std::size_t>(240) * 240 * 2,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* jpeg_probe = heap_caps_malloc(kLessonMjpegMp4MaxSampleBytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_reusable_decoder_t decoder_probe{};
    const bool decoder_ready = jpeg_reusable_decoder_prepare_workspace(
        &decoder_probe, kLessonCinematicWidth, kLessonCinematicHeight,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == ESP_OK;
    const bool buffers_ready = framebuffer_probe != nullptr && foreground_probe != nullptr &&
                               jpeg_probe != nullptr && decoder_ready;
    if (framebuffer_probe != nullptr) heap_caps_free(framebuffer_probe);
    if (foreground_probe != nullptr) heap_caps_free(foreground_probe);
    if (jpeg_probe != nullptr) heap_caps_free(jpeg_probe);
    jpeg_reusable_decoder_destroy(&decoder_probe);
    if (!buffers_ready) return false;
    g_production_context = std::make_unique<ProductionRendererContext>();
    g_production_context->display = display;
    g_production_renderer = std::make_unique<LessonCinematicRenderer>(LessonCinematicRendererOps{
        g_production_context.get(), ProductionAllocate, ProductionFree, ProductionOpen,
        ProductionClose, ProductionDecode, ProductionPresent, ProductionLastError,
        ProductionMonotonicMs});
    g_production_context->frame_task_stopped = xSemaphoreCreateBinary();
    if (g_production_context->frame_task_stopped == nullptr ||
        (g_production_context->frame_task = xTaskCreateStatic(
             ProductionRendererTask, "lesson_cinematic", kProductionRendererStackDepth,
             g_production_context.get(), tskIDLE_PRIORITY + 2,
             g_production_renderer_task_stack, &g_production_renderer_task_buffer)) == nullptr) {
        ShutdownProductionLessonCinematicRenderer();
        return false;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = ProductionRendererTimerCallback,
        .arg = g_production_context.get(),
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lesson_cinematic",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &g_production_context->frame_timer) != ESP_OK ||
        esp_timer_start_periodic(g_production_context->frame_timer, 10000) != ESP_OK) {
        ShutdownProductionLessonCinematicRenderer();
        return false;
    }
    SetActiveLessonCinematicRenderer(g_production_renderer.get());
    InitializeProductionLessonFlattenedCinematicRenderer(display);
    InitializeProductionLessonLayeredCinematicRenderer();
    return LessonCinematicRendererCapabilityReady();
#else
    (void)display;
    return false;
#endif
}

LessonCinematicRendererOps ProductionLessonCinematicRendererOps() {
#ifdef ESP_PLATFORM
    if (g_production_context == nullptr) return {};
    return {g_production_context.get(), ProductionAllocate, ProductionFree, ProductionOpen,
            ProductionClose, ProductionDecode, ProductionPresent, ProductionLastError,
            ProductionMonotonicMs};
#else
    return {};
#endif
}

void ConfigureProductionLessonCinematicSession(const std::string& assignment_id,
                                                const std::string& session_id,
                                                std::uint64_t generation) {
#ifdef ESP_PLATFORM
    if (g_production_context != nullptr) {
        g_production_context->assignment_id = assignment_id;
        g_production_context->session_id = session_id;
        g_production_context->generation = generation;
    }
#else
    (void)assignment_id;
    (void)session_id;
    (void)generation;
#endif
}

void ShutdownProductionLessonCinematicRenderer() {
#ifdef ESP_PLATFORM
    if (g_production_context != nullptr && g_production_context->frame_timer != nullptr) {
        esp_timer_stop(g_production_context->frame_timer);
        esp_timer_delete(g_production_context->frame_timer);
        g_production_context->frame_timer = nullptr;
    }
    if (g_production_context != nullptr && g_production_context->frame_task != nullptr) {
        g_production_context->stop_frame_task.store(true, std::memory_order_release);
        xTaskNotifyGive(g_production_context->frame_task);
        if (g_production_context->frame_task_stopped == nullptr ||
            xSemaphoreTake(g_production_context->frame_task_stopped,
                           pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE("LessonCinematic", "renderer task stop timed out");
        } else {
            while (eTaskGetState(g_production_context->frame_task) != eSuspended) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        vTaskDelete(g_production_context->frame_task);
        g_production_context->frame_task = nullptr;
    }
    if (g_production_context != nullptr &&
        g_production_context->frame_task_stopped != nullptr) {
        vSemaphoreDelete(g_production_context->frame_task_stopped);
        g_production_context->frame_task_stopped = nullptr;
    }
    SetActiveLessonCinematicRenderer(nullptr);
    ShutdownProductionLessonFlattenedCinematicRenderer();
    ShutdownProductionLessonLayeredCinematicRenderer();
    g_production_renderer.reset();
    g_production_context.reset();
#endif
}

LessonCinematicRenderer::LessonCinematicRenderer(LessonCinematicRendererOps ops) : ops_(ops) {}

LessonCinematicRenderer::~LessonCinematicRenderer() {
    Reset();
}

bool LessonCinematicRenderer::initialized() const {
    return ops_.allocate != nullptr && ops_.free != nullptr && ops_.open != nullptr &&
           ops_.close != nullptr && ops_.decode != nullptr && ops_.present != nullptr;
}

bool LessonCinematicRenderer::prepared() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kPrepared || state_ == State::kRunning || state_ == State::kPaused;
}

void LessonCinematicRenderer::DiscardSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    Reset();
}

LessonCinematicResponse LessonCinematicRenderer::Failure(
    std::uint64_t sequence, LessonCinematicError error) const {
    return {LessonCinematicResponseType::kFailure, false, sequence, phase_id_, error};
}

LessonCinematicResponse LessonCinematicRenderer::Applied(
    LessonCinematicResponseType type, std::uint64_t sequence) const {
    return {type, true, sequence, phase_id_, LessonCinematicError::kNone};
}

LessonCinematicError LessonCinematicRenderer::OperationError(
    LessonCinematicError fallback) const {
    if (ops_.last_error == nullptr) return fallback;
    const LessonCinematicError error = ops_.last_error(ops_.context);
    return error == LessonCinematicError::kNone ? fallback : error;
}

LessonCinematicResponse LessonCinematicRenderer::Prepare(
    const LessonCinematicPhaseConfig& config, std::uint64_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string fingerprint = PrepareFingerprint(config);
    if (config.command_sequence_id == last_sequence_ && last_response_.accepted) {
        if (last_command_ == "prepare" && last_fingerprint_ == fingerprint) return last_response_;
        return Failure(config.command_sequence_id, LessonCinematicError::kStaleCommand);
    }
    if (!initialized() || config.renderer_id == nullptr || config.template_id == nullptr ||
        std::strcmp(config.renderer_id, kLessonRendererV3) != 0 ||
        std::strcmp(config.template_id, kLessonDirectMp4CinematicTemplate) != 0) {
        return Failure(config.command_sequence_id, LessonCinematicError::kUnsupportedContract);
    }
    if (config.phase_id == nullptr || config.phase_id[0] == '\0') {
        return Failure(config.command_sequence_id, LessonCinematicError::kInvalidPhase);
    }
    for (const auto& layer : config.layers) {
        if (!LocalPath(layer.sd_path)) {
            return Failure(config.command_sequence_id, LessonCinematicError::kInvalidPath);
        }
    }
    if (!ValidRect(config.layers[1].rect) || !ValidRect(config.layers[2].rect)) {
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }

    Reset();
    phase_id_ = config.phase_id;
    layers_ = config.layers;
    constexpr std::size_t kFramebufferBytes =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    foreground_capacity_ = static_cast<std::size_t>(240) * 240 * 2;
    framebuffer_ = static_cast<std::uint16_t*>(ops_.allocate(ops_.context, kFramebufferBytes));
    foreground_scratch_ = static_cast<std::uint8_t*>(
        ops_.allocate(ops_.context, foreground_capacity_));
    if (framebuffer_ == nullptr || foreground_scratch_ == nullptr) {
        Reset();
        phase_id_ = config.phase_id;
        return Failure(config.command_sequence_id, LessonCinematicError::kInsufficientPsram);
    }

    for (std::size_t index = 0; index < streams_.size(); ++index) {
        if (!ops_.open(ops_.context, config.layers[index].sd_path, &metadata_[index],
                       &streams_[index])) {
            CloseStreams();
            ReleaseBuffers();
            return Failure(config.command_sequence_id,
                           OperationError(LessonCinematicError::kFileOpen));
        }
    }
    const bool metadata_ok = metadata_[0].width == kLessonCinematicWidth &&
        metadata_[0].height == kLessonCinematicHeight &&
        metadata_[0].fps != 0 && (metadata_[0].fps == 10 || metadata_[0].fps == 15) &&
        metadata_[0].frame_count != 0 && metadata_[0].duration_ms != 0 &&
        metadata_[1].width <= 240 && metadata_[1].height <= 240 &&
        metadata_[2].width <= 240 && metadata_[2].height <= 240 &&
        SameTiming(metadata_[0], metadata_[1]) && SameTiming(metadata_[0], metadata_[2]);
    if (!metadata_ok) {
        CloseStreams();
        ReleaseBuffers();
        return Failure(config.command_sequence_id, LessonCinematicError::kMetadataMismatch);
    }
    // AC:20 measured the cold background frame-zero decode at 124ms. Prepare is
    // outside the playback clock, so allow bounded cold-start headroom.
    constexpr std::uint64_t kPrepareDecodeDeadlineMs = 150;
    const LessonCinematicError frame_zero_error = RenderFrame(
        0, kPrepareDecodeDeadlineMs, kPrepareDecodeDeadlineMs);
    if (frame_zero_error != LessonCinematicError::kNone) {
        CloseStreams();
        ReleaseBuffers();
        state_ = State::kFailed;
        return Failure(config.command_sequence_id, frame_zero_error);
    }
    state_ = State::kPrepared;
    displayed_frame_ = 0;
    last_sequence_ = config.command_sequence_id;
    last_response_ = Applied(LessonCinematicResponseType::kFrameZeroReady, last_sequence_);
    last_command_ = "prepare";
    last_fingerprint_ = fingerprint;
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::ValidateControl(
    std::uint64_t sequence, const char* phase_id, const char* command) const {
    if (sequence == last_sequence_ && last_response_.accepted) {
        if (last_command_ == command && last_fingerprint_ == ControlFingerprint(command, phase_id)) {
            return last_response_;
        }
        return Failure(sequence, LessonCinematicError::kStaleCommand);
    }
    if (sequence < last_sequence_ || phase_id == nullptr || phase_id_ != phase_id) {
        return Failure(sequence, LessonCinematicError::kStaleCommand);
    }
    return {LessonCinematicResponseType::kCommandApplied, true, sequence, phase_id_,
            LessonCinematicError::kNone};
}

LessonCinematicResponse LessonCinematicRenderer::Start(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "start");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPrepared) return Failure(sequence, LessonCinematicError::kInvalidState);
    state_ = State::kRunning;
    clock_origin_ms_ = now_ms;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kPhaseReady, sequence);
    last_command_ = "start";
    last_fingerprint_ = ControlFingerprint("start", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::Pause(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "pause");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kRunning) return Failure(sequence, LessonCinematicError::kInvalidState);
    state_ = State::kPaused;
    paused_at_ms_ = now_ms;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "pause";
    last_fingerprint_ = ControlFingerprint("pause", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::Resume(
    std::uint64_t sequence, const char* phase_id, std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "resume");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    if (state_ != State::kPaused) return Failure(sequence, LessonCinematicError::kInvalidState);
    clock_origin_ms_ += now_ms - paused_at_ms_;
    state_ = State::kRunning;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "resume";
    last_fingerprint_ = ControlFingerprint("resume", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::Stop(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "stop");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    CloseStreams();
    ReleaseBuffers();
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "stop";
    last_fingerprint_ = ControlFingerprint("stop", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::Cancel(
    std::uint64_t sequence, const char* phase_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto valid = ValidateControl(sequence, phase_id, "cancel");
    if (!valid.accepted || sequence == last_sequence_) return valid;
    CloseStreams();
    ReleaseBuffers();
    state_ = State::kIdle;
    last_sequence_ = sequence;
    last_response_ = Applied(LessonCinematicResponseType::kCommandApplied, sequence);
    last_command_ = "cancel";
    last_fingerprint_ = ControlFingerprint("cancel", phase_id);
    return last_response_;
}

LessonCinematicResponse LessonCinematicRenderer::Tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kPaused) return Applied(LessonCinematicResponseType::kCommandApplied,
                                                 last_sequence_);
    if (state_ != State::kRunning) return Failure(last_sequence_, LessonCinematicError::kInvalidState);
    const std::uint64_t elapsed = now_ms >= clock_origin_ms_ ? now_ms - clock_origin_ms_ : 0;
    std::uint64_t frame = elapsed * metadata_[0].fps / 1000;
    const bool complete = frame >= metadata_[0].frame_count;
    if (complete) frame = metadata_[0].frame_count - 1;
    if (frame == displayed_frame_) {
        if (complete) {
            state_ = State::kPrepared;
            return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
        }
        return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
    }
    constexpr std::uint64_t kPlaybackBackgroundDecodeDeadlineMs = 150;
    constexpr std::uint64_t kPlaybackForegroundDecodeDeadlineMs = 100;
    const LessonCinematicError render_error = RenderFrame(
        static_cast<std::size_t>(frame), kPlaybackBackgroundDecodeDeadlineMs,
        kPlaybackForegroundDecodeDeadlineMs);
    if (render_error != LessonCinematicError::kNone) {
        CloseStreams();
        ReleaseBuffers();
        state_ = State::kFailed;
        return Failure(last_sequence_, render_error);
    }
    displayed_frame_ = static_cast<std::size_t>(frame);
    if (complete) {
        state_ = State::kPrepared;
        return Applied(LessonCinematicResponseType::kPhaseComplete, last_sequence_);
    }
    return Applied(LessonCinematicResponseType::kCommandApplied, last_sequence_);
}

LessonCinematicError LessonCinematicRenderer::RenderFrame(
    std::size_t frame_index, std::uint64_t background_decode_deadline_ms,
    std::uint64_t foreground_decode_deadline_ms) {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::size_t stride = 0;
    constexpr std::size_t kBackgroundCapacity =
        static_cast<std::size_t>(kLessonCinematicWidth) * kLessonCinematicHeight * 2;
    auto decode = [&]([[maybe_unused]] std::size_t layer_index, void* stream,
                      std::uint8_t* destination,
                      std::size_t capacity) {
        const std::uint64_t decode_deadline_ms = layer_index == 0
            ? background_decode_deadline_ms : foreground_decode_deadline_ms;
        const std::uint64_t started = ops_.monotonic_ms != nullptr
            ? ops_.monotonic_ms(ops_.context) : 0;
        const bool decoded = ops_.decode(ops_.context, stream, frame_index, destination,
                                         capacity, &width, &height, &stride);
        const LessonCinematicError operation_error = decoded
            ? LessonCinematicError::kNone
            : OperationError(LessonCinematicError::kDecodeFailed);
        if (ops_.monotonic_ms != nullptr) {
            const std::uint64_t finished = ops_.monotonic_ms(ops_.context);
#ifdef ESP_PLATFORM
            ESP_LOGI("LessonCinematic",
                     "decode layer=%u frame=%u elapsed_ms=%s"
                     " deadline_ms=%s decoded=%d operation_error=%u",
                     static_cast<unsigned>(layer_index), static_cast<unsigned>(frame_index),
                     FormatLessonStorageHilUint64(finished >= started ? finished - started : 0).c_str(),
                     FormatLessonStorageHilUint64(decode_deadline_ms).c_str(),
                     decoded ? 1 : 0, static_cast<unsigned>(operation_error));
#endif
            if (!decoded) return operation_error;
            if (finished >= started && finished - started > decode_deadline_ms) {
                return LessonCinematicError::kDecodeTimeout;
            }
        }
        if (!decoded) return operation_error;
        return LessonCinematicError::kNone;
    };
    const LessonCinematicError background_error = decode(
        0, streams_[0], reinterpret_cast<std::uint8_t*>(framebuffer_), kBackgroundCapacity);
    if (background_error != LessonCinematicError::kNone ||
        width != kLessonCinematicWidth || height != kLessonCinematicHeight ||
        stride != static_cast<std::size_t>(width) * 2) {
        return background_error != LessonCinematicError::kNone
            ? background_error : LessonCinematicError::kDecodeFailed;
    }
    for (std::size_t index = 1; index < streams_.size(); ++index) {
        const LessonCinematicError foreground_error = decode(
            index, streams_[index], foreground_scratch_, foreground_capacity_);
        if (foreground_error != LessonCinematicError::kNone ||
            width == 0 || height == 0 || width > 240 || height > 240 ||
            stride != static_cast<std::size_t>(width) * 2 ||
            !LessonCompositeRgb565(
                {reinterpret_cast<const std::uint16_t*>(foreground_scratch_), width, height,
                 stride / 2},
                {framebuffer_, kLessonCinematicWidth, kLessonCinematicHeight,
                 kLessonCinematicWidth}, layers_[index].rect, layers_[index].chroma)) {
            return foreground_error != LessonCinematicError::kNone
                ? foreground_error : LessonCinematicError::kDecodeFailed;
        }
    }
    return ops_.present(ops_.context, framebuffer_, kLessonCinematicWidth,
                        kLessonCinematicHeight, frame_index)
        ? LessonCinematicError::kNone : LessonCinematicError::kPresentFailed;
}

void LessonCinematicRenderer::CloseStreams() {
    for (void*& stream : streams_) {
        if (stream != nullptr) {
            ops_.close(ops_.context, stream);
            stream = nullptr;
        }
    }
}

void LessonCinematicRenderer::ReleaseBuffers() {
    if (foreground_scratch_ != nullptr) ops_.free(ops_.context, foreground_scratch_);
    if (framebuffer_ != nullptr) ops_.free(ops_.context, framebuffer_);
    foreground_scratch_ = nullptr;
    framebuffer_ = nullptr;
    foreground_capacity_ = 0;
}

void LessonCinematicRenderer::Reset() {
    CloseStreams();
    ReleaseBuffers();
    state_ = State::kIdle;
    displayed_frame_ = 0;
    clock_origin_ms_ = 0;
    paused_at_ms_ = 0;
}

}  // namespace tbot
