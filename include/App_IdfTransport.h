#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfTransport {

enum class NetMode : uint8_t {
    AUTO,
    WIFI_ONLY,
    CELLULAR_ONLY,
};

enum class ActiveTransport : uint8_t {
    NONE,
    WIFI,
    PPP_4G,
};

enum class CellularStage : uint8_t {
    IDLE,
    PPP,
    MQTT,
};

struct Snapshot {
    bool started = false;
    NetMode mode = NetMode::AUTO;
    ActiveTransport active = ActiveTransport::NONE;
    ActiveTransport pending = ActiveTransport::NONE;
    CellularStage cellularStage = CellularStage::IDLE;
    bool autoCellularSuppressed = false;
    bool switchingBlocked = false;
    bool cellularCancelRequested = false;
    uint32_t wifiLostMs = 0;
    uint32_t cellularAttemptMs = 0;
    uint32_t mqttAttemptMs = 0;
    uint32_t switchCount = 0;
    char lastReason[64] = "boot";
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
bool isStarted();

esp_err_t requestMode(NetMode mode);
esp_err_t requestActive(ActiveTransport transport, const char* reason = nullptr);
esp_err_t cancelCellularAttempt();

NetMode currentMode();
ActiveTransport activeTransport();
Snapshot snapshot();
uint32_t taskStackHighWatermark();

const char* netModeName(NetMode mode);
const char* activeTransportName(ActiveTransport transport);
const char* cellularStageName(CellularStage stage);

}  // namespace AppIdfTransport
