#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfScene {
struct SceneItem;
}

namespace AppIdfBlePresetScenes {

// 预制条目占用的 SceneItem.id 段：0xE0..0xEF（持久化条目永远 < 0xE0）。
constexpr uint8_t kIdBase = 0xE0;

// 列出所有 BLE 预制（按 ROM 顺序），写入 out，最多 cap 条；返回写入数。
size_t listAll(AppIdfScene::SceneItem* out, size_t cap);

// 按 id（0xE0..0xEF）查 BLE 预制；不存在返回 false。
bool getById(uint8_t id, AppIdfScene::SceneItem& out);

// 按 SceneItem.presetId（0xE0..0xEF）执行：依次调用 AppIdfBleAircon::*，每步约 120ms 间隔。
// 任一步失败立刻中止并返回错误。
esp_err_t executePresetById(uint16_t presetId);

}  // namespace AppIdfBlePresetScenes
