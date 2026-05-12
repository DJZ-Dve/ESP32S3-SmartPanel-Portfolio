#include "App_IdfTasks.h"

#include <limits.h>
#include <string.h>

#include "App_Log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

namespace AppIdfTasks {
namespace {

constexpr const char* TAG_TASKS = "IDF_TASK";

void releaseTaskMemory(StaticTaskMemory* taskMemory) {
    if (taskMemory == nullptr) {
        return;
    }

    if (taskMemory->stack != nullptr) {
        heap_caps_free(taskMemory->stack);
    }
    if (taskMemory->tcb != nullptr) {
        heap_caps_free(taskMemory->tcb);
    }

    *taskMemory = {};
}

}  // namespace

esp_err_t initTaskWatchdog(uint32_t timeoutMs, bool triggerPanic) {
    const esp_task_wdt_config_t config = {
        .timeout_ms = timeoutMs,
        .idle_core_mask = 0,
        .trigger_panic = triggerPanic,
    };

    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_reconfigure(&config);
    }

    if (err == ESP_OK) {
        LOG_I(TAG_TASKS, "Task watchdog enabled timeout=%u ms triggerPanic=%d", static_cast<unsigned>(timeoutMs),
              triggerPanic ? 1 : 0);
    } else {
        LOG_W(TAG_TASKS, "Task watchdog init failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t registerCurrentTaskWatchdog(const char* taskName) {
    const esp_err_t status = esp_task_wdt_status(nullptr);
    if (status == ESP_OK) {
        return ESP_OK;
    }
    if (status != ESP_ERR_INVALID_STATE && status != ESP_ERR_NOT_FOUND) {
        LOG_W(TAG_TASKS, "Task watchdog status failed (%s): %s", taskName ? taskName : "unknown",
              esp_err_to_name(status));
    }

    const esp_err_t err = esp_task_wdt_add(nullptr);
    if (err == ESP_OK) {
        LOG_I(TAG_TASKS, "Task watchdog registered for %s", taskName ? taskName : "unknown");
    } else {
        LOG_W(TAG_TASKS, "Task watchdog register failed (%s): %s", taskName ? taskName : "unknown",
              esp_err_to_name(err));
    }
    return err;
}

void unregisterCurrentTaskWatchdog() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_delete(nullptr);
    }
}

void feedCurrentTaskWatchdog() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

esp_err_t createPinnedToCoreInternal(TaskFunction_t taskCode,
                                     const char* taskName,
                                     uint32_t stackDepthWords,
                                     void* taskParameter,
                                     UBaseType_t priority,
                                     BaseType_t coreId,
                                     StaticTaskMemory* taskMemory) {
    if (taskCode == nullptr || taskName == nullptr || taskMemory == nullptr || stackDepthWords == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (taskMemory->handle != nullptr || taskMemory->stack != nullptr || taskMemory->tcb != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stackDepthWords > (UINT32_MAX / sizeof(StackType_t))) {
        return ESP_ERR_INVALID_SIZE;
    }

    taskMemory->stack = static_cast<StackType_t*>(
        heap_caps_malloc(stackDepthWords * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    taskMemory->tcb =
        static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    taskMemory->stackDepthWords = stackDepthWords;

    if (taskMemory->stack == nullptr || taskMemory->tcb == nullptr) {
        LOG_E(TAG_TASKS,
              "Failed to allocate internal SRAM task memory for %s stackWords=%u",
              taskName,
              static_cast<unsigned>(stackDepthWords));
        releaseTaskMemory(taskMemory);
        return ESP_ERR_NO_MEM;
    }

    memset(taskMemory->stack, 0, stackDepthWords * sizeof(StackType_t));
    memset(taskMemory->tcb, 0, sizeof(StaticTask_t));

    taskMemory->handle =
        xTaskCreateStaticPinnedToCore(taskCode, taskName, stackDepthWords, taskParameter, priority, taskMemory->stack,
                                      taskMemory->tcb, coreId);
    if (taskMemory->handle == nullptr) {
        LOG_E(TAG_TASKS, "Failed to create internal SRAM task %s", taskName);
        releaseTaskMemory(taskMemory);
        return ESP_FAIL;
    }

    LOG_I(TAG_TASKS,
          "Created task %s core=%d priority=%u stack=%u bytes in internal SRAM",
          taskName,
          static_cast<int>(coreId),
          static_cast<unsigned>(priority),
          static_cast<unsigned>(stackDepthWords * sizeof(StackType_t)));
    return ESP_OK;
}

void logTaskMemory(const StaticTaskMemory& taskMemory, const char* taskName) {
    if (taskMemory.handle == nullptr) {
        LOG_W(TAG_TASKS, "%s task not created", taskName ? taskName : "unknown");
        return;
    }

    LOG_I(TAG_TASKS,
          "%s stack watermark=%u stackSize=%u bytes",
          taskName ? taskName : "unknown",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(taskMemory.handle)),
          static_cast<unsigned>(taskMemory.stackDepthWords * sizeof(StackType_t)));
}

}  // namespace AppIdfTasks
