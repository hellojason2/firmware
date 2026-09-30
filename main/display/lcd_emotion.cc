#include "lcd_display_internal.h"

void LcdDisplay::StyleConversationOverlays() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P && !CONFIG_USE_WECHAT_MESSAGE_STYLE
    for (auto* bar : {top_bar_, status_bar_, bottom_bar_}) {
        if (bar) {
            lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
        }
    }
    for (auto* label : {status_label_, notification_label_, chat_message_label_,
                        network_label_, battery_label_, mute_label_}) {
        if (label) lv_obj_set_style_text_color(label, lv_color_white(), 0);
    }
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    if (container_) lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
#endif
}

void LcdDisplay::UpdateConversationFaceLayout() {
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P && !CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (emoji_box_ == nullptr || emoji_image_ == nullptr) return;
    lv_obj_update_layout(lv_screen_active());
    if (lv_obj_get_width(emoji_box_) != width_ ||
        lv_obj_get_height(emoji_box_) != height_) {
        ESP_LOGI(TAG, "Conversation face: %dx%d, transparent bars, white text",
                 width_, height_);
    }
    lv_obj_set_style_radius(emoji_box_, 0, 0);
    lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(emoji_box_, width_, height_);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(emoji_image_, width_, height_);
    lv_obj_center(emoji_image_);
    lv_image_set_inner_align(emoji_image_, LV_IMAGE_ALIGN_COVER);
    lv_obj_center(emoji_label_);
    StyleConversationOverlays();
    if (top_bar_) lv_obj_move_foreground(top_bar_);
    if (status_bar_) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
#endif
}

void LcdDisplay::SetEmotion(const char* emotion) {
    // Construct before either display guard so the timing log follows unlock.
    ChatRuntimeTiming timing(3, []() { return static_cast<uint64_t>(esp_timer_get_time()); },
        [](uint32_t site, uint32_t hi, uint32_t lo) {
            ESP_LOGW(TAG, "chat_slow_scope site=%u elapsed_us_hi=%lu elapsed_us_lo=%lu",
                static_cast<unsigned>(site), static_cast<unsigned long>(hi), static_cast<unsigned long>(lo));
        });
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    // SetLessonMode() already hid and stopped the face once. Conversation-state
    // updates during a lesson must not touch LVGL or resurrect that surface.
    if (lesson_mode_active_) return;

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (lesson_mode_active_) return;
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (lesson_mode_active_) return;
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P && !CONFIG_USE_WECHAT_MESSAGE_STYLE
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc(), width_ == 480 && height_ == 320);
#else
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
#endif
        
        if (gif_controller_->IsLoaded()) {
            // emoji_image_ uses LV_IMAGE_ALIGN_COVER (set in SetupUI), so LVGL
            // auto-scales each frame to fill the panel — no manual scaling here.
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
            auto fallback = emoji_collection != nullptr ? emoji_collection->GetEmojiImage("neutral") : nullptr;
            if (fallback != nullptr) {
                lv_image_set_src(emoji_image_, fallback->image_dsc());
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

    if (strcmp(emotion, "thinking") == 0) {
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        } else {
            if (emoji_image_ != nullptr) lv_obj_move_foreground(emoji_image_);
            if (emoji_label_ != nullptr) lv_obj_move_foreground(emoji_label_);
        }
        if (top_bar_ != nullptr) lv_obj_move_foreground(top_bar_);
        if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
        if (lesson_caption_bar_ != nullptr) lv_obj_move_foreground(lesson_caption_bar_);
        if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
    }

    UpdateConversationFaceLayout();

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}
