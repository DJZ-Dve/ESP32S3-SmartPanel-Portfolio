#include "App_IdfAppMode.h"

#include "App_FlashGuard.h"
#include "App_Log.h"

#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace AppIdfAppMode {
namespace {

constexpr const char* TAG_APPMODE = "IDF_APPMODE";
constexpr const char* kNamespace = "appmode";
constexpr const char* kKeyMode = "mode";

Mode g_currentMode = kDefaultMode;
bool g_initialized = false;

Mode sanitize(uint8_t raw) {
    switch (raw) {
        case static_cast<uint8_t>(Mode::BLE):
        case static_cast<uint8_t>(Mode::IR):
        case static_cast<uint8_t>(Mode::RF433):
            return static_cast<Mode>(raw);
        default:
            return kDefaultMode;
    }
}

}  // namespace

void init() {
    if (g_initialized) {
        return;
    }

    uint8_t stored = static_cast<uint8_t>(kDefaultMode);
    nvs_handle_t handle = 0;
    const esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        const esp_err_t getErr = nvs_get_u8(handle, kKeyMode, &stored);
        if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG_APPMODE, "Read app mode failed: %s", esp_err_to_name(getErr));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        LOG_W(TAG_APPMODE, "Open app mode namespace failed: %s", esp_err_to_name(err));
    }

    g_currentMode = sanitize(stored);
    g_initialized = true;
    LOG_I(TAG_APPMODE, "App mode loaded: %s", nameAscii(g_currentMode));
}

Mode current() {
    if (!g_initialized) {
        init();
    }
    return g_currentMode;
}

bool isBle() { return current() == Mode::BLE; }
bool isIr() { return current() == Mode::IR; }
bool isRf433() { return current() == Mode::RF433; }

const char* nameAscii(Mode mode) {
    switch (mode) {
        case Mode::BLE:
            return "ble";
        case Mode::IR:
            return "ir";
        case Mode::RF433:
            return "rf433";
    }
    return "unknown";
}

const char* nameCn(Mode mode) {
    switch (mode) {
        case Mode::BLE:
            return "蓝牙空调";
        case Mode::IR:
            return "红外";
        case Mode::RF433:
            return "射频433";
    }
    return "未知";
}

esp_err_t persist(Mode mode) {
    ScopedFlashGuard flashGuard("IDF app mode save", 5000);
    if (!flashGuard.ok()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_W(TAG_APPMODE, "Open app mode for write failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, kKeyMode, static_cast<uint8_t>(mode));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        LOG_W(TAG_APPMODE, "Persist app mode failed: %s", esp_err_to_name(err));
        return err;
    }

    g_currentMode = mode;
    LOG_I(TAG_APPMODE, "App mode persisted: %s", nameAscii(mode));
    return ESP_OK;
}

esp_err_t switchAndRestart(Mode mode, uint32_t delayMs) {
    const esp_err_t err = persist(mode);
    if (err != ESP_OK) {
        return err;
    }

    LOG_I(TAG_APPMODE, "Switching to %s, restarting in %u ms", nameAscii(mode),
          static_cast<unsigned>(delayMs));
    if (delayMs > 0) {
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }
    esp_restart();
    return ESP_OK;
}

}  // namespace AppIdfAppMode
