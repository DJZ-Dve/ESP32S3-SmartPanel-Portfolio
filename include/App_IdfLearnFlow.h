#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfLearnFlow {

enum class State : uint8_t {
    Idle = 0,
    Capturing,        // 已下发 IR/RF433 学习指令，等待 learnEvent
    AwaitingLabel,    // 信号已捕获，等待用户长按 KEY2 录音说出标签
    UploadingLabel,   // 录音上传中
};

enum class CaptureKind : uint8_t {
    None  = 0,
    IR    = 1,
    RF433 = 2,
};

esp_err_t start();

bool isActive();         // 非 Idle
State currentState();
CaptureKind captureKind();
// IR 学习已捕获第一次按键、等待第二次相同按键确认时返回 true。
// 仅在 Capturing + IR 时有意义；其它状态返回 false。
bool irFirstStageCaptured();

// IR 上一次第二次按键与第一次不一致、流程已回退到等第一次状态时返回 true。
// 用户按下新一次按键（FirstCaptured 事件）后清除；非 IR / Capturing 状态返回 false。
bool irMismatchPending();

// 提供给 UI 屏读：捕获 hash（IR 是 signatureHash，RF433 是 code 字面量）。
const char* hashHex();
// 仅 IR 时有意义；其它情况返回 0。
uint16_t lastIrRawLen();

// 启动学习捕获：按当前 AppIdfAppMode 自动选 IR / RF433。
// 仅 IR 或 RF433 模式可用；BLE 模式返回 ESP_ERR_INVALID_STATE。
esp_err_t startCapture();

// 用户取消（长按 KEY1 / 切屏）。释放暂存。
esp_err_t cancel();

// MCU executor 收到 local_label_v1 时调用。落盘并返回 IDLE。
esp_err_t onLabelArrived(const char* label);

// recorder 上传失败时由 executor / server.cpp 调用，回退 AwaitingLabel 让用户重录。
esp_err_t onUploadFailed();

// META JSON 用：仅在 AwaitingLabel/UploadingLabel 状态写入 pending_signal 对象到 outBuf，
// 输出格式：{"kind":"ir","hash":"...","raw_len":145}
// 非 awaiting 状态写入空字符串并返回 0。
size_t describePendingForMeta(char* outBuf, size_t cap);

// 倒计时剩余毫秒（CAPTURING/AwaitingLabel）；非 active 返回 0。
uint32_t remainingMs();

}  // namespace AppIdfLearnFlow
