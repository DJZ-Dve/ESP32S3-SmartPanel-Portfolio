#ifndef BATTERY_CONFIG_H
#define BATTERY_CONFIG_H

#include <stdint.h>

#include "Pin_Config.h"

#ifdef __cplusplus

namespace BatteryConfig {

// 电压阈值（CCV 即带载电压，已考虑 LCD/WiFi/4G 瞬态压降 150-250mV 余量）
// 锂电池 3.0V 是放电终止电压，低于此点会损伤电芯。线性映射 (V-3.00)/(4.20-3.00)*100。
constexpr float kEarlyShutdownVoltage = 3.20f;     // ≈17%：冷启动门控，进裸屏闪烁
constexpr float kWarnEnterVoltage = 3.30f;         // ≈25%：进入语音 cue + 状态栏图标
constexpr float kWarnExitVoltage = 3.36f;          // ≈30%：退出 WARN
constexpr float kCountdownEnterVoltage = 3.18f;    // ≈15%：30s 倒计时强制关机
constexpr float kCountdownExitVoltage = 3.24f;     // ≈20%：撤窗（滞回 +5%）
constexpr float kEmergencyVoltage = 3.10f;         // ≈8%：跳过倒计时立即 deep sleep

// 紧急档要求连续 N 个采样周期都 ≤ kEmergencyVoltage 才触发，避免瞬态毛刺。
constexpr uint8_t kEmergencyConsecutiveSamples = 2;

// 倒计时时长（秒）。30s 是行业惯例，给用户找充电器的时间。
constexpr int kLowBatteryCountdownSec = 30;

// 倒计时被 OTA/Recorder busy 阻塞的最长等待。超过则升级为 EMERGENCY 路径。
constexpr uint32_t kCountdownDeferTimeoutMs = 90000;

// 早期裸屏闪烁画面参数
constexpr uint32_t kEarlyShutdownDisplayMs = 6000;  // 总显示时长
constexpr uint32_t kEarlyShutdownBlinkMs = 500;     // 闪烁半周期
constexpr uint32_t kEarlyShutdownPollMs = 200;      // GPIO37 充电检测轮询周期

// Deep sleep 唤醒策略：
// ESP32-S3 ext1 唤醒源只支持 RTC IO（GPIO 0-21），而 PIN_CHARGE_DETECT=GPIO37
// 不在 RTC 域，无法直接做唤醒源。改用 timer wakeup 周期苏醒一次，
// 重新走 app_main 早期门控判断充电状态，未插电则再次 deep sleep。
constexpr uint64_t kDeepSleepTimerUs = 60ULL * 1000ULL * 1000ULL;  // 60s 周期唤醒检测

}  // namespace BatteryConfig

#endif  // __cplusplus

#endif  // BATTERY_CONFIG_H
