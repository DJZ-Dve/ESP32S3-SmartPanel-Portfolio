#ifndef APP_IDF_UI_H
#define APP_IDF_UI_H

#include <stdint.h>

#include "App_IdfInput.h"
#include "esp_err.h"

namespace AppIdfUi {

enum class AiStatus {
    Ack,
    Listening,
    Thinking,
    Executing,
    Speaking,
    Success,
    Error,
};

void preloadThemePreference();
esp_err_t start();
bool isStarted();
void handleKeyEvent(const AppIdfInput::KeyEvent& event);
const char* activeScreenName();
uint32_t handledKeyCount();
const char* currentThemeName();
esp_err_t setThemeLight(bool light, bool persist);
esp_err_t showAiStatus(AiStatus status, bool trackRecorder, uint32_t autoExitDelayMs = 0);

// 切到「场景学习」专用屏。由 AI executor 在收到「进入场景学习」语音命令后调用。
// 仅在 LearnFlow 已成功 startCapture（IR/RF433 模式）时切屏；BLE 模式被拒。
esp_err_t showLearningScreen();

esp_err_t applyPowerSaveEnter();
esp_err_t applyPowerSaveExit();

// 低电量倒计时弹窗。LVGL 任务安全：内部走 runLocked 投递。
// seconds 为剩余秒数；每次调用都会刷新 label 文本（首次调用会创建 popup）。
// seconds 可以为 0（显示"即将关机"）。
esp_err_t showLowBatteryCountdown(int seconds);
// 隐藏并销毁低电量倒计时弹窗。
void hideLowBatteryCountdown();

}  // namespace AppIdfUi

#endif  // APP_IDF_UI_H
