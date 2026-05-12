#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfCellular {

constexpr size_t kMaxApnLen = 32;
constexpr size_t kMaxImeiLen = 16;
constexpr size_t kMaxIpStringLen = 16;
constexpr size_t kMaxCellularErrorLen = 64;

struct Snapshot {
    bool started = false;
    bool powered = false;
    bool uartReady = false;
    bool dialing = false;
    bool pppRunning = false;
    bool connected = false;
    char apn[kMaxApnLen] = "cmnet";
    char imei[kMaxImeiLen] = {};
    char ip[kMaxIpStringLen] = "0.0.0.0";
    char lastError[kMaxCellularErrorLen] = "none";
    int csq = 0;
    int pppError = 0;
    uint8_t pppPhase = 0;
    uint32_t dialAttempts = 0;
    uint32_t rxBytes = 0;
    uint32_t txBytes = 0;
    esp_err_t lastEspError = ESP_OK;
};

esp_err_t start();
esp_err_t powerOn();
esp_err_t powerOff();
esp_err_t dial(const char* apn = "cmnet", uint32_t timeoutMs = 90000);
esp_err_t hangup();
esp_err_t setDefaultRoute();
esp_err_t refreshSignal();

// Diagnostic helpers (only safe when PPP is not running and the dial loop is idle).
esp_err_t sendRawAt(const char* command,
                    char* response,
                    size_t responseSize,
                    uint32_t timeoutMs = 2000);
esp_err_t dumpUartRx(char* buffer, size_t bufferSize, uint32_t durationMs);

// PWRKEY (GPIO46) helpers. Hardware has a pull-down for auto-boot; these are for manually
// driving the line during bring-up debugging.
esp_err_t pulsePowerKey(int level, uint32_t durationMs);
esp_err_t setPowerKeyLevel(int level);
int readPowerLevel();
int readPowerKeyLevel();

bool isStarted();
bool isPowered();
bool isConnected();
bool isReadyForMqtt();
Snapshot snapshot();
uint32_t taskStackHighWatermark();

}  // namespace AppIdfCellular
