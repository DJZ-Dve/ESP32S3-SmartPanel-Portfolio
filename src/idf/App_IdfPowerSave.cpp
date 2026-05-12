#include "App_IdfPowerSave.h"

#include <atomic>

#include "App_IdfAudio.h"
#include "App_IdfBleAircon.h"
#include "App_IdfDisplay.h"
#include "App_IdfOta.h"
#include "App_IdfRecorder.h"
#include "App_IdfUi.h"
#include "App_Log.h"
#include "Power_Config.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc.h"

namespace AppIdfPowerSave {
namespace {

constexpr const char* TAG = "IDF_POWER";

std::atomic<State> g_state{State::L0_Active};
std::atomic<uint32_t> g_lastActivityMs{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{AppPowerConfig::kPowerSaveEnable};

esp_timer_handle_t g_tickTimer = nullptr;

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void setCpuFreqMhz(int mhz) {
    rtc_cpu_freq_config_t cfg = {};
    if (!rtc_clk_cpu_freq_mhz_to_config(mhz, &cfg)) {
        LOG_W(TAG, "rtc_clk_cpu_freq_mhz_to_config(%d) unsupported", mhz);
        return;
    }
    rtc_clk_cpu_freq_set_config_fast(&cfg);
}

void tickCallback(void*) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (g_state.load(std::memory_order_relaxed) != State::L0_Active) {
        return;
    }
    const uint32_t idle = nowMs() - g_lastActivityMs.load(std::memory_order_relaxed);
    if (idle < AppPowerConfig::kIdleTimeoutMs) {
        return;
    }
    if (!canEnterL1()) {
        return;
    }
    (void)enterL1();
}

}  // namespace

esp_err_t start() {
    if (g_started.load()) {
        return ESP_OK;
    }
    g_lastActivityMs.store(nowMs());

    const esp_timer_create_args_t args = {
        .callback = tickCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "powersave_tick",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &g_tickTimer);
    if (err != ESP_OK) {
        LOG_E(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(g_tickTimer, AppPowerConfig::kTickPeriodMs * 1000ULL);
    if (err != ESP_OK) {
        LOG_E(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        esp_timer_delete(g_tickTimer);
        g_tickTimer = nullptr;
        return err;
    }

    g_started.store(true);
    LOG_I(TAG,
          "PowerSave started: idle=%ums, wifi_ps_l1=%d, cpu_l1=%dMHz, cpu_l0=%dMHz, enabled=%d",
          static_cast<unsigned>(AppPowerConfig::kIdleTimeoutMs),
          static_cast<int>(AppPowerConfig::kWifiPsModeL1),
          AppPowerConfig::kCpuFreqMhzL1,
          AppPowerConfig::kCpuFreqMhzL0,
          g_enabled.load() ? 1 : 0);
    return ESP_OK;
}

bool isStarted() {
    return g_started.load();
}

void notifyActivity() {
    g_lastActivityMs.store(nowMs(), std::memory_order_relaxed);
}

bool canEnterL1() {
    if (g_state.load() == State::L1_Standby) {
        return false;
    }
    const auto recSnap = AppIdfRecorder::snapshot();
    if (recSnap.recording || recSnap.uploading || recSnap.startPending) {
        return false;
    }
    if (AppIdfAudio::isPaEnabled()) {
        return false;
    }
    const auto pairState = AppIdfBleAircon::getPairingState();
    if (pairState == AppIdfBleAircon::PairingState::Scanning ||
        pairState == AppIdfBleAircon::PairingState::Pairing) {
        return false;
    }
    if (AppIdfOta::isBusy()) {
        return false;
    }
    return true;
}

esp_err_t enterL1() {
    if (g_state.load() == State::L1_Standby) {
        return ESP_OK;
    }
    LOG_I(TAG, "entering L1 (idle=%ums)", static_cast<unsigned>(inactivityMs()));

    const esp_err_t uiErr = AppIdfUi::applyPowerSaveEnter();
    if (uiErr != ESP_OK) {
        LOG_W(TAG, "L1 enter: UI lock failed: %s (will retry next tick)", esp_err_to_name(uiErr));
        return uiErr;
    }

    const esp_err_t psErr = esp_wifi_set_ps(AppPowerConfig::kWifiPsModeL1);
    if (psErr != ESP_OK && psErr != ESP_ERR_WIFI_NOT_INIT && psErr != ESP_ERR_WIFI_NOT_STARTED) {
        LOG_W(TAG, "esp_wifi_set_ps(MIN_MODEM) failed: %s", esp_err_to_name(psErr));
    }

    setCpuFreqMhz(AppPowerConfig::kCpuFreqMhzL1);

    g_state.store(State::L1_Standby);
    return ESP_OK;
}

esp_err_t exitL1() {
    if (g_state.load() == State::L0_Active) {
        return ESP_OK;
    }
    LOG_I(TAG, "exiting L1");

    setCpuFreqMhz(AppPowerConfig::kCpuFreqMhzL0);

    AppIdfDisplay::setBacklight(true);

    const esp_err_t psErr = esp_wifi_set_ps(AppPowerConfig::kWifiPsModeL0);
    if (psErr != ESP_OK && psErr != ESP_ERR_WIFI_NOT_INIT && psErr != ESP_ERR_WIFI_NOT_STARTED) {
        LOG_W(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(psErr));
    }

    const esp_err_t uiErr = AppIdfUi::applyPowerSaveExit();
    if (uiErr != ESP_OK) {
        LOG_W(TAG, "L1 exit: UI lock failed: %s", esp_err_to_name(uiErr));
    }

    g_state.store(State::L0_Active);
    g_lastActivityMs.store(nowMs());
    return ESP_OK;
}

State currentState() {
    return g_state.load();
}

bool isL1() {
    return g_state.load() == State::L1_Standby;
}

uint32_t lastActivityMs() {
    return g_lastActivityMs.load();
}

uint32_t inactivityMs() {
    return nowMs() - g_lastActivityMs.load();
}

void setEnabled(bool enabled) {
    g_enabled.store(enabled);
    if (!enabled) {
        (void)exitL1();
    } else {
        notifyActivity();
    }
    LOG_I(TAG, "PowerSave enabled=%d", enabled ? 1 : 0);
}

bool isEnabled() {
    return g_enabled.load();
}

}  // namespace AppIdfPowerSave
