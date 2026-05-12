#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfAudio {

constexpr uint32_t kSampleRateHz = 16000;
constexpr uint8_t kEs8311Address = 0x18;

struct Snapshot {
    bool started = false;
    bool codecFound = false;
    bool micEnabled = false;
    bool paEnabled = false;
    uint32_t sampleRateHz = kSampleRateHz;
    uint8_t volume = 70;
    uint8_t micGain = 0;
    uint8_t chipId1 = 0;
    uint8_t chipId2 = 0;
    uint8_t chipVersion = 0;
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
esp_err_t restart(const char* reason = nullptr);
bool isStarted();
bool isCodecFound();

Snapshot snapshot();

esp_err_t setVolume(uint8_t volume);
uint8_t getVolume();
esp_err_t setMicGain(uint8_t gain);
uint8_t getMicGain();
esp_err_t enableMicChannel();
esp_err_t disableMicChannel();
bool isMicChannelEnabled();

esp_err_t setPaEnabled(bool enabled);
bool isPaEnabled();

esp_err_t writePcm(const void* data, size_t len, size_t* bytesWritten, uint32_t timeoutMs = 1000);
esp_err_t readPcm(void* data, size_t len, size_t* bytesRead, uint32_t timeoutMs = 1000);
esp_err_t writeSilence(uint32_t durationMs);
esp_err_t playSineTest(uint32_t durationMs = 3000, uint32_t frequencyHz = 1000);
esp_err_t playGeneratedCue(const char* cueName, uint32_t timeoutMs = 2000);
esp_err_t playLocalCue(const char* cueName, uint32_t timeoutMs = 8000);

esp_err_t scanI2c(uint8_t* addresses, size_t maxAddresses, size_t* count);

}  // namespace AppIdfAudio
