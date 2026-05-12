#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfWakeWord {

struct Snapshot {
    bool initialized = false;
    bool running = false;
    bool paused = false;
    bool micEnabled = false;
    bool feedTaskResident = false;
    bool fetchTaskResident = false;
    uint32_t wakeCount = 0;
    uint32_t feedCount = 0;
    uint32_t fetchCount = 0;
    uint32_t lastRms = 0;
    uint32_t lastWakeMs = 0;
    int feedChunkSamples = 0;
    int fetchChunkSamples = 0;
    int feedChannels = 0;
    int fetchChannels = 0;
    int modelCount = 0;
    char wakeModelName[64] = {};
    char wakeWords[96] = {};
    esp_err_t lastError = ESP_OK;
};

esp_err_t init();
esp_err_t start();
void stop();
void pause();
esp_err_t resume();

bool isRunning();
bool hasResidentTasks();
Snapshot snapshot();
uint32_t feedTaskStackHighWatermark();
uint32_t fetchTaskStackHighWatermark();

}  // namespace AppIdfWakeWord
