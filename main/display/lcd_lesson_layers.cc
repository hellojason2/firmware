#include "lcd_display_internal.h"

#if CONFIG_BOARD_TYPE_LCDWIKI_ES3C35P
bool LcdDisplay::PresentLessonFramebuffer(const std::uint16_t* pixels,
                                          std::uint16_t width,
                                          std::uint16_t height) {
    if (pixels == nullptr || display_ == nullptr || lesson_background_ == nullptr ||
        width != width_ || height != height_) {
        return false;
    }
    DisplayLockGuard lock(this);
    const std::size_t framebuffer_bytes =
        static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
    if (lesson_cinematic_framebuffer_ == nullptr) {
        void* storage = heap_caps_malloc(
            framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (storage == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate persistent cinematic LVGL surface");
            return false;
        }
        lesson_cinematic_pixels_ = static_cast<std::uint16_t*>(storage);
        lesson_cinematic_framebuffer_ = std::make_unique<LvglAllocatedImage>(
            storage, framebuffer_bytes, width, height, width * sizeof(std::uint16_t),
            LV_COLOR_FORMAT_RGB565);
    }
    std::memcpy(lesson_cinematic_pixels_, pixels, framebuffer_bytes);
    SetLessonMode(true);
    const lv_img_dsc_t* image = lesson_cinematic_framebuffer_->image_dsc();
    lv_image_set_src(lesson_background_, image);
    lv_image_set_scale(lesson_background_, LessonImageCoverScale(
        static_cast<int>(image->header.w), static_cast<int>(image->header.h), width_, height_));
    lv_obj_align(lesson_background_, LV_ALIGN_CENTER, 0, 0);
    if (container_ != nullptr) lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    if (content_ != nullptr) lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(lesson_background_, LV_OBJ_FLAG_HIDDEN);
    if (top_bar_ != nullptr) lv_obj_move_foreground(top_bar_);
    if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
    lv_obj_invalidate(lesson_background_);
    lv_refr_now(display_);
    return true;
}

void LcdDisplay::SetLessonBackground(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (lesson_background_ == nullptr) {
        ESP_LOGE(TAG, "Lesson background object is not initialized");
        return;
    }

    if (image == nullptr) {
        lv_obj_add_flag(lesson_background_, LV_OBJ_FLAG_HIDDEN);
        ReplaceTrackedLessonLayer(&lesson_background_cached_, nullptr);
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (container_ != nullptr) {
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
        }
        if (content_ != nullptr) {
            lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0);
        }
        return;
    }

    ReplaceTrackedLessonLayer(&lesson_background_cached_, std::move(image));
    auto img_dsc = lesson_background_cached_->image_dsc();
    lv_image_set_src(lesson_background_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_image_set_scale(lesson_background_, LessonImageCoverScale(
            static_cast<int>(img_dsc->header.w), static_cast<int>(img_dsc->header.h), width_, height_));
    }
    lv_obj_align(lesson_background_, LV_ALIGN_CENTER, 0, 0);
    if (container_ != nullptr) {
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
    }
    if (content_ != nullptr) {
        lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    }
    lv_obj_remove_flag(lesson_background_, LV_OBJ_FLAG_HIDDEN);
    if (top_bar_ != nullptr) lv_obj_move_foreground(top_bar_);
    if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
}

void LcdDisplay::SetLessonObject(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (lesson_object_ == nullptr) {
        ESP_LOGE(TAG, "Lesson object layer is not initialized");
        return;
    }

    if (image == nullptr) {
        lv_obj_add_flag(lesson_object_, LV_OBJ_FLAG_HIDDEN);
        ReplaceTrackedLessonLayer(&lesson_object_cached_, nullptr);
        return;
    }

    ReplaceTrackedLessonLayer(&lesson_object_cached_, std::move(image));
    auto img_dsc = lesson_object_cached_->image_dsc();
    lv_image_set_src(lesson_object_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_image_set_scale(lesson_object_, LessonImageFitScale(
            static_cast<int>(img_dsc->header.w), static_cast<int>(img_dsc->header.h),
            width_ * kLessonObjectMaxWidthPercent / 100,
            height_ * kLessonObjectMaxHeightPercent / 100));
    }
    lv_obj_align(lesson_object_, LV_ALIGN_CENTER, 0, -height_ / kLessonObjectYOffsetDivisor);
    lv_obj_remove_flag(lesson_object_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lesson_object_);
    if (top_bar_ != nullptr) lv_obj_move_foreground(top_bar_);
    if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
}

void LcdDisplay::SetLessonRobotOverlayBounds(int left, int top, int width, int height) {
    DisplayLockGuard lock(this);
    lesson_robot_overlay_bounds_set_ = width > 0 && height > 0;
    lesson_robot_overlay_left_ = left;
    lesson_robot_overlay_top_ = top;
    lesson_robot_overlay_width_ = width;
    lesson_robot_overlay_height_ = height;
    if (lesson_robot_overlay_ == nullptr || lesson_robot_overlay_cached_ == nullptr) return;
    if (!lesson_robot_overlay_bounds_set_) {
        lv_obj_add_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_size(lesson_robot_overlay_, width * width_ / 480, height * height_ / 320);
    lv_image_set_scale(lesson_robot_overlay_, 256);
    lv_image_set_inner_align(lesson_robot_overlay_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(lesson_robot_overlay_, LV_ALIGN_TOP_LEFT,
                 left * width_ / 480, top * height_ / 320);
    lv_obj_remove_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (lesson_robot_overlay_ == nullptr) {
        ESP_LOGE(TAG, "Lesson robot overlay layer is not initialized");
        return;
    }

    if (image == nullptr) {
        lv_obj_add_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN);
        ReplaceTrackedLessonLayer(&lesson_robot_overlay_cached_, nullptr);
        return;
    }

    ReplaceTrackedLessonLayer(&lesson_robot_overlay_cached_, std::move(image));
    auto img_dsc = lesson_robot_overlay_cached_->image_dsc();
    lv_image_set_src(lesson_robot_overlay_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        const int max_width = lesson_robot_overlay_bounds_set_
            ? lesson_robot_overlay_width_ * width_ / 480
            : width_ * kLessonRobotMaxWidthPercent / 100;
        const int max_height = lesson_robot_overlay_bounds_set_
            ? lesson_robot_overlay_height_ * height_ / 320
            : height_ * kLessonRobotMaxHeightPercent / 100;
        if (lesson_robot_overlay_bounds_set_) {
            lv_obj_set_size(lesson_robot_overlay_, max_width, max_height);
            lv_image_set_scale(lesson_robot_overlay_, 256);
            lv_image_set_inner_align(lesson_robot_overlay_, LV_IMAGE_ALIGN_CONTAIN);
        } else {
            lv_obj_set_size(lesson_robot_overlay_, img_dsc->header.w, img_dsc->header.h);
            lv_image_set_scale(lesson_robot_overlay_, LessonImageFitScale(
                static_cast<int>(img_dsc->header.w), static_cast<int>(img_dsc->header.h),
                max_width, max_height));
        }
    }
    if (lesson_robot_overlay_bounds_set_) {
        lv_obj_align(lesson_robot_overlay_, LV_ALIGN_TOP_LEFT,
                     lesson_robot_overlay_left_ * width_ / 480,
                     lesson_robot_overlay_top_ * height_ / 320);
    } else {
        lv_obj_align(lesson_robot_overlay_, LV_ALIGN_BOTTOM_LEFT, width_ / 24, -height_ / kLessonRobotBottomInsetDivisor);
    }
    lv_obj_remove_flag(lesson_robot_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lesson_robot_overlay_);
    if (top_bar_ != nullptr) lv_obj_move_foreground(top_bar_);
    if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
}

bool LcdDisplay::StartLessonRobotEntrance(
    const LessonRobotEntrancePlan& plan, LessonVisualCompletion completion) {
    LessonVisualCompletion deferred_completion = std::move(completion);
    LessonVisualApplyResult deferred_result = LessonVisualApplyResult::kApplied;
    const char* deferred_reason = nullptr;
    bool complete_now = false;
    bool started = true;
    {
        DisplayLockGuard lock(this);
        CancelLessonRobotEntranceLocked();
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
        CancelLessonRendererSettledObservationLocked();
#endif
        lesson_renderer_memory_probe_.Capture(LessonRendererMemoryPhase::kStart);
        do {
            const lesson_tvideo::LayoutGeometry* arrived =
                lesson_tvideo::ArrivedGeometry(plan.layout_preset, 1);
            if (arrived == nullptr) {
                deferred_result = LessonVisualApplyResult::kRejected;
                deferred_reason = "unsupportedContract";
                complete_now = true;
                started = false;
                break;
            }

            lesson_robot_arrived_left_ = arrived->robot.left;
            lesson_robot_arrived_top_ = arrived->robot.top;
            lesson_robot_arrived_width_ = arrived->robot.width;
            lesson_robot_arrived_height_ = arrived->robot.height;

            if (lesson_robot_overlay_cached_ == nullptr) {
                SetLessonRobotOverlayBounds(0, 0, 0, 0);
                deferred_result = LessonVisualApplyResult::kDegraded;
                deferred_reason = "missingOverlay";
                complete_now = true;
                break;
            }
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) <
                kLessonRobotAnimationMinInternalHeapBytes) {
                SetLessonRobotOverlayBounds(0, 0, 0, 0);
                deferred_result = LessonVisualApplyResult::kDegraded;
                deferred_reason = "insufficientHeap";
                complete_now = true;
                break;
            }
            if (plan.reduced_motion) {
                SetLessonRobotOverlayBounds(arrived->robot.left, arrived->robot.top,
                                            arrived->robot.width, arrived->robot.height);
                deferred_result = LessonVisualApplyResult::kDegraded;
                deferred_reason = "reducedMotion";
                complete_now = true;
                break;
            }

            auto* context = new (std::nothrow) LessonRobotAnimationContext{
                this,
                lesson_tvideo::StateMachine({
                    "tvideoFlyWalk", 1, plan.layout_preset, 1, true, true, false}),
                std::move(deferred_completion),
            };
            if (context == nullptr) {
                SetLessonRobotOverlayBounds(arrived->robot.left, arrived->robot.top,
                                            arrived->robot.width, arrived->robot.height);
                deferred_result = LessonVisualApplyResult::kDegraded;
                deferred_reason = "insufficientHeap";
                complete_now = true;
                break;
            }
            lesson_robot_animation_context_ = context;
            LessonRendererMemoryContextOpened();
            context->timer = lv_timer_create([](lv_timer_t* timer) {
                auto* animation = static_cast<LessonRobotAnimationContext*>(
                    lv_timer_get_user_data(timer));
                if (animation == nullptr) return;
                LessonVisualCompletion timer_completion;
                LessonVisualApplyResult timer_result = LessonVisualApplyResult::kApplied;
                const char* timer_reason = nullptr;
                {
                    DisplayLockGuard lock(animation->owner);
                    if (animation->owner->lesson_robot_animation_context_ != animation) return;
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
                    const LessonRendererFrameAllocationToken allocation_token =
                        animation->owner->lesson_renderer_memory_probe_
                            .BeginFrameAllocationMeasurement();
#endif
                    const LessonRendererAnimationFrameResult frame =
                        AdvanceLessonRendererAnimationFrame(
                            &animation->machine, &animation->elapsed_ms,
                            kLessonRobotAnimationTickMs,
                            kLessonRobotAnimationTimeoutMs,
                            [](void* owner, const lesson_tvideo::Rect& robot, bool hidden) {
                                auto* display = static_cast<LcdDisplay*>(owner);
                                if (hidden) {
                                    display->SetLessonRobotOverlayBounds(0, 0, 0, 0);
                                } else {
                                    display->SetLessonRobotOverlayBounds(
                                        robot.left, robot.top, robot.width, robot.height);
                                }
                            },
                            animation->owner);
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
                    animation->owner->lesson_renderer_memory_probe_
                        .EndFrameAllocationMeasurement(allocation_token);
#endif
                    if (!frame.complete) return;

                    timer_completion = std::move(animation->completion);
                    timer_result = frame.timed_out ? LessonVisualApplyResult::kPhaseTimeout
                                                   : LessonVisualApplyResult::kApplied;
                    timer_reason = frame.timed_out ? "phaseTimeout" : nullptr;
                    animation->owner->lesson_robot_animation_context_ = nullptr;
                    lv_timer_delete(animation->timer);
                    LessonRendererMemoryAnimationStopped();
                    LessonRendererMemoryContextClosed();
                    animation->owner->lesson_renderer_memory_probe_.Capture(
                        LessonRendererMemoryPhase::kComplete);
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
                    animation->owner->ScheduleLessonRendererSettledObservationLocked(
                        LessonRendererMemoryPhase::kComplete);
#endif
                    delete animation;
                }
                if (timer_completion) timer_completion(timer_result, timer_reason);
            }, kLessonRobotAnimationTickMs, context);
            if (context->timer == nullptr) {
                lesson_robot_animation_context_ = nullptr;
                deferred_completion = std::move(context->completion);
                LessonRendererMemoryContextClosed();
                delete context;
                SetLessonRobotOverlayBounds(arrived->robot.left, arrived->robot.top,
                                            arrived->robot.width, arrived->robot.height);
                deferred_result = LessonVisualApplyResult::kDegraded;
                deferred_reason = "animationStartFailed";
                complete_now = true;
                started = false;
            } else {
                LessonRendererMemoryAnimationStarted();
                lesson_renderer_memory_probe_.Capture(LessonRendererMemoryPhase::kPeak);
            }
        } while (false);
        if (complete_now) {
            lesson_renderer_memory_probe_.Capture(LessonRendererMemoryPhase::kComplete);
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
            ScheduleLessonRendererSettledObservationLocked(
                LessonRendererMemoryPhase::kComplete);
#endif
        }
    }
    if (complete_now && deferred_completion) {
        deferred_completion(deferred_result, deferred_reason);
    }
    return started;
}

void LcdDisplay::CancelLessonRobotEntranceLocked() {
    auto* context = static_cast<LessonRobotAnimationContext*>(lesson_robot_animation_context_);
    lesson_robot_animation_context_ = nullptr;
    if (context == nullptr) return;
    if (context->timer != nullptr) {
        lv_timer_delete(context->timer);
        LessonRendererMemoryAnimationStopped();
    }
    context->completion = nullptr;
    LessonRendererMemoryContextClosed();
    delete context;
    lesson_renderer_memory_probe_.Capture(LessonRendererMemoryPhase::kCancel);
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
    ScheduleLessonRendererSettledObservationLocked(
        LessonRendererMemoryPhase::kCancel);
#endif
}

#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
void LcdDisplay::CancelLessonRendererSettledObservationLocked() {
    ++lesson_renderer_settled_generation_;
    lesson_renderer_settled_armed_generation_ = 0;
    if (lesson_renderer_settled_timer_ == nullptr) return;
    lv_timer_delete(lesson_renderer_settled_timer_);
    lesson_renderer_settled_timer_ = nullptr;
}

void LcdDisplay::ScheduleLessonRendererSettledObservationLocked(
    LessonRendererMemoryPhase phase) {
    CancelLessonRendererSettledObservationLocked();
    lesson_renderer_settled_phase_ = phase;
    lesson_renderer_settled_armed_generation_ =
        lesson_renderer_settled_generation_;
    lesson_renderer_settled_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* owner = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
        if (owner == nullptr) return;
        DisplayLockGuard lock(owner);
        if (owner->lesson_renderer_settled_timer_ != timer ||
            owner->lesson_renderer_settled_armed_generation_ !=
                owner->lesson_renderer_settled_generation_) {
            return;
        }
        const LessonRendererMemoryPhase phase =
            owner->lesson_renderer_settled_phase_;
        owner->lesson_renderer_settled_timer_ = nullptr;
        owner->lesson_renderer_settled_armed_generation_ = 0;
        lv_timer_delete(timer);
        owner->lesson_renderer_memory_probe_.CaptureSettled(
            phase, kLessonRendererSettledObservationMs);
    }, kLessonRendererSettledObservationMs, this);
    if (lesson_renderer_settled_timer_ == nullptr) {
        lesson_renderer_settled_armed_generation_ = 0;
    }
}
#endif

void LcdDisplay::CancelLessonRobotEntrance() {
    DisplayLockGuard lock(this);
    CancelLessonRobotEntranceLocked();
}

bool LcdDisplay::ApplyLessonVisualState(
    const LessonVisualState& state, LessonVisualCompletion completion) {
    LessonVisualCompletion deferred_completion = std::move(completion);
    LessonVisualApplyResult deferred_result = LessonVisualApplyResult::kApplied;
    const char* deferred_reason = nullptr;
    {
        DisplayLockGuard lock(this);
        CancelLessonRobotEntranceLocked();
        if (!state.overlay_available || lesson_robot_overlay_cached_ == nullptr) {
            SetLessonRobotOverlayBounds(0, 0, 0, 0);
            deferred_result = LessonVisualApplyResult::kDegraded;
            deferred_reason = "missingOverlay";
        } else {
            int top = lesson_robot_arrived_top_;
            int width = lesson_robot_arrived_width_;
            int height = lesson_robot_arrived_height_;
            if (state.kind == LessonVisualStateKind::kCelebrate) top -= 8;
            if (state.kind == LessonVisualStateKind::kCompletion) {
                width += 4;
                height += 4;
            }
            SetLessonRobotOverlayBounds(
                lesson_robot_arrived_left_, top, width, height);
        }
    }
    if (deferred_completion) deferred_completion(deferred_result, deferred_reason);
    return true;
}

#else

void LcdDisplay::SetLessonBackground(std::unique_ptr<LvglImage> image) { (void)image; }
void LcdDisplay::SetLessonObject(std::unique_ptr<LvglImage> image) { (void)image; }
void LcdDisplay::SetLessonRobotOverlayBounds(int left, int top, int width, int height) {
    (void)left;
    (void)top;
    (void)width;
    (void)height;
}
void LcdDisplay::SetLessonRobotOverlay(std::unique_ptr<LvglImage> image) { (void)image; }
bool LcdDisplay::PresentLessonFramebuffer(const std::uint16_t*, std::uint16_t, std::uint16_t) { return false; }
bool LcdDisplay::StartLessonRobotEntrance(const LessonRobotEntrancePlan&, LessonVisualCompletion completion) {
    if (completion) completion(LessonVisualApplyResult::kRejected, "unsupportedContract");
    return false;
}
void LcdDisplay::CancelLessonRobotEntranceLocked() { lesson_robot_animation_context_ = nullptr; }
#ifdef TBOT_RENDERER_MEMORY_DIAGNOSTICS
void LcdDisplay::CancelLessonRendererSettledObservationLocked() {}
void LcdDisplay::ScheduleLessonRendererSettledObservationLocked(LessonRendererMemoryPhase) {}
#endif
void LcdDisplay::CancelLessonRobotEntrance() {}
bool LcdDisplay::ApplyLessonVisualState(const LessonVisualState&, LessonVisualCompletion completion) {
    if (completion) completion(LessonVisualApplyResult::kRejected, "unsupportedContract");
    return false;
}

#endif
