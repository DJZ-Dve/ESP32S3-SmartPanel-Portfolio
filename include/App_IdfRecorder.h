#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfRecorder {

constexpr size_t kMaxRecordBytes = 512 * 1024;

struct Snapshot {
    bool started = false;
    bool startPending = false;
    bool recording = false;
    bool uploading = false;
    bool lastUploadOk = false;
    uint32_t recordedBytes = 0;
    uint32_t maxRecordBytes = kMaxRecordBytes;
    uint32_t droppedBytes = 0;
    uint32_t durationMs = 0;
    uint32_t uploadCount = 0;
    uint32_t startCount = 0;
    uint32_t lastRms = 0;  // 最近一次 PCM 块的 RMS（int16 域，~0..32768），录音停止后清零
    char lastSummary[160] = {};
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
bool isStarted();
bool isBusy();
bool isRecordingActive();
Snapshot snapshot();

esp_err_t startRecording();
esp_err_t stopRecordingAndUpload();
esp_err_t cancelRecording();
esp_err_t uploadLastRecording();
esp_err_t startWakeInteraction();

uint32_t taskStackHighWatermark();

}  // namespace AppIdfRecorder
