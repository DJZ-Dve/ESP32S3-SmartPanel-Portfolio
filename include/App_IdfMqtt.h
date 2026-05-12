#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfMqtt {

constexpr size_t kMaxDeviceIdLen = 19;
constexpr size_t kTopicBufferLen = 80;

struct Snapshot {
    bool started = false;
    bool clientStarted = false;
    bool connected = false;
    bool subscribed = false;
    char deviceId[kMaxDeviceIdLen + 1] = {};
    char clientId[32] = {};
    char commandTopic[kTopicBufferLen] = {};
    char otaTopic[kTopicBufferLen] = {};
    char statusTopic[kTopicBufferLen] = {};
    uint32_t connectCount = 0;
    uint32_t disconnectCount = 0;
    uint32_t receivedCount = 0;
    uint32_t commandOkCount = 0;
    uint32_t commandErrorCount = 0;
    int lastMessageId = 0;
    int lastSocketErrno = 0;
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
esp_err_t stop();
bool isStarted();
bool isConnected();

esp_err_t setDeviceIdentity(const char* identity);
const char* deviceId();
Snapshot snapshot();

esp_err_t publishStatus(const char* status = "online");
esp_err_t publishDeviceInfo();
esp_err_t publishOtaPreflightAck(const char* requestId,
                                 const char* targetVersion,
                                 bool ok,
                                 const char* reason,
                                 bool recording,
                                 bool playing,
                                 bool otaBusy);
esp_err_t publishOtaResult(const char* status, const char* version, const char* requestId, const char* message);

}  // namespace AppIdfMqtt
