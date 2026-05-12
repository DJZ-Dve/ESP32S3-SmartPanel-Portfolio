#ifndef POWER_CONFIG_H
#define POWER_CONFIG_H

#include "esp_wifi_types.h"

namespace AppPowerConfig {

constexpr bool kPowerSaveEnable = true;

constexpr uint32_t kIdleTimeoutMs = 60000;

constexpr uint32_t kStatusTimerSlowPeriodMs = 5000;
constexpr uint32_t kStatusTimerFastPeriodMs = 1000;

constexpr wifi_ps_type_t kWifiPsModeL1 = WIFI_PS_MIN_MODEM;
constexpr wifi_ps_type_t kWifiPsModeL0 = WIFI_PS_NONE;

// L1 期间 CPU 主频降档（80/160/240 之间，APB 始终 80 MHz，外设时序不变）。
// L0 恢复 240 MHz。
constexpr int kCpuFreqMhzL1 = 80;
constexpr int kCpuFreqMhzL0 = 240;

constexpr bool kConsumeFirstKey = true;

constexpr uint32_t kLvglLockTimeoutMs = 200;

constexpr uint32_t kTickPeriodMs = 1000;

}  // namespace AppPowerConfig

#endif  // POWER_CONFIG_H
