#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

namespace AppIdfNetwork {

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;
constexpr size_t kMaxIpStringLen = 16;
constexpr size_t kMaxPortalSsidLen = 16;
constexpr size_t kMaxScanResults = 12;

struct Credentials {
    char ssid[kMaxSsidLen + 1] = {};
    char password[kMaxPasswordLen + 1] = {};
};

struct ScanResult {
    char ssid[kMaxSsidLen + 1] = {};
    int8_t rssi = -127;
    wifi_auth_mode_t authMode = WIFI_AUTH_OPEN;
    uint8_t channel = 0;
    bool secure = false;
};

struct Snapshot {
    bool initialized = false;
    bool wifiStarted = false;
    bool credentialsLoaded = false;
    bool portalActive = false;
    bool connecting = false;
    bool connected = false;
    char ssid[kMaxSsidLen + 1] = {};
    char ip[kMaxIpStringLen] = "0.0.0.0";
    char portalSsid[kMaxPortalSsidLen] = {};
    char portalIp[kMaxIpStringLen] = "0.0.0.0";
    char portalHint[32] = {};
    int8_t rssi = -127;
    uint8_t channel = 0;
    uint8_t disconnectReason = 0;
    uint8_t wifiFailCount = 0;
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
bool isInitialized();
bool isConnected();
Snapshot snapshot();

esp_err_t loadCredentials(Credentials* credentials);
esp_err_t saveCredentials(const char* ssid, const char* password);
esp_err_t clearCredentials();
bool hasStoredCredentials();

esp_err_t connectStored();
esp_err_t connect(const char* ssid, const char* password, bool saveToNvs);
esp_err_t disconnect(bool stopWifi = false);
esp_err_t setDefaultRoute();

esp_err_t scan(ScanResult* results, size_t maxResults, size_t* count);

esp_err_t startPortal();
esp_err_t stopPortal();
bool isPortalActive();
uint32_t dnsTaskStackHighWatermark();

const char* authModeName(wifi_auth_mode_t authMode);

}  // namespace AppIdfNetwork
