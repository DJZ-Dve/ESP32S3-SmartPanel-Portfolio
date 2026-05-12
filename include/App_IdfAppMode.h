#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfAppMode {

enum class Mode : uint8_t {
    BLE = 0,
    IR = 1,
    RF433 = 2,
};

constexpr Mode kDefaultMode = Mode::BLE;

// 启动早期由 app_main 调用一次，从 NVS 读出当前模式并缓存。
void init();

Mode current();

bool isBle();
bool isIr();
bool isRf433();

const char* nameAscii(Mode mode);
const char* nameCn(Mode mode);

// 仅写 NVS，不重启。返回 ESP_OK 表示 NVS commit 成功。
esp_err_t persist(Mode mode);

// 持久化目标模式后延迟 delayMs 触发 esp_restart。delayMs 给上层 toast 显示余地。
// 该函数返回 ESP_OK 后，调用方应停止后续动作并等待重启。
esp_err_t switchAndRestart(Mode mode, uint32_t delayMs);

}  // namespace AppIdfAppMode
