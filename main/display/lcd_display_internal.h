#ifndef LCD_DISPLAY_INTERNAL_H
#define LCD_DISPLAY_INTERNAL_H

#include "lcd_display.h"
#include "application.h"
#include "chat_runtime_timing.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "lesson_layer_state.h"
#include "lesson_renderer_memory_probe.h"
#include "lesson_tvideo_template.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <cstring>
#include <new>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
constexpr int kLessonObjectMaxWidthPercent = 46;
constexpr int kLessonObjectMaxHeightPercent = 50;
constexpr int kLessonObjectYOffsetDivisor = 12;
constexpr int kLessonRobotMaxWidthPercent = 32;
constexpr int kLessonRobotMaxHeightPercent = 34;
constexpr int kLessonRobotBottomInsetDivisor = 6;
constexpr int kLessonCaptionWidthPercent = 92;
constexpr int kLessonCaptionLabelWidthPercent = 86;
constexpr int kLessonCaptionMaxHeightPercent = 24;
constexpr int kLessonCaptionBottomInsetDivisor = 60;
constexpr int kLessonFocusReferenceWidth = 480;
constexpr int kLessonFocusReferenceHeight = 320;
constexpr int kLessonFocusCueSize = 64;
constexpr uint32_t kLessonRobotAnimationTickMs = 16;
constexpr uint32_t kLessonRobotAnimationTimeoutMs = 6000;
constexpr size_t kLessonRobotAnimationMinInternalHeapBytes = 24 * 1024;
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
constexpr uint32_t kLessonRendererSettledObservationMs = 500;
#endif

struct LessonRobotAnimationContext {
    LcdDisplay* owner;
    lesson_tvideo::StateMachine machine;
    LessonVisualCompletion completion;
    lv_timer_t* timer = nullptr;
    uint32_t elapsed_ms = 0;
};

inline void ReplaceTrackedLessonLayer(std::unique_ptr<LvglImage>* current,
                                     std::unique_ptr<LvglImage> next) {
    const bool had_layer = *current != nullptr;
    const bool has_layer = next != nullptr;
    if (!had_layer && has_layer) LessonRendererMemoryDecodedLayerOpened();
    if (had_layer && !has_layer) LessonRendererMemoryDecodedLayerClosed();
    *current = std::move(next);
}

inline int LessonImageFitScale(int image_width, int image_height, int max_width, int max_height) {
    if (image_width <= 0 || image_height <= 0 || max_width <= 0 || max_height <= 0) {
        return 256;
    }
    const int scale_w = 256 * max_width / image_width;
    const int scale_h = 256 * max_height / image_height;
    return std::max(1, std::min(scale_w, scale_h));
}

inline int LessonImageCoverScale(int image_width, int image_height, int target_width, int target_height) {
    if (image_width <= 0 || image_height <= 0 || target_width <= 0 || target_height <= 0) {
        return 256;
    }
    const int scale_w = 256 * target_width / image_width;
    const int scale_h = 256 * target_height / image_height;
    return std::max(1, std::max(scale_w, scale_h));
}

inline lv_obj_t* CreateLessonFocusCue(lv_obj_t* screen) {
    lv_obj_t* cue = lv_obj_create(screen);
    lv_obj_set_size(cue, kLessonFocusCueSize, kLessonFocusCueSize);
    lv_obj_set_style_radius(cue, kLessonFocusCueSize / 2, 0);
    lv_obj_set_style_bg_opa(cue, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cue, 4, 0);
    lv_obj_set_style_border_color(cue, lv_color_hex(0xFFD43B), 0);
    lv_obj_set_style_shadow_width(cue, 10, 0);
    lv_obj_set_style_shadow_color(cue, lv_color_hex(0xFFF2A8), 0);
    lv_obj_set_style_shadow_opa(cue, LV_OPA_50, 0);
    lv_obj_clear_flag(cue, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cue, LV_OBJ_FLAG_HIDDEN);
    return cue;
}
#endif

#endif // LCD_DISPLAY_INTERNAL_H
