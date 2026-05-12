#ifndef APP_IDF_SENSORS_H
#define APP_IDF_SENSORS_H

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfSensors {

struct BatterySnapshot {
    bool valid = false;
    bool charging = false;
    int raw = 0;
    int adcMillivolts = 0;
    int calibratedAdcMillivolts = 0;
    float voltage = 0.0f;
    int percent = -1;
    esp_err_t lastError = ESP_ERR_INVALID_STATE;
};

// 低电量状态机当前档位。NORMAL → WARN25 → COUNTDOWN15 → EMERGENCY8。
// 充电插入 / 电压回升过滞回门限会向上回退。
enum class PowerState : uint8_t {
    Normal = 0,
    Warn25,        // ≤25%：常驻语音 cue + 状态栏图标
    Countdown15,   // ≤15%：30s 倒计时弹窗
    Emergency8,    // ≤8%：跳过倒计时直接 deep sleep
};

struct TemperatureSnapshot {
    bool valid = false;
    int raw = 0;
    int millivolts = 0;
    float celsius = -99.0f;
    esp_err_t lastError = ESP_ERR_INVALID_STATE;
};

struct Snapshot {
    bool valid = false;
    uint32_t sampleCount = 0;
    uint32_t updatedMs = 0;
    BatterySnapshot battery;
    TemperatureSnapshot temperature;
};

esp_err_t start();
bool isStarted();
Snapshot latest();
uint32_t taskStackHighWatermark();

// 低电量状态机查询。
PowerState currentPowerState();
// 倒计时剩余秒数；非 COUNTDOWN_15 状态返回 -1。
int countdownSecondsRemaining();

// 调试入口：覆写滤波后电压（单位 V），用于不放电也能测试整套阈值。
// voltage <= 0 表示清除覆写、恢复正常 ADC 读数。
void debugInjectVoltage(float voltage);
// 调试入口：覆写充电检测结果，模拟插拔。-1 表示清除覆写。
void debugInjectCharging(int chargingOrNeg);

}  // namespace AppIdfSensors

#endif  // APP_IDF_SENSORS_H
