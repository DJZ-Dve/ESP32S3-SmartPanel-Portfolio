#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfOta {

struct Snapshot {
    bool started = false;
    bool busy = false;
    bool pending = false;
    bool rollbackPending = false;
    int progress = -1;
    char version[16] = {};
    char requestId[40] = {};
    char lastReason[64] = {};
    uint32_t expectedSize = 0;
    uint32_t writtenSize = 0;
    esp_err_t lastError = ESP_OK;
};

esp_err_t start();
bool isStarted();
bool isBusy();
Snapshot snapshot();
uint32_t taskStackHighWatermark();

esp_err_t handlePreflightRequest(const char* json);
esp_err_t handleNotify(const char* json);
esp_err_t confirmRunningApp(const char* reason);

// 紧急中止信号：set 后下载循环在下一个 chunk 边界退出。
// 用于低电量紧急关机路径——deep sleep 前调用，避免在 flash 写入中途断电。
// 调用后 isBusy() 可能仍短暂为 true，等下一个循环 tick 才转 false。
void cancel(const char* reason);

}  // namespace AppIdfOta
