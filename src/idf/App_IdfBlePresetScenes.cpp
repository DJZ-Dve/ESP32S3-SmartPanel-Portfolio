#include "App_IdfBlePresetScenes.h"

#include <string.h>

#include "App_IdfBleAircon.h"
#include "App_IdfScene.h"
#include "App_Log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfBlePresetScenes {
namespace {

constexpr const char* TAG = "BLE_PRESET";
constexpr TickType_t kStepDelayTicks = pdMS_TO_TICKS(120);

enum class ActionType : uint8_t {
    PowerOn,
    PowerOff,
    EcoMode,
    Temperature,   // arg = 摄氏度
    FanSpeed,      // arg = 1..5
    LightOn,
    LightOff,
};

struct Action {
    ActionType type;
    uint8_t arg;
};

struct Preset {
    uint8_t id;          // 0xE0..0xEF
    const char* label;
    Action acts[4];
    uint8_t actCount;
};

// ROM 表：用户已确认 5 条。改顺序会改变 UI 列表呈现顺序。
const Preset kPresets[] = {
    { 0xE0, "开空调", { {ActionType::PowerOn, 0} }, 1 },
    { 0xE1, "关空调", { {ActionType::PowerOff, 0} }, 1 },
    { 0xE2, "节能模式",
      { {ActionType::PowerOn, 0},
        {ActionType::EcoMode, 0},
        {ActionType::Temperature, 26},
        {ActionType::FanSpeed, 3} },
      4 },
    { 0xE3, "开灯", { {ActionType::LightOn, 0} }, 1 },
    { 0xE4, "关灯", { {ActionType::LightOff, 0} }, 1 },
};

constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

const Preset* findPreset(uint8_t id) {
    for (size_t i = 0; i < kPresetCount; i++) {
        if (kPresets[i].id == id) return &kPresets[i];
    }
    return nullptr;
}

void fillSceneItem(const Preset& p, AppIdfScene::SceneItem& out) {
    memset(&out, 0, sizeof(out));
    out.id = p.id;
    out.type = AppIdfScene::SignalType::BLE;
    out.presetId = p.id;
    strncpy(out.label, p.label, sizeof(out.label) - 1);
}

esp_err_t runAction(const Action& a) {
    switch (a.type) {
        case ActionType::PowerOn:     return AppIdfBleAircon::powerOn();
        case ActionType::PowerOff:    return AppIdfBleAircon::powerOff();
        case ActionType::EcoMode:     return AppIdfBleAircon::setEcoMode();
        case ActionType::Temperature: return AppIdfBleAircon::setTemperature(a.arg);
        case ActionType::FanSpeed:    return AppIdfBleAircon::setFanSpeed(a.arg);
        case ActionType::LightOn:     return AppIdfBleAircon::setLightOn(true);
        case ActionType::LightOff:    return AppIdfBleAircon::setLightOn(false);
    }
    return ESP_ERR_INVALID_ARG;
}

}  // namespace

size_t listAll(AppIdfScene::SceneItem* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    const size_t n = (cap < kPresetCount) ? cap : kPresetCount;
    for (size_t i = 0; i < n; i++) {
        fillSceneItem(kPresets[i], out[i]);
    }
    return n;
}

bool getById(uint8_t id, AppIdfScene::SceneItem& out) {
    const Preset* p = findPreset(id);
    if (p == nullptr) return false;
    fillSceneItem(*p, out);
    return true;
}

esp_err_t executePresetById(uint16_t presetId) {
    if (presetId > 0xFF) return ESP_ERR_INVALID_ARG;
    const Preset* p = findPreset(static_cast<uint8_t>(presetId));
    if (p == nullptr) {
        LOG_W(TAG, "preset %u not found", static_cast<unsigned>(presetId));
        return ESP_ERR_NOT_FOUND;
    }
    if (!AppIdfBleAircon::isStarted()) {
        LOG_W(TAG, "BLE aircon not started; preset \"%s\" skipped", p->label);
        return ESP_ERR_INVALID_STATE;
    }
    LOG_I(TAG, "preset \"%s\" steps=%u", p->label, p->actCount);
    for (uint8_t i = 0; i < p->actCount; i++) {
        const esp_err_t err = runAction(p->acts[i]);
        if (err != ESP_OK) {
            LOG_E(TAG, "preset \"%s\" step %u failed: %s", p->label, i, esp_err_to_name(err));
            return err;
        }
        if (i + 1 < p->actCount) {
            vTaskDelay(kStepDelayTicks);
        }
    }
    return ESP_OK;
}

}  // namespace AppIdfBlePresetScenes
