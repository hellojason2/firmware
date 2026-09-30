#include "robot_uart.h"
#include "config.h"
#include "speaking_arm_transport.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#ifndef ROBOT_UART_NUM
#define ROBOT_UART_NUM UART_NUM_1
#endif

#ifndef ROBOT_UART_TX_PIN
#define ROBOT_UART_TX_PIN GPIO_NUM_17
#endif

#ifndef ROBOT_UART_RX_PIN
#define ROBOT_UART_RX_PIN GPIO_NUM_18
#endif

#ifndef ROBOT_UART_BAUD_RATE
#define ROBOT_UART_BAUD_RATE 115200
#endif

#ifndef ROBOT_UART_ACK_READER_ENABLED
#define ROBOT_UART_ACK_READER_ENABLED 1
#endif

#ifndef ROBOT_UART_ALT_NUM
#ifdef UART_NUM_2
#define ROBOT_UART_ALT_NUM UART_NUM_2
#else
#define ROBOT_UART_ALT_NUM ROBOT_UART_NUM
#endif
#endif

#ifndef ROBOT_UART_ALT_TX_PIN
#define ROBOT_UART_ALT_TX_PIN GPIO_NUM_NC
#endif

#ifndef ROBOT_UART_ALT_RX_PIN
#define ROBOT_UART_ALT_RX_PIN GPIO_NUM_NC
#endif

#define TAG "RobotUart"

static bool has_uart_profile(gpio_num_t tx_pin, gpio_num_t rx_pin) {
    return tx_pin != GPIO_NUM_NC && rx_pin != GPIO_NUM_NC;
}

static int clamp_servo_angle(int angle) {
    if (angle < 0) {
        return 0;
    }
    if (angle > 180) {
        return 180;
    }
    return angle;
}

static int clamp_percent(int percent) {
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static int percent_to_left_arm_angle(int percent) {
    return (clamp_percent(percent) * 60 + 50) / 100;
}

static int percent_to_right_arm_angle(int percent) {
    return 60 - percent_to_left_arm_angle(percent);
}

static int percent_to_head_angle(int percent) {
    return (clamp_percent(percent) * 180 + 50) / 100;
}

static bool configure_uart(uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (tx_pin == GPIO_NUM_NC || rx_pin == GPIO_NUM_NC) {
        return false;
    }
    uart_config_t uart_config = {
        .baud_rate = ROBOT_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
        },
    };

    esp_err_t err = uart_driver_install(port, 1024, 1024, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    err = uart_param_config(port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin port=%d failed: %s", port, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "UART ready: port=%d tx=%d rx=%d baud=%d",
        port, tx_pin, rx_pin, ROBOT_UART_BAUD_RATE);
    return true;
}

bool RobotUart::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    if (initialized_) {
        return true;
    }

    primary_ready_ = configure_uart(ROBOT_UART_NUM, ROBOT_UART_TX_PIN, ROBOT_UART_RX_PIN);
    if (has_uart_profile(ROBOT_UART_ALT_TX_PIN, ROBOT_UART_ALT_RX_PIN)) {
        if (ROBOT_UART_ALT_NUM == ROBOT_UART_NUM) {
            alt_ready_ = primary_ready_;
            if (alt_ready_) {
                ESP_LOGI(TAG, "Alternate UART profile ready: port=%d tx=%d rx=%d baud=%d",
                    ROBOT_UART_ALT_NUM, ROBOT_UART_ALT_TX_PIN, ROBOT_UART_ALT_RX_PIN, ROBOT_UART_BAUD_RATE);
            }
        } else {
            alt_ready_ = configure_uart(ROBOT_UART_ALT_NUM, ROBOT_UART_ALT_TX_PIN, ROBOT_UART_ALT_RX_PIN);
        }
    }
    if (!primary_ready_ && !alt_ready_) {
        ESP_LOGE(TAG, "No robot UART port ready");
        return false;
    }

    initialized_ = true;
    return true;
}

bool RobotUart::SendLeftArmRaise() {
    return SendArmAction("left_arm", "raise");
}

bool RobotUart::SendRightArmRaise() {
    return SendArmAction("right_arm", "raise");
}

bool RobotUart::SendLeftArmLower() {
    return SendArmAction("left_arm", "lower");
}

bool RobotUart::SendRightArmLower() {
    return SendArmAction("right_arm", "lower");
}

bool RobotUart::SendBothArmsRaise() {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    bool left_ok = SendLeftArmRaise();
    bool right_ok = SendRightArmRaise();
    return left_ok && right_ok;
}

bool RobotUart::SendBothArmsLower() {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    bool left_ok = SendLeftArmLower();
    bool right_ok = SendRightArmLower();
    return left_ok && right_ok;
}

bool RobotUart::SendLeftArmSetPercent(int percent) {
    int target_angle = percent_to_left_arm_angle(percent);
    return SendServoSweep("left_arm", "set_percent", 0, target_angle, 2, 20);
}

bool RobotUart::SendRightArmSetPercent(int percent) {
    int target_angle = percent_to_right_arm_angle(percent);
    return SendServoSweep("right_arm", "set_percent", 60, target_angle, 2, 20);
}

bool RobotUart::SendBothArmsSetPercent(int percent) {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    bool left_ok = SendLeftArmSetPercent(percent);
    bool right_ok = SendRightArmSetPercent(percent);
    return left_ok && right_ok;
}

bool RobotUart::SendHeadTurnLeft() {
    return SendServoSweep("head", "turn_left", 90, 45, 2, 20);
}

bool RobotUart::SendHeadTurnRight() {
    return SendServoSweep("head", "turn_right", 90, 135, 2, 20);
}

bool RobotUart::SendHeadCenter() {
    return SendServoSweep("head", "center", 90, 90, 2, 20);
}

bool RobotUart::SendHeadSetAngle(int angle) {
    int target_angle = clamp_servo_angle(angle);
    return SendServoSweep("head", "set_angle", 90, target_angle, 2, 20);
}

bool RobotUart::SendHeadSetPercent(int percent) {
    int target_angle = percent_to_head_angle(percent);
    return SendServoSweep("head", "set_percent", 90, target_angle, 2, 20);
}

bool RobotUart::SendArmAction(const std::string& part, const std::string& action) {
    if (part == "right_arm" && action == "raise") {
        return SendServoSweep(part, action, 60, 0, 2, 20);
    }
    if (part == "right_arm" && action == "lower") {
        return SendServoSweep(part, action, 0, 60, 2, 20);
    }
    if (action == "raise") {
        return SendServoSweep(part, action, 0, 60, 2, 20);
    }
    if (action == "lower") {
        return SendServoSweep(part, action, 60, 0, 2, 20);
    }
    ESP_LOGW(TAG, "Unsupported arm action: %s", action.c_str());
    return false;
}

bool RobotUart::TrySendAutomaticArm(bool left, int percent, const std::function<bool()>& owns) {
    // Automatic gestures use the board's configured primary wiring only.
    // Changing to alternate pins while TX is pending can truncate a frame.
    struct Adapter {
        RobotUart& robot;
        bool Ready() { return robot.initialized_ && robot.primary_ready_; }
        bool Idle() { return uart_wait_tx_done(ROBOT_UART_NUM, 0) == ESP_OK; }
        size_t BufferSpace() {
            size_t available = 0;
            return uart_get_tx_buffer_free_size(ROBOT_UART_NUM, &available) == ESP_OK ? available : 0;
        }
        bool SelectProfile() {
            return robot.SelectUartProfile("automatic", ROBOT_UART_NUM, ROBOT_UART_TX_PIN, ROBOT_UART_RX_PIN);
        }
        bool Write(const std::string& payload) {
            return uart_write_bytes(ROBOT_UART_NUM, payload.data(), payload.size()) == static_cast<int>(payload.size());
        }
    } adapter{*this};
    const bool sent = TrySpeakingArmWrite(uart_mutex_, left, percent, adapter, owns);
    ESP_LOGI(TAG, "speaking_arm_target part=%s percent=%d result=%s",
             left ? "left" : "right", percent, sent ? "sent" : "skipped");
    return sent;
}

bool RobotUart::SendServoSweep(const std::string& part, const std::string& action, int from, int to, int step, int delay_ms) {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    if (!Initialize()) {
        return false;
    }

    std::string payload = "{\"cmd\":\"servo\",\"part\":\"" + part +
        "\",\"action\":\"" + action + "\",\"from\":" + std::to_string(from) +
        ",\"to\":" + std::to_string(to) +
        ",\"step\":" + std::to_string(step) +
        ",\"delay_ms\":" + std::to_string(delay_ms) + "}\n";

    bool primary_sent = false;
    bool alternate_sent = false;
    if (using_alt_profile_ && alt_ready_) {
        alternate_sent = SendPayloadOnProfile("alternate", ROBOT_UART_ALT_NUM, ROBOT_UART_ALT_TX_PIN,
            ROBOT_UART_ALT_RX_PIN, alt_ready_, payload);
        primary_sent = SendPayloadOnProfile("primary", ROBOT_UART_NUM, ROBOT_UART_TX_PIN,
            ROBOT_UART_RX_PIN, primary_ready_, payload);
    } else {
        primary_sent = SendPayloadOnProfile("primary", ROBOT_UART_NUM, ROBOT_UART_TX_PIN,
            ROBOT_UART_RX_PIN, primary_ready_, payload);
        if (alt_ready_) {
            alternate_sent = SendPayloadOnProfile("alternate", ROBOT_UART_ALT_NUM, ROBOT_UART_ALT_TX_PIN,
                ROBOT_UART_ALT_RX_PIN, alt_ready_, payload);
        }
    }
    if (alternate_sent) {
        using_alt_profile_ = true;
    } else if (primary_sent) {
        using_alt_profile_ = false;
    }
    if (primary_sent && alternate_sent && ROBOT_UART_ALT_NUM == ROBOT_UART_NUM) {
        SelectUartProfile("primary-listen", ROBOT_UART_NUM, ROBOT_UART_TX_PIN, ROBOT_UART_RX_PIN);
    }
    return primary_sent || alternate_sent;
}

bool RobotUart::SendPayloadOnProfile(const char* profile_name, uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin,
        bool ready, const std::string& payload) {
    if (!ready) {
        ESP_LOGW(TAG, "%s UART profile is not ready", profile_name);
        return false;
    }
    if (!SelectUartProfile(profile_name, port, tx_pin, rx_pin)) {
        return false;
    }

    int written = uart_write_bytes(port, payload.data(), payload.size());
    if (written != static_cast<int>(payload.size())) {
        ESP_LOGW(TAG, "%s UART write incomplete: %d/%d", profile_name, written, static_cast<int>(payload.size()));
        return false;
    }

    ESP_LOGI(TAG, "Sent robot command via %s UART: %s", profile_name, payload.c_str());
    return true;
}

bool RobotUart::SelectUartProfile(const char* profile_name, uart_port_t port, gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (!has_uart_profile(tx_pin, rx_pin)) {
        ESP_LOGW(TAG, "%s UART profile has invalid pins", profile_name);
        return false;
    }

    esp_err_t err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin for %s profile port=%d tx=%d rx=%d failed: %s",
            profile_name, port, tx_pin, rx_pin, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Using %s UART profile: port=%d tx=%d rx=%d", profile_name, port, tx_pin, rx_pin);
    return true;
}

void RobotUart::StartAckReader() {
    if (!ROBOT_UART_ACK_READER_ENABLED) {
        ESP_LOGI(TAG, "Robot UART ACK reader disabled");
        return;
    }
    if (ack_reader_started_ || !primary_ready_) {
        return;
    }

    BaseType_t result = xTaskCreate(&RobotUart::AckReaderTask, "robot_uart_ack", 4096, this, 5, nullptr);
    if (result != pdPASS) {
        ESP_LOGW(TAG, "Robot UART ACK reader failed to start");
        return;
    }
    ack_reader_started_ = true;
    ESP_LOGI(TAG, "Robot UART ACK reader started");
}

// Parse mot dong tu slave. Chi nhan dang dong bat dau bang prefix ro rang; moi thu
// khac (ACK servo JSON, log slave dinh mau ANSI qua chung day console UART) deu bo qua.
void RobotUart::HandleReaderLine(const char* line) {
    RobotInputEvent evt;
    bool is_event = true;
    if (strcmp(line, "EVT:LEFT_CLICK") == 0) {
        evt = RobotInputEvent::LeftClick;
    } else if (strcmp(line, "EVT:RIGHT_CLICK") == 0) {
        evt = RobotInputEvent::RightClick;
    } else if (strcmp(line, "EVT:BOTH_CLICK") == 0) {
        evt = RobotInputEvent::BothClick;
    } else if (strcmp(line, "EVT:MENU_HOLD_3S") == 0) {
        evt = RobotInputEvent::MenuHold;
    } else if (strcmp(line, "EVT:RIGHT_HOLD_3S") == 0) {
        evt = RobotInputEvent::RightHold;
    } else if (strcmp(line, "SLAVE:READY") == 0) {
        evt = RobotInputEvent::SlaveReady;
    } else {
        is_event = false;
    }

    if (is_event) {
        ESP_LOGI(TAG, "Slave event: %s", line);
        if (event_cb_) {
            event_cb_(evt);
        }
        return;
    }

#if CONFIG_TBOT_VOICE_DEMO
    if (strncmp(line, "{\"ok\":", 6) == 0 || strcmp(line, "PONG") == 0) {
        ESP_LOGI(TAG, "Servo board reply: %.128s", line);
        return;
    }
#endif

    // PONG/ERR va ACK servo: chi ghi log muc thap, khong xu ly.
    ESP_LOGD(TAG, "Slave line ignored: %s", line);
}

void RobotUart::AckReaderTask(void* arg) {
    RobotUart* self = static_cast<RobotUart*>(arg);
    char line[160];
    int line_len = 0;
    uint8_t byte = 0;

    while (true) {
        int len = uart_read_bytes(ROBOT_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        if (byte == '\n') {
            line[line_len] = '\0';
            if (line_len > 0 && self != nullptr) {
                self->HandleReaderLine(line);
            }
            line_len = 0;
            continue;
        }

        if (byte == '\r') {
            continue;
        }

        if (line_len < static_cast<int>(sizeof(line)) - 1) {
            line[line_len++] = static_cast<char>(byte);
        } else {
            ESP_LOGW(TAG, "Slave line too long, dropping");
            line_len = 0;
        }
    }
}

bool RobotUart::SendControlLine(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lock(uart_mutex_);
    if (!Initialize()) {
        return false;
    }
    if (!primary_ready_) {
        return false;
    }
    // Dam bao ket thuc bang '\n'.
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }
    // Chon lai profile primary (trung voi pin listen) roi ghi. Khong dung uart_flush_input
    // de tranh xoa event dang den.
    if (!SelectUartProfile("primary-control", ROBOT_UART_NUM, ROBOT_UART_TX_PIN, ROBOT_UART_RX_PIN)) {
        return false;
    }
    int written = uart_write_bytes(ROBOT_UART_NUM, payload.data(), payload.size());
    return written == static_cast<int>(payload.size());
}
