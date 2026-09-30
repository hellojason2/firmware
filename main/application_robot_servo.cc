#include "application_internal.h"

bool Application::HandleRobotActionMessage(const cJSON* root, ChatRequestContext context) {
    if (!IsChatRequestCurrent(context))
        return false;
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot action ignored");
        return false;
    }

    auto action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) {
        return false;
    }

    if (strcmp(action->valuestring, "head_set_angle") == 0) {
        auto angle = cJSON_GetObjectItem(root, "angle");
        int target_angle = cJSON_IsNumber(angle) ? angle->valueint : 90;
        Schedule([this, target_angle, context]() {
            if (!IsChatRequestCurrent(context))
                return;
            SendHeadSetAngle(target_angle);
        });
        return true;
    }

    auto schedule_percent_action = [this, root, context](bool (Application::*method)(int),
                                                         int default_percent) {
        auto percent = cJSON_GetObjectItem(root, "percent");
        int target_percent = cJSON_IsNumber(percent) ? percent->valueint : default_percent;
        Schedule([this, method, target_percent, context]() {
            if (!IsChatRequestCurrent(context))
                return;
            (this->*method)(target_percent);
        });
    };
    if (strcmp(action->valuestring, "left_arm_set_percent") == 0) {
        schedule_percent_action(&Application::SendLeftArmSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "right_arm_set_percent") == 0) {
        schedule_percent_action(&Application::SendRightArmSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "both_arms_set_percent") == 0) {
        schedule_percent_action(&Application::SendBothArmsSetPercent, 100);
        return true;
    }
    if (strcmp(action->valuestring, "head_set_percent") == 0) {
        schedule_percent_action(&Application::SendHeadSetPercent, 50);
        return true;
    }

    using RobotActionHandler = bool (Application::*)();
    static const struct {
        const char* action;
        RobotActionHandler handler;
    } handlers[] = {
        {"left_arm_raise", &Application::SendLeftArmRaise},
        {"right_arm_raise", &Application::SendRightArmRaise},
        {"left_arm_lower", &Application::SendLeftArmLower},
        {"right_arm_lower", &Application::SendRightArmLower},
        {"both_arms_raise", &Application::SendBothArmsRaise},
        {"both_arms_lower", &Application::SendBothArmsLower},
        {"head_turn_left", &Application::SendHeadTurnLeft},
        {"head_turn_right", &Application::SendHeadTurnRight},
        {"head_center", &Application::SendHeadCenter},
    };

    for (const auto& handler : handlers) {
        if (strcmp(action->valuestring, handler.action) == 0) {
            Schedule([this, method = handler.handler, context]() {
                if (!IsChatRequestCurrent(context))
                    return;
                (this->*method)();
            });
            return true;
        }
    }

    return false;
}

void Application::HandleEmotionGesture(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Emotion gesture ignored for arm control: %s", emotion);
}

bool Application::SendLeftArmRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmRaise();
}

bool Application::SendRightArmRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmRaise();
}

bool Application::SendLeftArmLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmLower();
}

bool Application::SendRightArmLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmLower();
}

bool Application::SendBothArmsRaise() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsRaise();
}

bool Application::SendBothArmsLower() {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsLower();
}

bool Application::SendLeftArmSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendLeftArmSetPercent(percent);
}

bool Application::SendRightArmSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendRightArmSetPercent(percent);
}

bool Application::SendBothArmsSetPercent(int percent) {
    speaking_arm_dispatch_.Cancel();
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendBothArmsSetPercent(percent);
}

bool Application::SendHeadTurnLeft() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadTurnLeft();
}

bool Application::SendHeadTurnRight() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadTurnRight();
}

bool Application::SendHeadCenter() {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadCenter();
}

bool Application::SendHeadSetAngle(int angle) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadSetAngle(angle);
}

bool Application::SendHeadSetPercent(int percent) {
    if (lesson_runtime_active_.load()) {
        ESP_LOGI(TAG, "lesson robot uart action ignored");
        return false;
    }
    return robot_uart_.SendHeadSetPercent(percent);
}

LessonRuntimeToken Application::GetLessonRuntimeToken() const {
    if (!lesson_runtime_active_.load())
        return {};
    return {lesson_runtime_generation_.load()};
}

LessonEmbodiedMotionResult Application::ApplyLessonEmbodiedPreset(
    const LessonRuntimeToken& token, const LessonEmbodiedPreset& preset) {
    if (!IsLessonRuntimeTokenAuthorized(lesson_runtime_active_.load(),
                                        lesson_runtime_generation_.load(), token)) {
        ESP_LOGI(TAG, "stale lesson embodied action token rejected");
        return LessonEmbodiedMotionResult::kRejected;
    }
    return ApplyLessonEmbodiedPresetCommands(
        preset, [this](int percent) { return robot_uart_.SendHeadSetPercent(percent); },
        [this](int percent) { return robot_uart_.SendLeftArmSetPercent(percent); },
        [this](int percent) { return robot_uart_.SendRightArmSetPercent(percent); });
}

LessonEmbodiedMotionResult Application::CancelLessonEmbodiedAction(
    const LessonRuntimeToken& token) {
    return RestoreLessonRestPose(token);
}

LessonEmbodiedMotionResult Application::RestoreLessonRestPose(const LessonRuntimeToken& token) {
    return ApplyLessonEmbodiedPreset(token,
                                     ResolveLessonEmbodiedPreset(LessonEmbodiedIntent::kRestWarm));
}

#if CONFIG_TBOT_COURSE_MODE_HIL_DIAGNOSTICS
bool Application::RunCourseModeHilTftPattern() {
    return ScheduleAndWait(
        [] {
            Display* display = Board::GetInstance().GetDisplay();
            if (display == nullptr)
                return false;
            lv_obj_t* screen = lv_screen_active();
            if (screen == nullptr)
                return false;
            lv_obj_t* pattern = lv_obj_create(screen);
            if (pattern == nullptr)
                return false;
            lv_obj_set_size(pattern, lv_pct(100), lv_pct(100));
            lv_obj_center(pattern);
            lv_obj_set_flex_flow(pattern, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_all(pattern, 0, 0);
            lv_obj_set_style_pad_gap(pattern, 0, 0);
            lv_obj_set_style_border_width(pattern, 0, 0);
            lv_obj_set_style_radius(pattern, 0, 0);
            constexpr std::array<std::uint32_t, 3> colors = {0xFF0000, 0x00FF00, 0x0000FF};
            for (std::uint32_t color : colors) {
                lv_obj_t* bar = lv_obj_create(pattern);
                if (bar == nullptr)
                    return false;
                lv_obj_set_size(bar, lv_pct(34), lv_pct(100));
                lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(bar, 0, 0);
                lv_obj_set_style_radius(bar, 0, 0);
            }
            lv_timer_t* timer = lv_timer_create(
                [](lv_timer_t* value) {
                    lv_obj_t* object = static_cast<lv_obj_t*>(lv_timer_get_user_data(value));
                    if (object != nullptr)
                        lv_obj_delete(object);
                    lv_timer_delete(value);
                },
                1000, pattern);
            if (timer == nullptr) {
                lv_obj_delete(pattern);
                return false;
            }
            lv_timer_set_repeat_count(timer, 1);
            return true;
        },
        1000);
}

CourseModeHilSdEvidence Application::RunCourseModeHilSdRead(const std::string& relative_path,
                                                            const std::string& expected_sha256) {
    constexpr char kRoot[] = "/sdcard/tbot/lesson-assets";
    constexpr std::size_t kMaxBytes = 16 * 1024 * 1024;
    constexpr std::int64_t kMaxReadUs = 5 * 1000 * 1000;
    if (expected_sha256.size() != 64)
        return {};
    for (char ch : expected_sha256) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && (ch < 'a' || ch > 'f'))
            return {};
    }
    CourseModeHilAssetFile asset;
    if (!OpenCourseModeHilAssetFile(kRoot, relative_path, kMaxBytes, &asset))
        return {};

    auto hash_file = [&](std::uint32_t* bytes, std::string* sha256) {
        std::rewind(asset.file);
        std::clearerr(asset.file);
        const int descriptor = fileno(asset.file);
        mbedtls_sha256_context context;
        mbedtls_sha256_init(&context);
#if MBEDTLS_VERSION_MAJOR >= 3
        int result = mbedtls_sha256_starts(&context, 0);
#else
        int result = mbedtls_sha256_starts_ret(&context, 0);
#endif
        std::array<unsigned char, 4096> buffer{};
        std::size_t total = 0;
        const std::int64_t deadline = esp_timer_get_time() + kMaxReadUs;
        while (result == 0) {
            const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), asset.file);
            if (read > 0) {
                total += read;
                if (total > kMaxBytes || esp_timer_get_time() > deadline) {
                    result = -1;
                    break;
                }
#if MBEDTLS_VERSION_MAJOR >= 3
                result = mbedtls_sha256_update(&context, buffer.data(), read);
#else
                result = mbedtls_sha256_update_ret(&context, buffer.data(), read);
#endif
            }
            if (read < buffer.size()) {
                if (std::ferror(asset.file))
                    result = -1;
                break;
            }
            esp_task_wdt_reset();
            taskYIELD();
        }
        std::array<unsigned char, 32> digest{};
        if (result == 0) {
#if MBEDTLS_VERSION_MAJOR >= 3
            result = mbedtls_sha256_finish(&context, digest.data());
#else
            result = mbedtls_sha256_finish_ret(&context, digest.data());
#endif
        }
        struct stat final_metadata{};
        if (result == 0 && (descriptor < 0 || fstat(descriptor, &final_metadata) != 0 ||
                            !S_ISREG(final_metadata.st_mode) || total != asset.bytes ||
                            static_cast<std::uint64_t>(final_metadata.st_size) != asset.bytes)) {
            result = -1;
        }
        mbedtls_sha256_free(&context);
        if (result != 0 || total == 0 || total > UINT32_MAX)
            return false;
        constexpr char kHex[] = "0123456789abcdef";
        sha256->resize(64);
        for (std::size_t index = 0; index < digest.size(); ++index) {
            (*sha256)[index * 2] = kHex[digest[index] >> 4U];
            (*sha256)[index * 2 + 1] = kHex[digest[index] & 0x0FU];
        }
        *bytes = static_cast<std::uint32_t>(total);
        return true;
    };

    std::uint32_t first_bytes = 0;
    std::uint32_t second_bytes = 0;
    std::string first_sha;
    std::string second_sha;
    const bool verified = hash_file(&first_bytes, &first_sha) && first_sha == expected_sha256 &&
                          hash_file(&second_bytes, &second_sha) && second_bytes == first_bytes &&
                          second_sha == first_sha;
    CloseCourseModeHilAssetFile(&asset);
    if (!verified)
        return {};
    return {true, second_bytes, second_sha, "verifiedReadback"};
}

bool Application::RunCourseModeHilAudioDrain() {
    return audio_service_.WaitForPlaybackQueueEmpty(1000) && audio_service_.IsIdle();
}

bool Application::RunCourseModeHilStopAndRest() {
    speaking_arm_dispatch_.Cancel();
    bool sent = robot_uart_.SendBothArmsLower();
    return robot_uart_.SendHeadCenter() && sent;
}

bool Application::RunCourseModeHilSafeMotion(int duration_ms) {
    speaking_arm_dispatch_.Cancel();
    if (duration_ms <= 0 || duration_ms > 750)
        return false;
    bool sent = robot_uart_.SendRightArmRaise();
    sent = robot_uart_.SendHeadCenter() && sent;
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return RunCourseModeHilStopAndRest() && sent;
}
#endif

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}
