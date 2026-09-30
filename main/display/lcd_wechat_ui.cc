#include "lcd_display_internal.h"

#if CONFIG_TBOT_VOICE_DEMO
void LcdDisplay::StartChatCaptionTimer() {
    // SetupUI holds the LVGL lock. Render on the existing UI task, never RX/app.
    chat_caption_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
        ChatCaptionMailbox::Message caption;
        if (!display->lesson_mode_active_.load() &&
            Application::GetInstance().TakeChatCaption(caption)) {
            display->SetChatMessage(caption.assistant ? "assistant" : "user", caption.text);
        }
    }, 100, this);
    if (!chat_caption_timer_) ESP_LOGW(TAG, "chat_caption_timer_unavailable");
}
#endif

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
#if CONFIG_TBOT_VOICE_DEMO
    StartChatCaptionTimer();
#endif

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
    /* US-006 lesson background: full-screen, persistent poster. Created on `screen`
     * (NOT inside the flex `container_`) so it underlays the whole chat layout and is
     * not subject to flex sizing; moved to the back of the z-order so the chat bars
     * stay on top. Hidden until a lesson_step draws into it (SetLessonBackground);
     * no auto-hide timer (distinct from the centered chat-bubble preview). */
    lesson_background_ = lv_image_create(screen);
    lv_obj_set_size(lesson_background_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(lesson_background_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lesson_background_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(lesson_background_, 0);  // behind container_ (the chat UI)

    /* US-006 teaching object: foreground layer above the poster. Hidden until a
     * lesson_step draws scene.teachingObject.asset.src into it. */
    lesson_object_ = lv_image_create(screen);
    lv_obj_align(lesson_object_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lesson_object_, LV_OBJ_FLAG_HIDDEN);

    /* US-006 robot overlay: image layer above the teaching object. */
    lesson_robot_overlay_ = lv_image_create(screen);
    lv_obj_align(lesson_robot_overlay_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN);

    lesson_focus_cue_ = CreateLessonFocusCue(screen);

    const auto word_pill = LessonWordPillGeometry(width_, height_);
    lesson_word_pill_ = lv_obj_create(screen);
    lv_obj_set_pos(lesson_word_pill_, word_pill.x, word_pill.y);
    lv_obj_set_size(lesson_word_pill_, word_pill.width, word_pill.height);
    lv_obj_set_style_radius(lesson_word_pill_, word_pill.height / 2, 0);
    lv_obj_set_style_bg_color(lesson_word_pill_, lv_color_hex(0xFFF2A8), 0);
    lv_obj_set_style_bg_opa(lesson_word_pill_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(lesson_word_pill_, 3, 0);
    lv_obj_set_style_border_color(lesson_word_pill_, lv_color_hex(0xF2A900), 0);
    lesson_word_label_ = lv_label_create(lesson_word_pill_);
    lv_obj_set_width(lesson_word_label_, word_pill.width - 16);
    lv_obj_set_style_text_align(lesson_word_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lesson_word_label_, lv_color_hex(0x1E2A38), 0);
    lv_label_set_text(lesson_word_label_, "");
    lv_obj_center(lesson_word_label_);
    lv_obj_add_flag(lesson_word_pill_, LV_OBJ_FLAG_HIDDEN);

    /* Lesson-only caption overlay: fixed bottom strip, separate from scrolling chat. */
    lesson_caption_bar_ = lv_obj_create(screen);
    lv_obj_set_width(lesson_caption_bar_, width_ * kLessonCaptionWidthPercent / 100);
    lv_obj_set_height(lesson_caption_bar_, height_ * kLessonCaptionMaxHeightPercent / 100);
    lv_obj_set_style_radius(lesson_caption_bar_, 8, 0);
    lv_obj_set_style_bg_color(lesson_caption_bar_, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(lesson_caption_bar_, LV_OPA_80, 0);
    lv_obj_set_style_border_width(lesson_caption_bar_, 0, 0);
    lv_obj_set_style_pad_all(lesson_caption_bar_, lvgl_theme->spacing(3), 0);
    lv_obj_set_scrollbar_mode(lesson_caption_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(lesson_caption_bar_, LV_ALIGN_BOTTOM_MID, 0, -height_ / kLessonCaptionBottomInsetDivisor);
    lesson_caption_label_ = lv_label_create(lesson_caption_bar_);
    lv_obj_set_width(lesson_caption_label_, width_ * kLessonCaptionLabelWidthPercent / 100);
    lv_label_set_long_mode(lesson_caption_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lesson_caption_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lesson_caption_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(lesson_caption_label_, "");
    lv_obj_center(lesson_caption_label_);
    lv_obj_add_flag(lesson_caption_bar_, LV_OBJ_FLAG_HIDDEN);
#endif

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    // Full-screen face: size the image object to the whole panel and let LVGL
    // scale every neon GIF to COVER it (aspect-preserved, overflow cropped) so the
    // 320x240 source fills the 480x320 (3.5") LCD with no black margins.
    lv_obj_set_size(emoji_image_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(emoji_image_, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_inner_align(emoji_image_, LV_IMAGE_ALIGN_COVER);

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
}

#endif // CONFIG_USE_WECHAT_MESSAGE_STYLE
