#include "lcd_display_internal.h"

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
void LcdDisplay::SetLessonCaption(const char* content) {
    DisplayLockGuard lock(this);
    const bool empty = content == nullptr || content[0] == '\0';
    lesson_caption_active_ = !empty;
    if (lesson_caption_bar_ != nullptr && lesson_caption_label_ != nullptr) {
        lv_label_set_text(lesson_caption_label_, empty ? "" : content);
        if (empty || hide_subtitle_) {
            lv_obj_add_flag(lesson_caption_bar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_remove_flag(lesson_caption_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(lesson_caption_bar_, LV_ALIGN_BOTTOM_MID, 0, -height_ / kLessonCaptionBottomInsetDivisor);
        lv_obj_move_foreground(lesson_caption_bar_);
        return;
    }

    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetLessonCaption('%s') failed: caption label is nullptr", content ? content : "");
        }
        return;
    }
    lv_label_set_text(chat_message_label_, empty ? "" : content);
    if (bottom_bar_ != nullptr) {
        if (empty || hide_subtitle_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_move_foreground(bottom_bar_);
        }
    }
}

void LcdDisplay::SetLessonTeachingWord(const char* text) {
    DisplayLockGuard lock(this);
    if (lesson_word_pill_ == nullptr || lesson_word_label_ == nullptr) return;
    const bool empty = text == nullptr || text[0] == '\0';
    lv_label_set_text(lesson_word_label_, empty ? "" : text);
    if (empty) {
        lv_obj_add_flag(lesson_word_pill_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(lesson_word_pill_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lesson_word_pill_);
}

void LcdDisplay::SetLessonVisualFocus(LessonVisualFocusRegion region) {
    DisplayLockGuard lock(this);
    if (lesson_focus_cue_ == nullptr) return;
    int reference_x = 67;
    const int reference_y = 215;
    if (region == LessonVisualFocusRegion::kRightChoice) reference_x = 366;
    const int x = reference_x * width_ / kLessonFocusReferenceWidth;
    const int y = reference_y * height_ / kLessonFocusReferenceHeight;
    lv_obj_set_pos(lesson_focus_cue_, x - kLessonFocusCueSize / 2,
                   y - kLessonFocusCueSize / 2);
    lv_obj_remove_flag(lesson_focus_cue_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lesson_focus_cue_);
}

void LcdDisplay::ClearLessonVisualFocus() {
    DisplayLockGuard lock(this);
    if (lesson_focus_cue_ != nullptr) {
        lv_obj_add_flag(lesson_focus_cue_, LV_OBJ_FLAG_HIDDEN);
    }
}

// US-006 lesson display mode: hide/show the idle realtime emoji face.
void LcdDisplay::SetLessonMode(bool active) {
    DisplayLockGuard lock(this);
    if (lesson_mode_active_.exchange(active) == active) return;
    lesson_caption_active_ = false;
    // Conversation callbacks and transitions share this lock, including callbacks
    // already queued when their timers are stopped.
    std::uint8_t bit = 1;
    if (active) lesson_chat_visibility_ = 0;
    for (auto* bar : {top_bar_, status_bar_, bottom_bar_}) {
        if (bar != nullptr) {
            if (active) {
                if (!lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN)) lesson_chat_visibility_ |= bit;
                lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            } else if ((lesson_chat_visibility_ & bit) != 0 &&
                       (bar != bottom_bar_ || !hide_subtitle_)) {
                lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            }
        }
        bit <<= 1;
    }
    if (active) {
        if (notification_timer_) esp_timer_stop(notification_timer_);
        if (preview_timer_) esp_timer_stop(preview_timer_);
        if (notification_label_) lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        if (preview_image_) lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (low_battery_popup_) lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    } else if (status_label_) {
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!active && lesson_focus_cue_ != nullptr) {
        lv_obj_add_flag(lesson_focus_cue_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ == nullptr && emoji_image_ == nullptr && emoji_label_ == nullptr) {
        return;
    }
    if (active) {
        // Pause any animated face so it does not run invisibly behind the lesson scene.
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Re-show the face; the lesson_stop/lesson_error handler issues the SetEmotion that
        // follows (happy/sad), which restores the correct image/GIF.
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        } else {
            if (emoji_image_ != nullptr) lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            if (emoji_label_ != nullptr) lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
#else
void LcdDisplay::SetLessonCaption(const char* content) { (void)content; }
void LcdDisplay::SetLessonTeachingWord(const char* text) { (void)text; }
void LcdDisplay::SetLessonVisualFocus(LessonVisualFocusRegion region) { (void)region; }
void LcdDisplay::ClearLessonVisualFocus() {}
void LcdDisplay::SetLessonMode(bool active) { (void)active; }
#endif
