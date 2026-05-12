#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfServer {

struct Snapshot {
    bool started = false;
    bool lastProbeOk = false;
    bool lastResponseJsonOk = false;
    uint32_t connectCount = 0;
    uint32_t responseCount = 0;
    char host[48] = {};
    uint16_t port = 0;
    char lastSummary[160] = {};
    char lastError[96] = {};
    esp_err_t lastEspError = ESP_OK;
};

esp_err_t start(const char* host, uint16_t port);
bool isStarted();
Snapshot snapshot();

esp_err_t probe();
// outCuePlayed: 当 executor 已经播过 cue(op_failed/settings_done/not_understood 等)
// 时置 true,避免上层(recorder)把 executor 失败当成网络失败再播一次 network_failed。
esp_err_t uploadPcmAndReceive(const int16_t* pcm, size_t sampleCount, bool* outCuePlayed = nullptr);

}  // namespace AppIdfServer
