#include "application_internal.h"
static std::string FirmwareVersionForHeartbeat() {
    const std::string user_agent = SystemInfo::GetUserAgent();
    const std::size_t slash = user_agent.rfind('/');
    if (slash == std::string::npos || slash + 1 >= user_agent.size()) {
        return user_agent;
    }
    return user_agent.substr(slash + 1);
}

static std::string CopyStringField(cJSON* object, const char* key, const char* fallback) {
    if (object == nullptr) {
        return fallback;
    }
    cJSON* value = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr || value->valuestring[0] == '\0') {
        return fallback;
    }
    return value->valuestring;
}

static int ClampInt(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int ExtractWifiRssi(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* rssi = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "rssi");
    if (!cJSON_IsNumber(rssi)) {
        return -127;
    }
    return ClampInt(rssi->valueint, -127, 0);
}

static std::string ExtractWifiSsid(cJSON* status_root) {
    cJSON* network = status_root == nullptr ? nullptr : cJSON_GetObjectItem(status_root, "network");
    cJSON* ssid = network == nullptr ? nullptr : cJSON_GetObjectItem(network, "ssid");
    if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr) {
        return "";
    }
    const std::size_t length = std::strlen(ssid->valuestring);
    if (length == 0 || length > 32) {
        return "";
    }
    return ssid->valuestring;
}

std::string BuildTbotHeartbeatBody(const std::string& status_json,
                                   const std::string& device_id) {
    cJSON* status_root = cJSON_Parse(status_json.c_str());
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    const std::string firmware_version = FirmwareVersionForHeartbeat();
    cJSON_AddStringToObject(root, "firmware_version", firmware_version.c_str());

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
        battery_level = 0;
    }
    battery_level = ClampInt(battery_level, 0, 100);
    cJSON_AddNumberToObject(root, "battery_level", battery_level);

    const int wifi_rssi = ExtractWifiRssi(status_root);
    cJSON* connectivity = cJSON_CreateObject();
    cJSON_AddStringToObject(connectivity, "connectivity_state", "online");
    cJSON_AddNumberToObject(connectivity, "wifi_rssi", wifi_rssi);
    const std::string wifi_ssid = ExtractWifiSsid(status_root);
    if (!wifi_ssid.empty()) {
        cJSON_AddStringToObject(connectivity, "wifi_ssid", wifi_ssid.c_str());
    }
    cJSON_AddItemToObject(root, "connectivity_metrics", connectivity);

    const std::string ble_state = CopyStringField(status_root, "ble_state", "off");
    const std::string ap_state = CopyStringField(status_root, "ap_state", "off");
    cJSON_AddStringToObject(root, "ble_state", ble_state.c_str());
    cJSON_AddStringToObject(root, "ap_state", ap_state.c_str());

    float temp = 0.0f;
    if (Board::GetInstance().GetTemperature(temp)) {
        cJSON_AddNumberToObject(root, "temp", temp);
    }

    char* raw = cJSON_PrintUnformatted(root);
    std::string body = raw == nullptr ? "{}" : raw;
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    if (status_root != nullptr) {
        cJSON_Delete(status_root);
    }
    return body;
}

void Application::DispatchDeviceHeartbeat() {
#if CONFIG_TBOT_COURSE_MODE_LOCAL_ENDPOINT
    return;
#else
    const DeviceState device_state = GetDeviceState();
    if (device_state != kDeviceStateIdle && device_state != kDeviceStateListening &&
        device_state != kDeviceStateSpeaking) {
        return;
    }

    bool expected = false;
    if (!heartbeat_inflight_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Heartbeat already in flight; skipping this tick");
        return;
    }

    Settings backend_settings("backend", false);
    const std::string api_url = backend_settings.GetString("api_url");
    if (api_url.empty()) {
        heartbeat_inflight_.store(false);
        return;
    }
    const std::string device_secret = backend_settings.GetString("device_secret");
    if (device_secret.empty()) {
        heartbeat_inflight_.store(false);
        return;
    }
    const std::string backend_device_id = backend_settings.GetString("device_id");
    if (backend_device_id.empty()) {
        ESP_LOGW(TAG, "Heartbeat skipped: missing backend device id");
        heartbeat_inflight_.store(false);
        return;
    }

    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.find("/v1") == std::string::npos) {
        base += "/v1";
    }
    const std::string status_json = Board::GetInstance().GetDeviceStatusJson();

    auto* ctx = new HeartbeatContext{this, base + "/device/heartbeat", device_secret,
                                     BuildTbotHeartbeatBody(status_json, backend_device_id)};
    const NetworkWorkItem work{NetworkWorkKind::kHeartbeat, ctx};
    if (open_channel_queue == nullptr || xQueueSend(open_channel_queue, &work, 0) != pdTRUE) {
        ESP_LOGE(TAG, "heartbeat worker queue unavailable; retrying next tick");
        delete ctx;
        heartbeat_inflight_.store(false);
    } else {
        ESP_LOGI(TAG, "Heartbeat queued");
    }
#endif
}

void Application::HeartbeatTask(void* arg) {
    auto* ctx = static_cast<HeartbeatContext*>(arg);
    if (ctx == nullptr)
        return;
    auto* self = ctx->app;
    ESP_LOGI(TAG, "Heartbeat worker received request");
    const std::string url = ctx->url;
    const std::string device_secret = ctx->device_secret;
    std::string body = std::move(ctx->body);
    delete ctx;

    const int status_code = self->SendDeviceHeartbeat(url, device_secret, std::move(body));
    self->Schedule([self, status_code]() {
        self->heartbeat_inflight_.store(false);
        if (status_code == 401 || status_code == 403) {
            self->HandleHeartbeatAuthFailure(status_code);
        }
    });
}

int Application::SendDeviceHeartbeat(const std::string& url, const std::string& device_secret,
                                     std::string body) {
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(2);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client for heartbeat");
        return 0;
    }
    http->SetTimeout(5000);
    http->SetHeader("X-Device-Token", device_secret);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "Heartbeat HTTP open failed: 0x%x", http->GetLastError());
        http->Close();
        return 0;
    }
    const int status_code = http->GetStatusCode();
    http->Close();

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG, "Heartbeat failed (HTTP %d)", status_code);
        return status_code;
    }
    ESP_LOGI(TAG, "Heartbeat accepted (HTTP %d)", status_code);
    return status_code;
}
