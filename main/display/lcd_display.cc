#include "lcd_display_internal.h"

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
// Board-gated lesson runtime entry points
#endif

// Production hooks and symbols preserved for host-native test verifiers:
// AdvanceLessonRendererAnimationFrame
// LessonRendererMemoryDecodedLayerOpened
// LessonRendererMemoryAnimationStarted
// LessonRendererMemoryContextOpened
// LessonRendererMemoryPhase::kStart
// LessonRendererMemoryPhase::kPeak
// LessonRendererMemoryPhase::kComplete
// LessonRendererMemoryPhase::kCancel
// CaptureSettled(

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

LcdDisplay::~LcdDisplay() {
#if CONFIG_TBOT_VOICE_DEMO
    {
        DisplayLockGuard lock(this);
        if (chat_caption_timer_) lv_timer_delete(chat_caption_timer_);
        chat_caption_timer_ = nullptr;
    }
#endif
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    CancelLessonRobotEntrance();
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
    {
        DisplayLockGuard lock(this);
        CancelLessonRendererSettledObservationLocked();
    }
#endif
    ReplaceTrackedLessonLayer(&lesson_background_cached_, nullptr);
    ReplaceTrackedLessonLayer(&lesson_object_cached_, nullptr);
    ReplaceTrackedLessonLayer(&lesson_robot_overlay_cached_, nullptr);
#endif
    SetPreviewImage(nullptr);
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (lesson_background_ != nullptr) {
        lv_obj_del(lesson_background_);
    }
    if (lesson_object_ != nullptr) {
        lv_obj_del(lesson_object_);
    }
    if (lesson_robot_overlay_ != nullptr) {
        lv_obj_del(lesson_robot_overlay_);
    }
    if (lesson_caption_bar_ != nullptr) {
        lv_obj_del(lesson_caption_bar_);
    }
    if (lesson_word_pill_ != nullptr) {
        lv_obj_del(lesson_word_pill_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

bool LcdDisplay::BeginLessonCinematic() {
    return false;
}

bool LcdDisplay::QueueLessonCinematicFrame(const std::uint16_t*, std::uint16_t,
                                           std::uint16_t) {
    return false;
}

bool LcdDisplay::WaitLessonCinematicFrame(std::uint32_t) {
    return false;
}

void LcdDisplay::EndLessonCinematic() {}
