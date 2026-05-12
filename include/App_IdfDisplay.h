#ifndef APP_IDF_DISPLAY_H
#define APP_IDF_DISPLAY_H

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfDisplay {

constexpr int kScreenWidth = 240;
constexpr int kScreenHeight = 240;

esp_err_t init();
bool isInitialized();
bool isBacklightOn();
esp_err_t setBacklight(bool enabled);
esp_err_t drawRgb565Bitmap(int xStart, int yStart, int xEnd, int yEnd, const uint16_t* pixels);
esp_err_t waitForPendingTransfers();
esp_err_t drawBootProbe();

// 低电量裸屏画面：黑底 + 红色电池图标（程序化绘制，不依赖 LVGL/字体）。
// iconVisible 控制图标显示或全黑，用于闪烁效果。
esp_err_t drawLowBatteryFrame(bool iconVisible);

// 早期开机门控的闭环：闪烁 6 秒、200ms 轮询充电检测 GPIO。
// 检测到充电立刻 return ESP_OK，调用方可继续正常 boot；
// 6 秒到 → 关背光 → enter deep sleep（不返回）。
esp_err_t runLowBatteryShutdownScreen();

// 进入 deep sleep。封装 timer wakeup 配置 + 背光/RST 关闭。不返回。
[[noreturn]] void enterDeepSleepForLowBattery(const char* reason);

}  // namespace AppIdfDisplay

#endif  // APP_IDF_DISPLAY_H
