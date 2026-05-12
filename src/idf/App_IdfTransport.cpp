#include "App_IdfTransport.h"

#include <stdio.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfCellular.h"
#include "App_IdfMqtt.h"
#include "App_IdfNetwork.h"
#include "App_IdfOta.h"
#include "App_IdfRecorder.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace AppIdfTransport {
namespace {

constexpr const char* TAG_TRANSPORT = "IDF_TRANSPORT";
constexpr uint32_t kTaskStackWords = 8192;
constexpr uint32_t kLoopDelayMs = 1000;
constexpr uint32_t kAutoWifiLostFallbackMs = 30000;
constexpr uint32_t kAutoWifiStablePreferMs = 15000;
constexpr uint32_t kForcedPppTimeoutMs = 120000;
constexpr uint32_t kForcedMqttTimeoutMs = 75000;
constexpr uint32_t kAutoMqttTimeoutMs = 75000;
constexpr uint32_t kCellularDialRetryMs = 30000;
constexpr uint32_t kMqttRestartCooldownMs = 5000;
constexpr uint8_t kWifiMaxRetries = 2;
constexpr uint32_t kAutoSuppressResetMs = 300000;
// STA 拿到 IP 后再保持 kPortalAutoStopMs 毫秒不抖才关 AP,
// 避免握手抖动 / DHCP 续租瞬时丢线导致 portal 提前关闭让用户没法重试
constexpr uint32_t kPortalAutoStopMs = 5000;

SemaphoreHandle_t g_mutex = nullptr;
AppIdfTasks::StaticTaskMemory g_taskMemory;
Snapshot g_snapshot;
int64_t g_wifiLostSinceUs = 0;
int64_t g_wifiStableSinceUs = 0;
int64_t g_cellularAttemptStartUs = 0;
int64_t g_mqttAttemptStartUs = 0;
int64_t g_lastCellularDialUs = 0;
int64_t g_lastMqttRestartUs = 0;
int64_t g_autoCellularSuppressedSinceUs = 0;
int64_t g_portalAutoStopSinceUs = 0;
// 仅当本次 portal 会话期间见过 STA 断线再恢复才允许自动关闭,避免在线时手动开 portal
// (如串口 WIFIPORTAL 调试)被 5s 计时器秒关。
bool g_portalSawDisconnect = false;

class MutexLock {
public:
    explicit MutexLock(TickType_t timeoutTicks = portMAX_DELAY) {
        if (g_mutex != nullptr) {
            _locked = xSemaphoreTake(g_mutex, timeoutTicks) == pdTRUE;
        }
    }

    ~MutexLock() {
        if (_locked && g_mutex != nullptr) {
            xSemaphoreGive(g_mutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    bool _locked = false;
};

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

uint32_t elapsedMs(int64_t startUs, int64_t nowUs) {
    if (startUs <= 0 || nowUs <= startUs) {
        return 0;
    }
    return static_cast<uint32_t>((nowUs - startUs) / 1000);
}

void copyReasonLocked(const char* reason) {
    snprintf(g_snapshot.lastReason, sizeof(g_snapshot.lastReason), "%s", reason != nullptr ? reason : "none");
}

esp_err_t setLastError(esp_err_t err, const char* reason = nullptr) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.lastError = err;
        if (reason != nullptr) {
            copyReasonLocked(reason);
        }
    }
    return err;
}

bool isSwitchingBlocked() {
    return AppFlashGuard::isActive() || AppIdfOta::isBusy() || AppIdfRecorder::isBusy();
}

void setCellularStage(CellularStage stage) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.cellularStage = stage;
    }
}

void setActiveTransport(ActiveTransport transport, const char* reason) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }
    if (g_snapshot.active != transport) {
        LOG_I(TAG_TRANSPORT,
              "Active transport: %s -> %s (%s)",
              activeTransportName(g_snapshot.active),
              activeTransportName(transport),
              reason != nullptr ? reason : "-");
        ++g_snapshot.switchCount;
    }
    g_snapshot.active = transport;
    g_snapshot.pending = ActiveTransport::NONE;
    copyReasonLocked(reason);
}

void updateDurations(int64_t nowUs) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }
    g_snapshot.wifiLostMs = elapsedMs(g_wifiLostSinceUs, nowUs);
    g_snapshot.cellularAttemptMs = elapsedMs(g_cellularAttemptStartUs, nowUs);
    g_snapshot.mqttAttemptMs = elapsedMs(g_mqttAttemptStartUs, nowUs);
    g_snapshot.switchingBlocked = isSwitchingBlocked();
}

esp_err_t restartMqttForRoute(const char* reason) {
    const int64_t nowUs = esp_timer_get_time();
    if (g_lastMqttRestartUs > 0 && elapsedMs(g_lastMqttRestartUs, nowUs) < kMqttRestartCooldownMs) {
        return ESP_OK;
    }
    g_lastMqttRestartUs = nowUs;
    LOG_I(TAG_TRANSPORT, "Restarting MQTT for %s", reason != nullptr ? reason : "transport switch");
    AppIdfMqtt::stop();
    return AppIdfMqtt::start();
}

esp_err_t activateWifi(const char* reason) {
    if (!AppIdfNetwork::isConnected()) {
        return setLastError(ESP_ERR_INVALID_STATE, "wifi_not_connected");
    }
    esp_err_t err = AppIdfNetwork::setDefaultRoute();
    if (err != ESP_OK) {
        return setLastError(err, "wifi_default_route");
    }
    err = restartMqttForRoute("WiFi");
    setActiveTransport(ActiveTransport::WIFI, reason != nullptr ? reason : "wifi_ready");
    setCellularStage(CellularStage::IDLE);
    if (!isSwitchingBlocked() && AppIdfCellular::isConnected()) {
        AppIdfCellular::hangup();
    }
    return setLastError(err);
}

esp_err_t activateCellular(const char* reason) {
    if (!AppIdfCellular::isReadyForMqtt()) {
        return setLastError(ESP_ERR_INVALID_STATE, "ppp_not_ready");
    }
    esp_err_t err = AppIdfCellular::setDefaultRoute();
    if (err != ESP_OK) {
        return setLastError(err, "ppp_default_route");
    }
    err = restartMqttForRoute("4G PPP");
    setActiveTransport(ActiveTransport::PPP_4G, reason != nullptr ? reason : "ppp_ready");
    setCellularStage(AppIdfMqtt::isConnected() ? CellularStage::IDLE : CellularStage::MQTT);
    return setLastError(err);
}

void deactivateTransport(const char* reason) {
    AppIdfMqtt::stop();
    setActiveTransport(ActiveTransport::NONE, reason);
    g_mqttAttemptStartUs = 0;
    setCellularStage(CellularStage::IDLE);
}

void ensureWifiConnectRequested(bool limitRetries = false) {
    if (AppIdfNetwork::isConnected()) {
        return;
    }
    if (!AppIdfNetwork::hasStoredCredentials()) {
        if (!AppIdfNetwork::isPortalActive()) {
            AppIdfNetwork::startPortal();
        }
        return;
    }
    if (AppIdfNetwork::isPortalActive()) {
        return;
    }
    const AppIdfNetwork::Snapshot net = AppIdfNetwork::snapshot();
    if (limitRetries && net.wifiFailCount >= kWifiMaxRetries) {
        LOG_W(TAG_TRANSPORT, "WiFi failed %u times; starting provisioning portal",
              static_cast<unsigned>(net.wifiFailCount));
        AppIdfNetwork::startPortal();
        return;
    }
    if (!net.connecting) {
        AppIdfNetwork::connectStored();
    }
}

bool dialCellularIfDue(int64_t nowUs) {
    if (AppIdfCellular::isReadyForMqtt()) {
        return true;
    }
    if (g_lastCellularDialUs > 0 && elapsedMs(g_lastCellularDialUs, nowUs) < kCellularDialRetryMs) {
        return false;
    }
    g_lastCellularDialUs = nowUs;
    setCellularStage(CellularStage::PPP);
    LOG_W(TAG_TRANSPORT, "Dialing 4G PPP fallback");
    const esp_err_t err = AppIdfCellular::dial("cmnet", 90000);
    if (err != ESP_OK) {
        setLastError(err, "ppp_dial_failed");
        LOG_W(TAG_TRANSPORT, "4G PPP dial failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void evaluateWifiOnly(int64_t nowUs) {
    g_cellularAttemptStartUs = 0;
    g_mqttAttemptStartUs = 0;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.autoCellularSuppressed = false;
            g_snapshot.cellularCancelRequested = false;
        }
    }
    setCellularStage(CellularStage::IDLE);

    if (activeTransport() == ActiveTransport::PPP_4G && !isSwitchingBlocked()) {
        deactivateTransport("wifi_only");
        AppIdfCellular::hangup();
    }

    ensureWifiConnectRequested(true);
    if (AppIdfNetwork::isConnected()) {
        g_wifiLostSinceUs = 0;
        if (activeTransport() != ActiveTransport::WIFI && !isSwitchingBlocked()) {
            activateWifi("wifi_only_ready");
        }
    } else if (activeTransport() == ActiveTransport::WIFI) {
        deactivateTransport("wifi_lost");
        if (g_wifiLostSinceUs == 0) {
            g_wifiLostSinceUs = nowUs;
        }
    }
}

void evaluateCellularOnly(int64_t nowUs) {
    if (g_cellularAttemptStartUs == 0) {
        g_cellularAttemptStartUs = nowUs;
    }
    if (activeTransport() == ActiveTransport::WIFI && !isSwitchingBlocked()) {
        deactivateTransport("forced_4g");
    }
    if (!isSwitchingBlocked()) {
        AppIdfNetwork::disconnect(true);
    }

    const bool pppReady = dialCellularIfDue(nowUs);
    if (pppReady && activeTransport() != ActiveTransport::PPP_4G && !isSwitchingBlocked()) {
        activateCellular("forced_4g_ready");
        g_mqttAttemptStartUs = nowUs;
    }

    if (activeTransport() == ActiveTransport::PPP_4G) {
        if (AppIdfMqtt::isConnected()) {
            setCellularStage(CellularStage::IDLE);
            g_mqttAttemptStartUs = 0;
        } else {
            setCellularStage(CellularStage::MQTT);
            if (g_mqttAttemptStartUs == 0) {
                g_mqttAttemptStartUs = nowUs;
            }
        }
    }

    if (!pppReady && elapsedMs(g_cellularAttemptStartUs, nowUs) > kForcedPppTimeoutMs) {
        LOG_W(TAG_TRANSPORT, "Forced 4G PPP timed out; returning to AUTO");
        requestMode(NetMode::AUTO);
        return;
    }
    if (activeTransport() == ActiveTransport::PPP_4G && !AppIdfMqtt::isConnected() &&
        elapsedMs(g_mqttAttemptStartUs, nowUs) > kForcedMqttTimeoutMs) {
        LOG_W(TAG_TRANSPORT, "Forced 4G MQTT timed out; returning to AUTO");
        AppIdfCellular::hangup();
        requestMode(NetMode::AUTO);
    }
}

void evaluateAuto(int64_t nowUs) {
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked() && g_snapshot.autoCellularSuppressed &&
            g_autoCellularSuppressedSinceUs > 0 &&
            elapsedMs(g_autoCellularSuppressedSinceUs, nowUs) >= kAutoSuppressResetMs) {
            g_snapshot.autoCellularSuppressed = false;
            g_autoCellularSuppressedSinceUs = 0;
            g_cellularAttemptStartUs = 0;
            g_lastCellularDialUs = 0;
            LOG_W(TAG_TRANSPORT, "4G suppression expired; resuming cellular fallback");
        }
    }

    ensureWifiConnectRequested(true);
    const bool wifiReady = AppIdfNetwork::isConnected();

    if (wifiReady) {
        g_wifiLostSinceUs = 0;
        if (g_wifiStableSinceUs == 0) {
            g_wifiStableSinceUs = nowUs;
        }
        {
            MutexLock lock(pdMS_TO_TICKS(100));
            if (lock.locked()) {
                g_snapshot.autoCellularSuppressed = false;
                g_autoCellularSuppressedSinceUs = 0;
                g_snapshot.cellularCancelRequested = false;
            }
        }
        if (!isSwitchingBlocked() &&
            (activeTransport() == ActiveTransport::NONE ||
             (activeTransport() == ActiveTransport::PPP_4G &&
              elapsedMs(g_wifiStableSinceUs, nowUs) >= kAutoWifiStablePreferMs))) {
            activateWifi("auto_wifi_priority");
        }
        return;
    }

    g_wifiStableSinceUs = 0;
    if (g_wifiLostSinceUs == 0) {
        g_wifiLostSinceUs = nowUs;
    }
    if (activeTransport() == ActiveTransport::WIFI && !isSwitchingBlocked()) {
        deactivateTransport("auto_wifi_lost");
    }

    Snapshot snap = snapshot();
    const AppIdfNetwork::Snapshot netSnap = AppIdfNetwork::snapshot();
    const bool wifiTriedEnough = netSnap.wifiFailCount >= kWifiMaxRetries ||
                                 elapsedMs(g_wifiLostSinceUs, nowUs) >= kAutoWifiLostFallbackMs;
    if (snap.autoCellularSuppressed || !wifiTriedEnough || isSwitchingBlocked()) {
        return;
    }
    if (g_cellularAttemptStartUs == 0) {
        g_cellularAttemptStartUs = nowUs;
    }
    const bool pppReady = dialCellularIfDue(nowUs);
    if (pppReady && activeTransport() != ActiveTransport::PPP_4G) {
        activateCellular("auto_ppp_fallback");
        g_mqttAttemptStartUs = nowUs;
    }

    if (activeTransport() == ActiveTransport::PPP_4G) {
        if (AppIdfMqtt::isConnected()) {
            setCellularStage(CellularStage::IDLE);
            g_mqttAttemptStartUs = 0;
        } else {
            setCellularStage(CellularStage::MQTT);
            if (g_mqttAttemptStartUs == 0) {
                g_mqttAttemptStartUs = nowUs;
            }
            if (elapsedMs(g_mqttAttemptStartUs, nowUs) > kAutoMqttTimeoutMs) {
                LOG_W(TAG_TRANSPORT, "AUTO 4G MQTT timed out; suppressing this fallback round");
                deactivateTransport("auto_ppp_mqtt_timeout");
                AppIdfCellular::hangup();
                MutexLock lock(pdMS_TO_TICKS(100));
                if (lock.locked()) {
                    g_snapshot.autoCellularSuppressed = true;
                    g_autoCellularSuppressedSinceUs = nowUs;
                }
            }
        }
    }
}

void applyPendingIfPossible() {
    if (isSwitchingBlocked()) {
        return;
    }
    const ActiveTransport pending = snapshot().pending;
    if (pending == ActiveTransport::NONE) {
        return;
    }
    if (pending == ActiveTransport::WIFI) {
        activateWifi("pending_wifi");
    } else if (pending == ActiveTransport::PPP_4G) {
        activateCellular("pending_4g");
    }
}

void transportTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Transport");
    while (true) {
        const int64_t nowUs = esp_timer_get_time();
        updateDurations(nowUs);

        if (snapshot().cellularCancelRequested) {
            deactivateTransport("cellular_cancel");
            AppIdfCellular::hangup();
            g_cellularAttemptStartUs = 0;
            MutexLock lock(pdMS_TO_TICKS(100));
            if (lock.locked()) {
                g_snapshot.cellularCancelRequested = false;
                g_snapshot.autoCellularSuppressed = true;
            }
        }

        applyPendingIfPossible();

        // STA 拿到 IP 且持续稳定后,自动关掉配网 AP 和 captive portal HTTP 服务,
        // 避免开放热点长期暴露 /save 接口、APSTA 双发耗电、DNS/HTTP 任务常驻。
        // 在事件回调里直接 set_mode 不安全,所以放到这里由 transport 任务收口。
        // 仅当本次 portal 会话观察过 STA 断线再恢复才触发,绕开"在线时手动 WIFIPORTAL"
        // 被秒关的场景(留给串口调试)。
        {
            const AppIdfNetwork::Snapshot net = AppIdfNetwork::snapshot();
            if (net.portalActive) {
                if (!net.connected) {
                    g_portalSawDisconnect = true;
                    g_portalAutoStopSinceUs = 0;
                } else if (g_portalSawDisconnect) {
                    if (g_portalAutoStopSinceUs == 0) {
                        g_portalAutoStopSinceUs = nowUs;
                    } else if (elapsedMs(g_portalAutoStopSinceUs, nowUs) >= kPortalAutoStopMs) {
                        LOG_I(TAG_TRANSPORT,
                              "STA stable for %u ms after provisioning; auto-closing portal AP",
                              static_cast<unsigned>(kPortalAutoStopMs));
                        AppIdfNetwork::stopPortal();
                        g_portalAutoStopSinceUs = 0;
                        g_portalSawDisconnect = false;
                    }
                } else {
                    // portal 开启时 STA 始终在线 → 视作手动调试,不主动关
                    g_portalAutoStopSinceUs = 0;
                }
            } else {
                g_portalAutoStopSinceUs = 0;
                g_portalSawDisconnect = false;
            }
        }

        switch (currentMode()) {
            case NetMode::WIFI_ONLY:
                evaluateWifiOnly(nowUs);
                break;
            case NetMode::CELLULAR_ONLY:
                evaluateCellularOnly(nowUs);
                break;
            case NetMode::AUTO:
            default:
                evaluateAuto(nowUs);
                break;
        }

        updateDurations(esp_timer_get_time());
        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
    }
}

}  // namespace

esp_err_t start() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }
    if (g_taskMemory.handle != nullptr) {
        return ESP_OK;
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.started = true;
            g_snapshot.mode = NetMode::AUTO;
            g_snapshot.active = ActiveTransport::NONE;
            g_snapshot.pending = ActiveTransport::NONE;
            copyReasonLocked("start");
        }
    }

    err = AppIdfTasks::createPinnedToCoreInternal(transportTask, "IDF_Transport", kTaskStackWords, nullptr, 3, 0,
                                                  &g_taskMemory);
    if (err != ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.started = false;
            g_snapshot.lastError = err;
        }
    }
    return err;
}

bool isStarted() {
    return g_taskMemory.handle != nullptr;
}

esp_err_t requestMode(NetMode mode) {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_snapshot.mode != mode) {
        LOG_I(TAG_TRANSPORT, "Net mode: %s -> %s", netModeName(g_snapshot.mode), netModeName(mode));
    }
    g_snapshot.mode = mode;
    g_snapshot.autoCellularSuppressed = false;
    g_snapshot.cellularCancelRequested = false;
    g_snapshot.pending = ActiveTransport::NONE;
    copyReasonLocked("mode_request");
    g_cellularAttemptStartUs = 0;
    g_mqttAttemptStartUs = 0;
    g_lastCellularDialUs = 0;
    g_autoCellularSuppressedSinceUs = 0;
    return ESP_OK;
}

esp_err_t requestActive(ActiveTransport transport, const char* reason) {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    g_snapshot.pending = transport;
    copyReasonLocked(reason != nullptr ? reason : "active_request");
    return ESP_OK;
}

esp_err_t cancelCellularAttempt() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    g_snapshot.cellularCancelRequested = true;
    return ESP_OK;
}

NetMode currentMode() {
    return snapshot().mode;
}

ActiveTransport activeTransport() {
    return snapshot().active;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

uint32_t taskStackHighWatermark() {
    return g_taskMemory.handle != nullptr ? uxTaskGetStackHighWaterMark(g_taskMemory.handle) : 0;
}

const char* netModeName(NetMode mode) {
    switch (mode) {
        case NetMode::AUTO:
            return "AUTO";
        case NetMode::WIFI_ONLY:
            return "WIFI_ONLY";
        case NetMode::CELLULAR_ONLY:
            return "4G_ONLY";
        default:
            return "UNKNOWN";
    }
}

const char* activeTransportName(ActiveTransport transport) {
    switch (transport) {
        case ActiveTransport::NONE:
            return "NONE";
        case ActiveTransport::WIFI:
            return "WIFI";
        case ActiveTransport::PPP_4G:
            return "PPP_4G";
        default:
            return "UNKNOWN";
    }
}

const char* cellularStageName(CellularStage stage) {
    switch (stage) {
        case CellularStage::IDLE:
            return "idle";
        case CellularStage::PPP:
            return "ppp";
        case CellularStage::MQTT:
            return "mqtt";
        default:
            return "unknown";
    }
}

}  // namespace AppIdfTransport
