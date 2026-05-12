#include "App_FlashGuard.h"

#include "App_Log.h"

#if !defined(ARDUINO)
#include "App_IdfWakeWord.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

static SemaphoreHandle_t s_flashGuardMutex = nullptr;
static volatile bool s_flashGuardActive = false;
static volatile bool s_restoringWakeWord = false;
#if !defined(ARDUINO)
static bool s_idfWakeWordWasRunning = false;
#endif

static constexpr const char* TAG_FLASH = "FLASH";

static void releaseGuard() {
    s_flashGuardActive = false;
    if (s_flashGuardMutex) {
        xSemaphoreGive(s_flashGuardMutex);
    }
}

#if !defined(ARDUINO)
static void pauseIdfWakeWordForFlash(const char* reason) {
    s_idfWakeWordWasRunning = AppIdfWakeWord::isRunning();
    if (!s_idfWakeWordWasRunning) {
        return;
    }

    LOG_I(TAG_FLASH, "[%s] Pausing IDF WakeWord for FLASH write", reason ? reason : "unknown");
    AppIdfWakeWord::pause();
    vTaskDelay(pdMS_TO_TICKS(80));
}

static void restoreIdfWakeWordAfterFlash() {
    if (!s_idfWakeWordWasRunning) {
        return;
    }

    s_idfWakeWordWasRunning = false;
    s_restoringWakeWord = true;
    const esp_err_t err = AppIdfWakeWord::resume();
    s_restoringWakeWord = false;
    if (err != ESP_OK) {
        LOG_W(TAG_FLASH, "Failed to restore IDF WakeWord after FLASH write: %s", esp_err_to_name(err));
    }
}
#endif

}  // namespace

void AppFlashGuard::init() {
    if (s_flashGuardMutex != nullptr) {
        return;
    }

    s_flashGuardMutex = xSemaphoreCreateMutex();
    if (!s_flashGuardMutex) {
        LOG_E(TAG_FLASH, "Failed to create FLASH write guard mutex");
    }
}

bool AppFlashGuard::begin(const char* reason, uint32_t playbackWaitMs) {
    init();
    if (!s_flashGuardMutex) {
        return false;
    }

    if (xSemaphoreTake(s_flashGuardMutex, pdMS_TO_TICKS(playbackWaitMs > 0 ? playbackWaitMs : 5000)) != pdTRUE) {
        LOG_W(TAG_FLASH, "[%s] Timed out while acquiring FLASH write guard",
              reason ? reason : "unknown");
        return false;
    }

    s_flashGuardActive = true;
#if !defined(ARDUINO)
    pauseIdfWakeWordForFlash(reason);
#endif

    LOG_I(TAG_FLASH, "[%s] FLASH write guard acquired", reason ? reason : "unknown");
    return true;
}

void AppFlashGuard::end() {
    if (!s_flashGuardMutex || !s_flashGuardActive) {
        return;
    }

    releaseGuard();
#if !defined(ARDUINO)
    restoreIdfWakeWordAfterFlash();
#endif
    LOG_I(TAG_FLASH, "FLASH write guard released");
}

bool AppFlashGuard::isActive() {
    return s_flashGuardActive;
}

bool AppFlashGuard::isRestoringWakeWord() {
    return s_restoringWakeWord;
}
