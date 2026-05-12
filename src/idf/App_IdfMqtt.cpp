#include "App_IdfMqtt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "App_IdfCommandExecutor.h"
#include "App_IdfOta.h"
#include "App_IdfSensors.h"
#include "App_IdfSystem.h"
#include "App_Log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

namespace AppIdfMqtt {
namespace {

constexpr const char* TAG_MQTT_IDF = "IDF_MQTT";
constexpr const char* kBrokerHost = "<your-mqtt-broker-host>";
constexpr uint32_t kBrokerPort = 1883;
constexpr const char* kBrokerUser = "<your-mqtt-username>";
constexpr const char* kBrokerPass = "<your-mqtt-password>";
constexpr const char* kBroadcastCommandTopic = "server/cmd/broadcast/down";
constexpr size_t kMaxPayloadLen = 1024;

SemaphoreHandle_t g_mutex = nullptr;
esp_mqtt_client_handle_t g_client = nullptr;
Snapshot g_snapshot;
char g_otaResultTopic[kTopicBufferLen] = "esp32/unknown/ota/result";
char g_otaPreflightAckTopic[kTopicBufferLen] = "esp32/unknown/ota/preflight_ack";
char g_deviceInfoTopic[kTopicBufferLen] = "esp32/unknown/telemetry/device_info";
char g_rxTopic[kTopicBufferLen] = {};
char g_rxPayload[kMaxPayloadLen + 1] = {};
int g_rxPayloadLen = 0;
bool g_rxOverflow = false;

class MutexLock {
public:
    explicit MutexLock(TickType_t timeoutTicks = portMAX_DELAY) {
        if (g_mutex != nullptr) {
            _locked = xSemaphoreTake(g_mutex, timeoutTicks) == pdTRUE;
        }
    }

    ~MutexLock() {
        if (_locked && g_mutex != nullptr) {
            xSemaphoreGive(g_mutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    bool _locked = false;
};

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t setLastError(esp_err_t err) {
    g_snapshot.lastError = err;
    return err;
}

void copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstSize, "%s", src);
}

const char* firmwareVersion() {
    const esp_app_desc_t* desc = esp_app_get_description();
    return (desc != nullptr && desc->version[0] != '\0') ? desc->version : "IDF";
}

bool sanitizeDeviceId(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return false;
    }
    output[0] = '\0';
    if (input == nullptr) {
        return false;
    }

    size_t written = 0;
    for (size_t i = 0; input[i] != '\0' && written + 1 < outputSize; ++i) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (isdigit(ch)) {
            output[written++] = static_cast<char>(ch);
        } else if (isalpha(ch)) {
            output[written++] = static_cast<char>(toupper(ch));
        }
    }
    output[written] = '\0';
    return written > 0;
}

void setDefaultDeviceIdentityLocked() {
    if (g_snapshot.deviceId[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        copyText(g_snapshot.deviceId, sizeof(g_snapshot.deviceId), "UNKNOWN");
        return;
    }

    snprintf(g_snapshot.deviceId,
             sizeof(g_snapshot.deviceId),
             "%02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

void refreshTopicsLocked() {
    setDefaultDeviceIdentityLocked();
    snprintf(g_snapshot.clientId, sizeof(g_snapshot.clientId), "esp32_%s", g_snapshot.deviceId);
    snprintf(g_snapshot.commandTopic, sizeof(g_snapshot.commandTopic), "server/cmd/%s/down", g_snapshot.deviceId);
    snprintf(g_snapshot.otaTopic, sizeof(g_snapshot.otaTopic), "server/ota/%s/notify", g_snapshot.deviceId);
    snprintf(g_snapshot.statusTopic, sizeof(g_snapshot.statusTopic), "esp32/%s/telemetry/status", g_snapshot.deviceId);
    snprintf(g_otaResultTopic, sizeof(g_otaResultTopic), "esp32/%s/ota/result", g_snapshot.deviceId);
    snprintf(g_otaPreflightAckTopic,
             sizeof(g_otaPreflightAckTopic),
             "esp32/%s/ota/preflight_ack",
             g_snapshot.deviceId);
    snprintf(g_deviceInfoTopic, sizeof(g_deviceInfoTopic), "esp32/%s/telemetry/device_info", g_snapshot.deviceId);
}

int publishRaw(const char* topic, const char* payload) {
    esp_mqtt_client_handle_t client = nullptr;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked() || g_client == nullptr || !g_snapshot.connected) {
            return -1;
        }
        client = g_client;
    }
    return esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
}

const char* cjsonString(cJSON* object, const char* name) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

void handleOtaNotify(const char* payload) {
    const esp_err_t err = AppIdfOta::handleNotify(payload);
    if (err != ESP_OK) {
        LOG_W(TAG_MQTT_IDF, "OTA notify handling failed: %s", esp_err_to_name(err));
    }
}

void handleCommandPayload(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') {
        return;
    }

    if (strcmp(payload, "beep") == 0) {
        LOG_I(TAG_MQTT_IDF, "Received beep command; local prompt tones stay disabled for MQTT");
        return;
    }

    cJSON* root = cJSON_Parse(payload);
    if (root != nullptr) {
        const char* type = cjsonString(root, "type");
        if (strcmp(type, "ota_preflight") == 0) {
            AppIdfOta::handlePreflightRequest(payload);
            cJSON_Delete(root);
            return;
        }
        cJSON_Delete(root);
    }

    AppIdfCommandExecutor::Result result;
    const esp_err_t err = AppIdfCommandExecutor::executeControlJson(payload, &result);
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        if (err == ESP_OK && result.success) {
            ++g_snapshot.commandOkCount;
        } else {
            ++g_snapshot.commandErrorCount;
        }
    }

    if (err == ESP_OK && result.success) {
        LOG_I(TAG_MQTT_IDF, "MQTT command executed: %s", result.summary[0] ? result.summary : "ok");
    } else if (result.handled) {
        LOG_W(TAG_MQTT_IDF, "MQTT command failed: %s", result.error[0] ? result.error : esp_err_to_name(err));
    } else {
        LOG_I(TAG_MQTT_IDF, "MQTT payload did not contain a migrated command");
    }
}

void processMessage(const char* topic, const char* payload) {
    if (topic == nullptr || payload == nullptr) {
        return;
    }

    char commandTopic[kTopicBufferLen];
    char otaTopic[kTopicBufferLen];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return;
        }
        ++g_snapshot.receivedCount;
        copyText(commandTopic, sizeof(commandTopic), g_snapshot.commandTopic);
        copyText(otaTopic, sizeof(otaTopic), g_snapshot.otaTopic);
    }

    if (strcmp(topic, otaTopic) == 0) {
        handleOtaNotify(payload);
        return;
    }
    if (strcmp(topic, commandTopic) == 0 || strcmp(topic, kBroadcastCommandTopic) == 0) {
        handleCommandPayload(payload);
        return;
    }

    LOG_W(TAG_MQTT_IDF, "Ignoring MQTT message on unexpected topic: %s", topic);
}

void resetRxBufferLocked() {
    g_rxTopic[0] = '\0';
    g_rxPayload[0] = '\0';
    g_rxPayloadLen = 0;
    g_rxOverflow = false;
}

void subscribeOnConnect(esp_mqtt_client_handle_t client) {
    char commandTopic[kTopicBufferLen];
    char otaTopic[kTopicBufferLen];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return;
        }
        copyText(commandTopic, sizeof(commandTopic), g_snapshot.commandTopic);
        copyText(otaTopic, sizeof(otaTopic), g_snapshot.otaTopic);
    }

    const int cmdId = esp_mqtt_client_subscribe(client, commandTopic, 0);
    const int otaId = esp_mqtt_client_subscribe(client, otaTopic, 0);
    const int broadcastId = esp_mqtt_client_subscribe(client, kBroadcastCommandTopic, 0);

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.subscribed = cmdId >= 0 && otaId >= 0 && broadcastId >= 0;
        g_snapshot.lastMessageId = broadcastId >= 0 ? broadcastId : (otaId >= 0 ? otaId : cmdId);
    }

    LOG_I(TAG_MQTT_IDF, "Subscribed MQTT topics cmd=%d ota=%d broadcast=%d", cmdId, otaId, broadcastId);
}

void handleMqttData(esp_mqtt_event_handle_t event) {
    if (event == nullptr || event->total_data_len <= 0 || event->data_len < 0 || event->current_data_offset < 0) {
        return;
    }

    bool ready = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return;
        }

        if (event->current_data_offset == 0) {
            resetRxBufferLocked();
            if (event->topic != nullptr && event->topic_len > 0) {
                const int topicLen = event->topic_len < static_cast<int>(sizeof(g_rxTopic) - 1)
                                         ? event->topic_len
                                         : static_cast<int>(sizeof(g_rxTopic) - 1);
                memcpy(g_rxTopic, event->topic, topicLen);
                g_rxTopic[topicLen] = '\0';
            }
            if (event->total_data_len > static_cast<int>(kMaxPayloadLen)) {
                g_rxOverflow = true;
            }
        }

        if (!g_rxOverflow && event->data != nullptr && event->data_len > 0) {
            const int remaining = static_cast<int>(kMaxPayloadLen) - g_rxPayloadLen;
            if (event->data_len > remaining) {
                g_rxOverflow = true;
            } else {
                memcpy(g_rxPayload + g_rxPayloadLen, event->data, event->data_len);
                g_rxPayloadLen += event->data_len;
                g_rxPayload[g_rxPayloadLen] = '\0';
            }
        }

        ready = event->current_data_offset + event->data_len >= event->total_data_len;
        if (ready && g_rxOverflow) {
            LOG_W(TAG_MQTT_IDF, "MQTT payload too large: %d bytes", event->total_data_len);
            resetRxBufferLocked();
            return;
        }
    }

    if (ready) {
        char topic[kTopicBufferLen];
        char payload[kMaxPayloadLen + 1];
        {
            MutexLock lock(pdMS_TO_TICKS(100));
            if (!lock.locked()) {
                return;
            }
            copyText(topic, sizeof(topic), g_rxTopic);
            copyText(payload, sizeof(payload), g_rxPayload);
            resetRxBufferLocked();
        }
        processMessage(topic, payload);
    }
}

void mqttEventHandler(void*, esp_event_base_t, int32_t eventId, void* eventData) {
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (eventId) {
        case MQTT_EVENT_CONNECTED:
            {
                MutexLock lock(pdMS_TO_TICKS(100));
                if (lock.locked()) {
                    g_snapshot.connected = true;
                    g_snapshot.subscribed = false;
                    g_snapshot.lastError = ESP_OK;
                    ++g_snapshot.connectCount;
                }
            }
            LOG_I(TAG_MQTT_IDF, "MQTT connected");
            if (event != nullptr) {
                subscribeOnConnect(event->client);
            }
            AppIdfOta::confirmRunningApp("mqtt_connected");
            publishStatus("online");
            publishDeviceInfo();
            break;
        case MQTT_EVENT_DISCONNECTED:
            {
                MutexLock lock(pdMS_TO_TICKS(100));
                if (lock.locked()) {
                    g_snapshot.connected = false;
                    g_snapshot.subscribed = false;
                    ++g_snapshot.disconnectCount;
                }
            }
            LOG_W(TAG_MQTT_IDF, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            handleMqttData(event);
            break;
        case MQTT_EVENT_ERROR:
            if (event != nullptr && event->error_handle != nullptr) {
                MutexLock lock(pdMS_TO_TICKS(100));
                if (lock.locked()) {
                    g_snapshot.lastError = event->error_handle->esp_tls_last_esp_err;
                    g_snapshot.lastSocketErrno = event->error_handle->esp_transport_sock_errno;
                }
            }
            LOG_W(TAG_MQTT_IDF, "MQTT error event");
            break;
        case MQTT_EVENT_PUBLISHED:
        case MQTT_EVENT_SUBSCRIBED:
            if (event != nullptr) {
                MutexLock lock(pdMS_TO_TICKS(100));
                if (lock.locked()) {
                    g_snapshot.lastMessageId = event->msg_id;
                }
            }
            break;
        default:
            break;
    }
}

esp_err_t createClientLocked() {
    refreshTopicsLocked();
    esp_mqtt_client_config_t config = {};
    config.broker.address.hostname = kBrokerHost;
    config.broker.address.port = kBrokerPort;
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    config.credentials.client_id = g_snapshot.clientId;
    config.credentials.username = kBrokerUser;
    config.credentials.authentication.password = kBrokerPass;
    config.session.keepalive = 60;
    config.network.reconnect_timeout_ms = 15000;
    config.network.timeout_ms = 5000;
    config.task.priority = 4;
    config.task.stack_size = 8192;
    config.buffer.size = 768;
    config.buffer.out_size = 768;
    config.outbox.limit = 1536;

    g_client = esp_mqtt_client_init(&config);
    if (g_client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return esp_mqtt_client_register_event(g_client, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
}

}  // namespace

esp_err_t start() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    esp_mqtt_client_handle_t client = nullptr;
    {
        MutexLock lock;
        if (!lock.locked()) {
            return setLastError(ESP_ERR_TIMEOUT);
        }
        if (g_snapshot.started) {
            return ESP_OK;
        }

        err = createClientLocked();
        if (err != ESP_OK) {
            if (g_client != nullptr) {
                esp_mqtt_client_destroy(g_client);
                g_client = nullptr;
            }
            return setLastError(err);
        }
        g_snapshot.started = true;
        client = g_client;
    }

    err = esp_mqtt_client_start(client);
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.clientStarted = err == ESP_OK;
        }
    }
    if (err == ESP_OK) {
        LOG_I(TAG_MQTT_IDF, "MQTT client started with device id %s", g_snapshot.deviceId);
    } else {
        LOG_E(TAG_MQTT_IDF, "MQTT client start failed: %s", esp_err_to_name(err));
    }
    return setLastError(err);
}

esp_err_t stop() {
    esp_mqtt_client_handle_t client = nullptr;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return setLastError(ESP_ERR_TIMEOUT);
        }
        client = g_client;
        g_client = nullptr;
        g_snapshot.started = false;
        g_snapshot.clientStarted = false;
        g_snapshot.connected = false;
        g_snapshot.subscribed = false;
    }
    if (client == nullptr) {
        return ESP_OK;
    }
    esp_mqtt_client_stop(client);
    return esp_mqtt_client_destroy(client);
}

bool isStarted() {
    return g_snapshot.started;
}

bool isConnected() {
    return g_snapshot.connected;
}

esp_err_t setDeviceIdentity(const char* identity) {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    char cleaned[kMaxDeviceIdLen + 1];
    if (!sanitizeDeviceId(identity, cleaned, sizeof(cleaned))) {
        return setLastError(ESP_ERR_INVALID_ARG);
    }

    bool restartClient = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return setLastError(ESP_ERR_TIMEOUT);
        }
        if (strcmp(g_snapshot.deviceId, cleaned) == 0) {
            return ESP_OK;
        }
        copyText(g_snapshot.deviceId, sizeof(g_snapshot.deviceId), cleaned);
        refreshTopicsLocked();
        restartClient = g_snapshot.started;
    }

    if (!restartClient) {
        return ESP_OK;
    }

    stop();
    return start();
}

const char* deviceId() {
    return g_snapshot.deviceId;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

esp_err_t publishStatus(const char* status) {
    char topic[kTopicBufferLen];
    char device[kMaxDeviceIdLen + 1];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        copyText(topic, sizeof(topic), g_snapshot.statusTopic);
        copyText(device, sizeof(device), g_snapshot.deviceId);
    }

    char payload[192];
    snprintf(payload,
             sizeof(payload),
             "{\"status\":\"%s\",\"imei\":\"%s\",\"fw_version\":\"%s\",\"idf\":true}",
             status != nullptr ? status : "online",
             device,
             firmwareVersion());
    return publishRaw(topic, payload) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t publishDeviceInfo() {
    char topic[kTopicBufferLen];
    char device[kMaxDeviceIdLen + 1];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        copyText(topic, sizeof(topic), g_deviceInfoTopic);
        copyText(device, sizeof(device), g_snapshot.deviceId);
    }

    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();
    const uint32_t uptimeSec = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int batteryPct = sensors.battery.valid ? sensors.battery.percent : -1;
    const float temperature = sensors.temperature.valid ? sensors.temperature.celsius : -99.0f;

    char payload[384];
    snprintf(payload,
             sizeof(payload),
             "{\"fw_version\":\"%s\",\"imei\":\"%s\",\"ai_count\":0,\"uptime_sec\":%u,"
             "\"free_heap\":%u,\"battery_pct\":%d,\"charging\":%s,"
             "\"largest_internal_block\":%u,\"temperature\":%.1f,\"idf\":true}",
             firmwareVersion(),
             device,
             static_cast<unsigned>(uptimeSec),
             static_cast<unsigned>(freeInternal),
             batteryPct,
             sensors.battery.charging ? "true" : "false",
             static_cast<unsigned>(largestInternal),
             temperature);
    return publishRaw(topic, payload) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t publishOtaPreflightAck(const char* requestId,
                                 const char* targetVersion,
                                 bool ok,
                                 const char* reason,
                                 bool recording,
                                 bool playing,
                                 bool otaBusy) {
    char topic[kTopicBufferLen];
    char device[kMaxDeviceIdLen + 1];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        copyText(topic, sizeof(topic), g_otaPreflightAckTopic);
        copyText(device, sizeof(device), g_snapshot.deviceId);
    }

    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int batteryPct = sensors.battery.valid ? sensors.battery.percent : -1;

    char payload[384];
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"ota_preflight_ack\",\"request_id\":\"%s\",\"device_id\":\"%s\","
             "\"fw_version\":\"%s\",\"target_version\":\"%s\",\"charging\":%s,"
             "\"battery_pct\":%d,\"free_heap\":%u,\"largest_internal_block\":%u,"
             "\"recording\":%s,\"playing\":%s,\"ota_busy\":%s,"
             "\"ok\":%s,\"reason\":\"%s\"}",
             requestId != nullptr ? requestId : "",
             device,
             firmwareVersion(),
             targetVersion != nullptr ? targetVersion : "",
             sensors.battery.charging ? "true" : "false",
             batteryPct,
             static_cast<unsigned>(freeInternal),
             static_cast<unsigned>(largestInternal),
             recording ? "true" : "false",
             playing ? "true" : "false",
             otaBusy ? "true" : "false",
             ok ? "true" : "false",
             reason != nullptr ? reason : "ready");
    return publishRaw(topic, payload) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t publishOtaResult(const char* status, const char* version, const char* requestId, const char* message) {
    char topic[kTopicBufferLen];
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        copyText(topic, sizeof(topic), g_otaResultTopic);
    }

    char payload[192];
    snprintf(payload,
             sizeof(payload),
             "{\"status\":\"%s\",\"version\":\"%s\",\"request_id\":\"%s\",\"message\":\"%s\"}",
             status != nullptr ? status : "unknown",
             version != nullptr ? version : "",
             requestId != nullptr ? requestId : "",
             message != nullptr ? message : "");
    return publishRaw(topic, payload) >= 0 ? ESP_OK : ESP_FAIL;
}

}  // namespace AppIdfMqtt
