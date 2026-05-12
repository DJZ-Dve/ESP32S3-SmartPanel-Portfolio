#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfScene {

constexpr size_t kMaxScenes = 20;
constexpr size_t kMaxLabelLen = 48;
constexpr size_t kMaxIrNameLen = 32;

enum class SignalType : uint8_t {
    IR = 0,
    RF433 = 1,
    BLE = 2,   // BLE 预制：仅内存合并，不进 scenes.json
};

struct SceneItem {
    uint8_t id;
    SignalType type;
    char label[kMaxLabelLen];           // 持久条目：AI 返回；BLE 预制：ROM 字面量
    char irName[kMaxIrNameLen];         // type=IR 用
    uint64_t code433;                   // type=RF433 用
    uint8_t len433;
    uint16_t T433;
    uint16_t presetId;                  // type=BLE 用：索引 BLE 预制 ROM 表
};

esp_err_t start();
bool isStarted();

// 仅持久化条目计数（不含 BLE 预制）。
size_t count();

esp_err_t addSceneIr(const char* label, const char* irName);
esp_err_t addSceneRf433(const char* label, uint64_t code, uint8_t len, uint16_t T);
esp_err_t removeById(uint8_t id);
esp_err_t clearAll();

bool getById(uint8_t id, SceneItem* out);
bool getByIndex(size_t index, SceneItem* out);

// 按当前 app mode 执行：
//   id 落在 BLE 预制范围（0xE0..0xEF）→ 调 BLE 预制执行器
//   否则按 SceneItem.type 派发到 IR / RF433
//   type 与当前模式不匹配返回 ESP_ERR_INVALID_STATE。
esp_err_t executeById(uint8_t id);

// 按当前 app mode 过滤：BLE → BLE 预制；IR/RF433 → 同 type 持久条目。
// 返回写入 out 的条目数；最多 cap 条。
size_t listForCurrentMode(SceneItem* out, size_t cap);

// 组装 META JSON 中 scene_labels 数组：BLE 模式返回 "[]"；IR/RF433 模式
// 返回当前模式下持久条目的 [{"id":..,"label":..}, ...]。
// 返回写入字节数（不含末尾 NUL）；缓冲不够时返回 0 并写空数组。
size_t labelsForServerMeta(char* outBuf, size_t outBufSize);

}  // namespace AppIdfScene
