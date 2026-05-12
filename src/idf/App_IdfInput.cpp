#include "App_IdfInput.h"

#include "App_IdfAdc.h"
#include "App_IdfPowerSave.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "Pin_Config.h"
#include "Power_Config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfInput {
namespace {

constexpr const char* TAG_INPUT = "IDF_INPUT";
constexpr uint32_t kInputTaskStackWords = 4096;
constexpr uint32_t kScanPeriodMs = 10;
constexpr uint32_t kDebounceMs = 25;
constexpr uint32_t kLongPressMs = 800;

AppIdfTasks::StaticTaskMemory g_inputTaskMemory;
KeyEventCallback g_callback = nullptr;
KeyEvent g_lastEvent;
KeyId g_activeKeyId = KeyId::None;
int g_lastRaw = 0;
int g_lastMillivolts = 0;
uint32_t g_pressStartMs = 0;
uint32_t g_lastDebounceMs = 0;
bool g_lastButtonReading = false;
bool g_buttonState = false;
bool g_longPressHandled = false;
bool g_started = false;

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

KeyId keyIdFromMillivolts(int mv) {
#ifdef ADC_KEYS_CONFIGURED
    if (mv >= ADC_KEY_BOTH_MIN && mv <= ADC_KEY_BOTH_MAX) {
        return KeyId::Both;
    }
    if (mv >= ADC_KEY2_MIN && mv <= ADC_KEY2_MAX) {
        return KeyId::Key2;
    }
    if (mv >= ADC_KEY1_MIN && mv <= ADC_KEY1_MAX) {
        return KeyId::Key1;
    }
#endif
    return KeyId::None;
}

esp_err_t readAdc(int* raw, int* millivolts) {
    if (raw == nullptr || millivolts == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    AppIdfAdc::Sample sample;
    const esp_err_t err = AppIdfAdc::readGpioMillivolts(PIN_ADC_KEY, &sample);
    *raw = sample.raw;
    *millivolts = sample.millivolts;
    return err;
}

void publishEvent(KeyAction action, KeyId keyId, int raw, int millivolts) {
    if (action == KeyAction::None || action == KeyAction::LongPressHold) {
        return;
    }

    g_lastEvent = {
        .action = action,
        .keyId = keyId,
        .raw = raw,
        .millivolts = millivolts,
    };

    LOG_D(TAG_INPUT,
          "key event action=%s key=%s raw=%d mv=%d",
          keyActionName(action),
          keyIdName(keyId),
          raw,
          millivolts);

    AppIdfPowerSave::notifyActivity();
    const bool wokeFromL1 = AppIdfPowerSave::isL1();
    if (wokeFromL1) {
        (void)AppIdfPowerSave::exitL1();
        if (AppPowerConfig::kConsumeFirstKey) {
            return;
        }
    }

    if (g_callback != nullptr) {
        g_callback(g_lastEvent);
    }
}

void scanOnce() {
    int raw = 0;
    int millivolts = 0;
    const esp_err_t err = readAdc(&raw, &millivolts);
    if (err != ESP_OK) {
        LOG_W(TAG_INPUT, "ADC read failed: %s", esp_err_to_name(err));
        return;
    }

    g_lastRaw = raw;
    g_lastMillivolts = millivolts;

    const KeyId currentKey = keyIdFromMillivolts(millivolts);
    const bool reading = currentKey != KeyId::None;
    const uint32_t now = nowMs();
    KeyAction action = KeyAction::None;
    KeyId eventKey = g_activeKeyId;

    if (reading != g_lastButtonReading) {
        g_lastDebounceMs = now;
    }

    if ((now - g_lastDebounceMs) > kDebounceMs && reading != g_buttonState) {
        g_buttonState = reading;
        if (g_buttonState) {
            g_activeKeyId = currentKey;
            eventKey = g_activeKeyId;
            g_pressStartMs = now;
            g_longPressHandled = false;
            action = KeyAction::Down;
        } else {
            eventKey = g_activeKeyId;
            if (g_longPressHandled) {
                action = KeyAction::LongPressEnd;
            } else if ((now - g_pressStartMs) > 10) {
                action = KeyAction::ShortPress;
            }
        }
    }

    if (g_buttonState && (now - g_pressStartMs) > kLongPressMs) {
        eventKey = g_activeKeyId;
        if (!g_longPressHandled) {
            action = KeyAction::LongPressStart;
            g_longPressHandled = true;
        } else if (action == KeyAction::None) {
            action = KeyAction::LongPressHold;
        }
    }

    publishEvent(action, eventKey, raw, millivolts);

    if (!g_buttonState && action != KeyAction::None) {
        g_activeKeyId = KeyId::None;
    }
    g_lastButtonReading = reading;
}

void inputTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Input");

    while (true) {
        scanOnce();
        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(kScanPeriodMs));
    }
}

}  // namespace

esp_err_t start(KeyEventCallback callback) {
    if (g_started) {
        g_callback = callback;
        return ESP_OK;
    }

    g_callback = callback;

    esp_err_t err = AppIdfAdc::init();
    if (err != ESP_OK) {
        return err;
    }

    err = AppIdfTasks::createPinnedToCoreInternal(inputTask, "IDF_Input", kInputTaskStackWords, nullptr, 2, 1,
                                                  &g_inputTaskMemory);
    if (err != ESP_OK) {
        LOG_E(TAG_INPUT, "Input task creation failed: %s", esp_err_to_name(err));
        return err;
    }

    g_started = true;
    LOG_I(TAG_INPUT, "ADC key input started on GPIO%d", PIN_ADC_KEY);
    return ESP_OK;
}

bool isStarted() {
    return g_started;
}

KeyId activeKeyId() {
    return g_activeKeyId;
}

KeyEvent lastEvent() {
    return g_lastEvent;
}

int lastRaw() {
    return g_lastRaw;
}

int lastMillivolts() {
    return g_lastMillivolts;
}

uint32_t taskStackHighWatermark() {
    if (g_inputTaskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_inputTaskMemory.handle);
}

const char* keyActionName(KeyAction action) {
    switch (action) {
        case KeyAction::None:
            return "none";
        case KeyAction::Down:
            return "down";
        case KeyAction::ShortPress:
            return "short";
        case KeyAction::LongPressStart:
            return "long_start";
        case KeyAction::LongPressHold:
            return "long_hold";
        case KeyAction::LongPressEnd:
            return "long_end";
        default:
            return "unknown";
    }
}

const char* keyIdName(KeyId keyId) {
    switch (keyId) {
        case KeyId::None:
            return "none";
        case KeyId::Key1:
            return "key1";
        case KeyId::Key2:
            return "key2";
        case KeyId::Both:
            return "both";
        default:
            return "unknown";
    }
}

}  // namespace AppIdfInput
