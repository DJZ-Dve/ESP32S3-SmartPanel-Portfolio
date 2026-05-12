#ifndef APP_IDF_SYSTEM_H
#define APP_IDF_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfSystem {

struct HeapSnapshot {
    size_t internalFree = 0;
    size_t internalLargest = 0;
    size_t internalMinimumFree = 0;
    size_t psramFree = 0;
    size_t psramLargest = 0;
    size_t psramMinimumFree = 0;
};

esp_err_t initNvs();

HeapSnapshot getHeapSnapshot();
void logHeapSnapshot(const char* phase);
void logAppDescription();
void logChipInfo();
void logKeyPartitions();
void logTaskWatermark(TaskHandle_t taskHandle, const char* taskName);

}  // namespace AppIdfSystem

#endif
