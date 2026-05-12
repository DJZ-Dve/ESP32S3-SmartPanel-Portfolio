#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfBleAircon {

constexpr size_t kMaxPairScanResults = 10;

enum class PairingState : uint8_t {
    Idle = 0,
    Scanning,
    Ready,
    Pairing,
    Success,
    Error,
};

struct PairScanEntry {
    char name[24] = {};
    char address[18] = {};
    int8_t rssi = 0;
    uint8_t addressType = 0;
    bool connectable = false;
    bool hasService = false;
};

esp_err_t start();
bool isStarted();
bool isStackReady();
const char* getLastError();

bool hasStoredTarget();
bool hasActiveConnection();
const char* getTargetAddressString();
uint8_t getTargetAddressType();
bool isDisconnectAfterCommandEnabled();
void setDisconnectAfterCommand(bool enabled);

// FFE1 echo-based application ACK verification. When enabled (default),
// every successful FFE2 write must be echoed back on FFE1 within ~800 ms,
// otherwise the command is reported as failed.
bool isAckVerificationEnabled();
void setAckVerificationEnabled(bool enabled);
bool isNotifySubscribed();

esp_err_t powerOn();
esp_err_t powerOff();
esp_err_t setCoolingMode();
esp_err_t setVentMode();
esp_err_t setEcoMode();
esp_err_t setSleepMode();
esp_err_t setTemperature(uint8_t tempC);
esp_err_t setFanSpeed(uint8_t level);
esp_err_t setDisplayOn(bool on);
esp_err_t setLightOn(bool on);
esp_err_t setSwingHorizontal();
esp_err_t setSwingVertical();

esp_err_t startPairingScan();
esp_err_t startPairing(size_t index);
void cancelPairing();
esp_err_t releaseConnection();

PairingState getPairingState();
const char* pairingStateName(PairingState state);
size_t getPairingResultCount();
bool getPairingResult(size_t index, PairScanEntry* out);

uint32_t workerTaskStackHighWatermark();

}  // namespace AppIdfBleAircon
