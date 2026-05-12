#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace AppIdfIr {

constexpr size_t kMaxNameLen = 32;

enum class LearnStage : uint8_t {
    Unknown = 0,
    FirstCaptured = 1,
    Confirmed = 2,
    Mismatch = 3,
    SaveFailed = 4,
};

struct LearnEvent {
    bool learningSuccess;
    bool isAC;
    uint16_t rawLen;
    char signatureHash[32];
    LearnStage stage;
};

esp_err_t start();
bool isStarted();

bool isLearning();
const char* currentLearnName();
esp_err_t startLearning(const char* name);
void stopLearning();

bool sendLearned(const char* name, int repeatCount = 1);
esp_err_t clearAllLearned();
// 把已学红外条目名字写入 names 二维数组，返回写入数量。names[i] 是 \0 结尾的字符串。
size_t listLearnedNames(char names[][kMaxNameLen], size_t maxNames);

// UI/上层订阅学习事件的 queue（可能为 null 表示模块未启动）。
QueueHandle_t learnEventQueue();

uint32_t taskStackHighWatermark();

}  // namespace AppIdfIr
