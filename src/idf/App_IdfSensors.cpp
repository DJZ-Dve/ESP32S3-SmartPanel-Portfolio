#include "App_IdfSensors.h"

#include <math.h>
#include <stddef.h>

#include "App_FlashGuard.h"
#include "App_IdfAdc.h"
#include "App_IdfAudio.h"
#include "App_IdfDisplay.h"
#include "App_IdfOta.h"
#include "App_IdfRecorder.h"
#include "App_IdfTasks.h"
#include "App_IdfUi.h"
#include "App_IdfWakeWord.h"
#include "App_Log.h"
#include "Battery_Config.h"
#include "Pin_Config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

namespace AppIdfSensors {
namespace {

constexpr const char* TAG_SENSORS = "IDF_SENSORS";
constexpr uint32_t kSensorsTaskStackWords = 4096;
constexpr uint32_t kSensorsSamplePeriodMs = 1000;

constexpr int kBatteryAdcSampleCount = 8;
constexpr float kBatteryDividerScale = 11.0f;
constexpr uint8_t kBatteryMaxInvalidSampleWindows = 3;
constexpr int kChargeDetectSampleCount = 7;
constexpr uint32_t kChargeDetectSampleDelayUs = 400;

constexpr float kTempR7 = 8200.0f;
constexpr float kTempR48 = 200000.0f;
constexpr float kTempVccMv = 3300.0f;
constexpr float kNtcR0 = 100000.0f;
constexpr float kNtcB = 3950.0f;
constexpr float kNtcT0 = 298.15f;

struct BatteryCalibrationPoint {
    float raw;
    float voltage;
};

constexpr BatteryCalibrationPoint kBatteryCalibration[] = {
    {287.6667f, 3.00f},
    {324.6667f, 3.30f},
    {360.3333f, 3.60f},
    {396.3333f, 3.90f},
    {426.6667f, 4.20f},
};

struct AveragedAdcSample {
    int raw = 0;
    int millivolts = 0;
};

AppIdfTasks::StaticTaskMemory g_sensorsTaskMemory;
portMUX_TYPE g_snapshotMux = portMUX_INITIALIZER_UNLOCKED;
Snapshot g_latest;
bool g_started = false;

bool g_batteryFilterInitialized = false;
float g_batteryFilteredVoltage = 0.0f;
uint32_t g_batteryLastSampleMs = 0;
uint8_t g_batteryInvalidSampleWindows = 0;
int g_batteryDisplayedPercent = -1;
uint32_t g_batteryDisplayedPercentLastStepMs = 0;
bool g_batteryChargingLatched = false;
uint32_t g_batteryChargeStartMs = 0;

// 低电量状态机
PowerState g_powerState = PowerState::Normal;
uint8_t g_emergencyConsecutive = 0;
int g_countdownSecondsRemaining = -1;     // -1 = 倒计时未启动 / 已停
uint32_t g_countdownDeferStartMs = 0;     // 因 OTA/Recorder busy 推迟启动的起点
bool g_chargingLastSnapshot = false;
bool g_cueAlreadyPlayedThisVisit = false;

// 调试覆写
bool g_debugVoltageOverride = false;
float g_debugVoltageValue = 0.0f;
int g_debugChargingOverride = -1;

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

float interpolateBatterySegment(float raw, const BatteryCalibrationPoint& low, const BatteryCalibrationPoint& high) {
    const float span = high.raw - low.raw;
    if (span <= 0.01f) {
        return low.voltage;
    }
    return low.voltage + (raw - low.raw) * (high.voltage - low.voltage) / span;
}

float convertBatteryRawToVoltage(int raw) {
    if (raw <= 0) {
        return 0.0f;
    }

    const float rawValue = static_cast<float>(raw);
    constexpr size_t pointCount = sizeof(kBatteryCalibration) / sizeof(kBatteryCalibration[0]);

    if (rawValue <= kBatteryCalibration[0].raw) {
        const float voltage = interpolateBatterySegment(rawValue, kBatteryCalibration[0], kBatteryCalibration[1]);
        return voltage > 0.0f ? voltage : 0.0f;
    }

    for (size_t i = 1; i < pointCount; ++i) {
        if (rawValue <= kBatteryCalibration[i].raw) {
            return interpolateBatterySegment(rawValue, kBatteryCalibration[i - 1], kBatteryCalibration[i]);
        }
    }

    return interpolateBatterySegment(rawValue, kBatteryCalibration[pointCount - 2], kBatteryCalibration[pointCount - 1]);
}

int convertBatteryVoltageToPercent(float voltage) {
    constexpr float kFullVoltage = 4.20f;
    constexpr float kEmptyVoltage = 3.00f;

    if (voltage <= 0.1f) {
        return -1;
    }
    if (voltage <= kEmptyVoltage + 0.05f) {
        return 0;
    }
    if (voltage >= kFullVoltage - 0.03f) {
        return 100;
    }

    int percent = static_cast<int>(lroundf(((voltage - kEmptyVoltage) / (kFullVoltage - kEmptyVoltage)) * 100.0f));
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return percent;
}

bool readChargeDetectStable() {
    int highSamples = 0;
    for (int i = 0; i < kChargeDetectSampleCount; ++i) {
        if (gpio_get_level(static_cast<gpio_num_t>(PIN_CHARGE_DETECT)) != 0) {
            ++highSamples;
        }
        esp_rom_delay_us(kChargeDetectSampleDelayUs);
    }
    return highSamples >= (kChargeDetectSampleCount / 2 + 1);
}

esp_err_t readAveragedAdc(int gpio, int sampleCount, uint32_t delayUs, AveragedAdcSample* averaged) {
    if (averaged == nullptr || sampleCount <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int rawSamples[kBatteryAdcSampleCount] = {};
    int mvSamples[kBatteryAdcSampleCount] = {};
    int validCount = 0;
    esp_err_t lastErr = ESP_OK;

    for (int i = 0; i < sampleCount; ++i) {
        AppIdfAdc::Sample sample;
        const esp_err_t err = AppIdfAdc::readGpioMillivolts(gpio, &sample);
        if (err != ESP_OK) {
            lastErr = err;
        } else if (sample.raw > 0 && validCount < kBatteryAdcSampleCount) {
            rawSamples[validCount] = sample.raw;
            mvSamples[validCount] = sample.millivolts;
            ++validCount;
        }
        esp_rom_delay_us(delayUs);
    }

    if (validCount == 0) {
        return lastErr == ESP_OK ? ESP_FAIL : lastErr;
    }

    int minIndex = 0;
    int maxIndex = 0;
    int sumRaw = 0;
    int sumMv = 0;
    for (int i = 0; i < validCount; ++i) {
        sumRaw += rawSamples[i];
        sumMv += mvSamples[i];
        if (rawSamples[i] < rawSamples[minIndex]) {
            minIndex = i;
        }
        if (rawSamples[i] > rawSamples[maxIndex]) {
            maxIndex = i;
        }
    }

    int divisor = validCount;
    if (validCount >= 4) {
        sumRaw -= rawSamples[minIndex];
        sumMv -= mvSamples[minIndex];
        --divisor;
        if (maxIndex != minIndex) {
            sumRaw -= rawSamples[maxIndex];
            sumMv -= mvSamples[maxIndex];
            --divisor;
        }
    }

    averaged->raw = (sumRaw + divisor / 2) / divisor;
    averaged->millivolts = (sumMv + divisor / 2) / divisor;
    return ESP_OK;
}

float updateFilteredBatteryVoltage(float rawVoltage, uint32_t now) {
    if (rawVoltage <= 0.1f) {
        if (g_batteryInvalidSampleWindows < UINT8_MAX) {
            ++g_batteryInvalidSampleWindows;
        }
        if (g_batteryFilterInitialized && g_batteryInvalidSampleWindows < kBatteryMaxInvalidSampleWindows) {
            return g_batteryFilteredVoltage;
        }

        g_batteryFilterInitialized = false;
        g_batteryDisplayedPercent = -1;
        g_batteryDisplayedPercentLastStepMs = 0;
        g_batteryChargingLatched = false;
        g_batteryChargeStartMs = 0;
        return 0.0f;
    }

    g_batteryInvalidSampleWindows = 0;
    if (!g_batteryFilterInitialized || g_batteryLastSampleMs == 0 || (now - g_batteryLastSampleMs > 15000)) {
        g_batteryFilteredVoltage = rawVoltage;
        g_batteryFilterInitialized = true;
    } else {
        constexpr float kAlpha = 0.20f;
        constexpr float kMaxStepPerUpdate = 0.03f;
        float candidate = g_batteryFilteredVoltage + (rawVoltage - g_batteryFilteredVoltage) * kAlpha;
        float delta = candidate - g_batteryFilteredVoltage;
        if (delta > kMaxStepPerUpdate) {
            delta = kMaxStepPerUpdate;
        }
        if (delta < -kMaxStepPerUpdate) {
            delta = -kMaxStepPerUpdate;
        }
        g_batteryFilteredVoltage += delta;
    }

    g_batteryLastSampleMs = now;
    return g_batteryFilteredVoltage;
}

int updateDisplayedBatteryPercent(int percent, bool charging, uint32_t now) {
    if (percent < 0) {
        return percent;
    }

    if (charging) {
        if (!g_batteryChargingLatched) {
            g_batteryChargingLatched = true;
            g_batteryChargeStartMs = now;
        }
    } else {
        g_batteryChargingLatched = false;
        g_batteryChargeStartMs = 0;
    }

    if (g_batteryDisplayedPercent < 0) {
        g_batteryDisplayedPercent = percent;
        g_batteryDisplayedPercentLastStepMs = now;
        return g_batteryDisplayedPercent;
    }

    constexpr uint32_t kNormalDisplayStepMs = 1000;
    constexpr uint32_t kChargingSurfaceHoldMs = 120000;
    constexpr uint32_t kChargingDisplayRiseStepMs = 15000;
    constexpr uint32_t kChargingDisplayFallStepMs = 3000;
    const uint32_t elapsed =
        (g_batteryDisplayedPercentLastStepMs == 0) ? UINT32_MAX : (now - g_batteryDisplayedPercentLastStepMs);
    bool allowIncrease = true;
    uint32_t increaseStepMs = kNormalDisplayStepMs;
    uint32_t decreaseStepMs = kNormalDisplayStepMs;

    if (charging) {
        allowIncrease = g_batteryChargeStartMs > 0 && (now - g_batteryChargeStartMs >= kChargingSurfaceHoldMs);
        increaseStepMs = kChargingDisplayRiseStepMs;
        decreaseStepMs = kChargingDisplayFallStepMs;
    }

    if (percent >= g_batteryDisplayedPercent + 2) {
        if (allowIncrease && elapsed >= increaseStepMs) {
            ++g_batteryDisplayedPercent;
            g_batteryDisplayedPercentLastStepMs = now;
        }
    } else if (percent <= g_batteryDisplayedPercent - 2) {
        if (elapsed >= decreaseStepMs) {
            --g_batteryDisplayedPercent;
            g_batteryDisplayedPercentLastStepMs = now;
        }
    }

    return g_batteryDisplayedPercent;
}

esp_err_t sampleBattery(BatterySnapshot* battery, uint32_t now) {
    if (battery == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    AveragedAdcSample sample;
    const esp_err_t err = readAveragedAdc(PIN_ADC_BAT, kBatteryAdcSampleCount, 250, &sample);
    battery->lastError = err;
    battery->charging = readChargeDetectStable();
    if (err != ESP_OK) {
        return err;
    }

    const float rawVoltage = convertBatteryRawToVoltage(sample.raw);
    const float filteredVoltage = updateFilteredBatteryVoltage(rawVoltage, now);
    const int rawPercent = convertBatteryVoltageToPercent(filteredVoltage);
    const int displayedPercent = updateDisplayedBatteryPercent(rawPercent, battery->charging, now);

    battery->raw = sample.raw;
    battery->adcMillivolts = sample.millivolts;
    battery->voltage = filteredVoltage;
    battery->percent = displayedPercent;
    battery->calibratedAdcMillivolts =
        filteredVoltage > 0.0f ? static_cast<int>(lroundf((filteredVoltage * 1000.0f) / kBatteryDividerScale)) : 0;
    battery->valid = displayedPercent >= 0 && filteredVoltage > 0.1f;
    battery->lastError = ESP_OK;
    return ESP_OK;
}

float convertTempMillivoltsToCelsius(int millivolts) {
    if (millivolts < 100 || millivolts > 3200) {
        return -99.0f;
    }

    const float rTotal = (kTempVccMv * kTempR48) / static_cast<float>(millivolts);
    const float rNtc = rTotal - kTempR7 - kTempR48;
    if (rNtc <= 0.0f) {
        return 0.0f;
    }

    const float lnRatio = logf(rNtc / kNtcR0);
    const float kelvin = 1.0f / ((1.0f / kNtcT0) + (lnRatio / kNtcB));
    return kelvin - 273.15f;
}

esp_err_t sampleTemperature(TemperatureSnapshot* temperature) {
    if (temperature == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    AppIdfAdc::Sample sample;
    const esp_err_t err = AppIdfAdc::readGpioMillivolts(PIN_ADC_TEMP, &sample);
    temperature->lastError = err;
    if (err != ESP_OK) {
        return err;
    }

    temperature->raw = sample.raw;
    temperature->millivolts = sample.millivolts;
    temperature->celsius = convertTempMillivoltsToCelsius(sample.millivolts);
    temperature->valid = temperature->celsius > 0.0f && isfinite(temperature->celsius);
    temperature->lastError = ESP_OK;
    return ESP_OK;
}

void publishSnapshot(const Snapshot& snapshot) {
    portENTER_CRITICAL(&g_snapshotMux);
    g_latest = snapshot;
    portEXIT_CRITICAL(&g_snapshotMux);
}

const char* powerStateName(PowerState s) {
    switch (s) {
        case PowerState::Normal:      return "NORMAL";
        case PowerState::Warn25:      return "WARN_25";
        case PowerState::Countdown15: return "COUNTDOWN_15";
        case PowerState::Emergency8:  return "EMERGENCY_8";
    }
    return "?";
}

void playLowBatteryCueOnce() {
    if (g_cueAlreadyPlayedThisVisit) {
        return;
    }
    if (AppFlashGuard::isActive() || AppIdfRecorder::isBusy()) {
        LOG_D(TAG_SENSORS, "Skipping low battery cue while audio-critical path is busy");
        return;
    }

    const bool wakeWasRunning = AppIdfWakeWord::isRunning();
    if (wakeWasRunning) {
        AppIdfWakeWord::pause();
    }
    const esp_err_t err = AppIdfAudio::playGeneratedCue("low_battery", 2500);
    if (wakeWasRunning) {
        const esp_err_t resumeErr = AppIdfWakeWord::resume();
        if (resumeErr != ESP_OK) {
            LOG_W(TAG_SENSORS, "WakeWord resume after low battery cue failed: %s", esp_err_to_name(resumeErr));
        }
    }
    if (err != ESP_OK) {
        LOG_W(TAG_SENSORS, "Low battery cue failed: %s", esp_err_to_name(err));
    }
    g_cueAlreadyPlayedThisVisit = true;
}

void transitionTo(PowerState next, const char* reason) {
    if (next == g_powerState) {
        return;
    }
    LOG_W(TAG_SENSORS, "PowerState %s -> %s (%s)", powerStateName(g_powerState), powerStateName(next),
          reason != nullptr ? reason : "");
    // 离开 COUNTDOWN_15 时撤窗
    if (g_powerState == PowerState::Countdown15 && next != PowerState::Emergency8) {
        AppIdfUi::hideLowBatteryCountdown();
        g_countdownSecondsRemaining = -1;
    }
    // 进入 NORMAL/WARN 时允许下次 WARN 再播一次 cue
    if (next == PowerState::Normal) {
        g_cueAlreadyPlayedThisVisit = false;
    }
    g_powerState = next;
}

[[noreturn]] void performEmergencyShutdown(const char* reason) {
    LOG_E(TAG_SENSORS, "Emergency low-battery shutdown: %s", reason != nullptr ? reason : "voltage");
    AppIdfOta::cancel("emergency_low_battery");
    AppIdfWakeWord::pause();
    AppIdfUi::hideLowBatteryCountdown();
    AppIdfDisplay::enterDeepSleepForLowBattery(reason != nullptr ? reason : "emergency");
}

void tryStartCountdownLocked(uint32_t nowMs) {
    if (g_countdownSecondsRemaining > 0) {
        return;  // 已在跑
    }
    const bool busy = AppIdfOta::isBusy() || AppFlashGuard::isActive() || AppIdfRecorder::isBusy();
    if (busy) {
        if (g_countdownDeferStartMs == 0) {
            g_countdownDeferStartMs = nowMs;
        } else if (nowMs - g_countdownDeferStartMs > BatteryConfig::kCountdownDeferTimeoutMs) {
            LOG_E(TAG_SENSORS, "Countdown deferred too long, escalating to emergency");
            performEmergencyShutdown("defer_timeout");
        }
        return;
    }
    g_countdownDeferStartMs = 0;
    g_countdownSecondsRemaining = BatteryConfig::kLowBatteryCountdownSec;
    const esp_err_t err = AppIdfUi::showLowBatteryCountdown(g_countdownSecondsRemaining);
    if (err != ESP_OK) {
        LOG_W(TAG_SENSORS, "showLowBatteryCountdown failed: %s", esp_err_to_name(err));
    }
}

void tickCountdown(uint32_t nowMs) {
    if (g_countdownSecondsRemaining < 0) {
        tryStartCountdownLocked(nowMs);
        return;
    }
    --g_countdownSecondsRemaining;
    (void)AppIdfUi::showLowBatteryCountdown(g_countdownSecondsRemaining);
    if (g_countdownSecondsRemaining <= 0) {
        LOG_E(TAG_SENSORS, "Countdown reached zero, shutting down");
        AppIdfWakeWord::pause();
        AppIdfDisplay::enterDeepSleepForLowBattery("countdown");
    }
}

void updatePowerStateMachine(const BatterySnapshot& battery, uint32_t nowMs) {
    if (!battery.valid) {
        return;
    }

    // 紧急档要求连续 N 个采样周期都 ≤ kEmergencyVoltage
    if (battery.voltage > 0.0f && battery.voltage <= BatteryConfig::kEmergencyVoltage) {
        if (g_emergencyConsecutive < UINT8_MAX) {
            ++g_emergencyConsecutive;
        }
        if (g_emergencyConsecutive >= BatteryConfig::kEmergencyConsecutiveSamples &&
            g_powerState != PowerState::Emergency8) {
            transitionTo(PowerState::Emergency8, "voltage<=emergency");
            performEmergencyShutdown("voltage<=emergency");
        }
    } else {
        g_emergencyConsecutive = 0;
    }

    // 充电插入上升沿 → 回到 NORMAL
    if (battery.charging && !g_chargingLastSnapshot) {
        if (g_powerState != PowerState::Normal) {
            transitionTo(PowerState::Normal, "charging_rising_edge");
        }
    }
    g_chargingLastSnapshot = battery.charging;

    // 充电中不再下沉
    if (battery.charging) {
        return;
    }

    switch (g_powerState) {
        case PowerState::Normal:
            if (battery.voltage <= BatteryConfig::kWarnEnterVoltage) {
                transitionTo(PowerState::Warn25, "voltage<=warn_enter");
                playLowBatteryCueOnce();
            }
            break;
        case PowerState::Warn25:
            if (battery.voltage <= BatteryConfig::kCountdownEnterVoltage) {
                transitionTo(PowerState::Countdown15, "voltage<=countdown_enter");
                g_countdownDeferStartMs = 0;
                tryStartCountdownLocked(nowMs);
            } else if (battery.voltage >= BatteryConfig::kWarnExitVoltage) {
                transitionTo(PowerState::Normal, "voltage>=warn_exit");
            }
            break;
        case PowerState::Countdown15:
            if (battery.voltage >= BatteryConfig::kCountdownExitVoltage) {
                transitionTo(PowerState::Warn25, "voltage>=countdown_exit");
            } else {
                tickCountdown(nowMs);
            }
            break;
        case PowerState::Emergency8:
            // 已 deep sleep；理论不会走到这里
            break;
    }
}

void sampleAndPublish() {
    Snapshot snapshot = latest();
    snapshot.updatedMs = nowMs();
    ++snapshot.sampleCount;

    const esp_err_t batteryErr = sampleBattery(&snapshot.battery, snapshot.updatedMs);
    const esp_err_t tempErr = sampleTemperature(&snapshot.temperature);
    snapshot.valid = batteryErr == ESP_OK || tempErr == ESP_OK;

    // 调试覆写：覆盖电压/充电状态（不动 ADC 路径），用于不放电测试低电量状态机。
    if (g_debugVoltageOverride) {
        snapshot.battery.voltage = g_debugVoltageValue;
        const float v = snapshot.battery.voltage;
        int pct = static_cast<int>(lroundf(((v - 3.00f) / 1.20f) * 100.0f));
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        snapshot.battery.percent = pct;
        snapshot.battery.valid = v > 0.1f;
        snapshot.battery.lastError = ESP_OK;
    }
    if (g_debugChargingOverride >= 0) {
        snapshot.battery.charging = (g_debugChargingOverride != 0);
    }

    publishSnapshot(snapshot);
    updatePowerStateMachine(snapshot.battery, snapshot.updatedMs);
}

esp_err_t configureChargeDetectGpio() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << PIN_CHARGE_DETECT;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

void sensorsTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Sensors");

    while (true) {
        sampleAndPublish();
        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(kSensorsSamplePeriodMs));
    }
}

}  // namespace

esp_err_t start() {
    if (g_started) {
        return ESP_OK;
    }

    esp_err_t err = AppIdfAdc::init();
    if (err != ESP_OK) {
        LOG_E(TAG_SENSORS, "ADC init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = configureChargeDetectGpio();
    if (err != ESP_OK) {
        LOG_E(TAG_SENSORS, "Charge detect GPIO init failed: %s", esp_err_to_name(err));
        return err;
    }

    sampleAndPublish();

    err = AppIdfTasks::createPinnedToCoreInternal(sensorsTask, "IDF_Sensors", kSensorsTaskStackWords, nullptr, 1, 1,
                                                  &g_sensorsTaskMemory);
    if (err != ESP_OK) {
        LOG_E(TAG_SENSORS, "Sensor task creation failed: %s", esp_err_to_name(err));
        return err;
    }

    g_started = true;
    LOG_I(TAG_SENSORS, "IDF sensors started: battery GPIO%d, temp GPIO%d, charge GPIO%d",
          PIN_ADC_BAT,
          PIN_ADC_TEMP,
          PIN_CHARGE_DETECT);
    return ESP_OK;
}

bool isStarted() {
    return g_started;
}

Snapshot latest() {
    Snapshot snapshot;
    portENTER_CRITICAL(&g_snapshotMux);
    snapshot = g_latest;
    portEXIT_CRITICAL(&g_snapshotMux);
    return snapshot;
}

uint32_t taskStackHighWatermark() {
    if (g_sensorsTaskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_sensorsTaskMemory.handle);
}

PowerState currentPowerState() {
    return g_powerState;
}

int countdownSecondsRemaining() {
    return g_countdownSecondsRemaining;
}

void debugInjectVoltage(float voltage) {
    if (voltage <= 0.0f) {
        g_debugVoltageOverride = false;
        g_debugVoltageValue = 0.0f;
        LOG_W(TAG_SENSORS, "Debug voltage override cleared");
    } else {
        g_debugVoltageOverride = true;
        g_debugVoltageValue = voltage;
        LOG_W(TAG_SENSORS, "Debug voltage override set to %.3fV", voltage);
    }
}

void debugInjectCharging(int chargingOrNeg) {
    if (chargingOrNeg < 0) {
        g_debugChargingOverride = -1;
        LOG_W(TAG_SENSORS, "Debug charging override cleared");
    } else {
        g_debugChargingOverride = chargingOrNeg ? 1 : 0;
        LOG_W(TAG_SENSORS, "Debug charging override set to %d", g_debugChargingOverride);
    }
}

}  // namespace AppIdfSensors
