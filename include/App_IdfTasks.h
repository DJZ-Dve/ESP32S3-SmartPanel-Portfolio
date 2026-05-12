#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfTasks {

struct StaticTaskMemory {
    StackType_t* stack = nullptr;
    StaticTask_t* tcb = nullptr;
    TaskHandle_t handle = nullptr;
    uint32_t stackDepthWords = 0;
};

esp_err_t initTaskWatchdog(uint32_t timeoutMs, bool triggerPanic);
esp_err_t registerCurrentTaskWatchdog(const char* taskName);
void unregisterCurrentTaskWatchdog();
void feedCurrentTaskWatchdog();

esp_err_t createPinnedToCoreInternal(TaskFunction_t taskCode,
                                     const char* taskName,
                                     uint32_t stackDepthWords,
                                     void* taskParameter,
                                     UBaseType_t priority,
                                     BaseType_t coreId,
                                     StaticTaskMemory* taskMemory);

void logTaskMemory(const StaticTaskMemory& taskMemory, const char* taskName);

}  // namespace AppIdfTasks
