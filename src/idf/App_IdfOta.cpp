#include "App_IdfOta.h"

#include <stdio.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfAudio.h"
#include "App_IdfCellular.h"
#include "App_IdfMqtt.h"
#include "App_IdfNetwork.h"
#include "App_IdfPowerSave.h"
#include "App_IdfRecorder.h"
#include "App_IdfSensors.h"
#include "App_IdfTasks.h"
#include "App_IdfTransport.h"
#include "App_Log.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md5.h"

namespace AppIdfOta {
namespace {

constexpr const char* TAG_OTA_IDF = "IDF_OTA";
constexpr uint32_t kOtaTaskStackWords = 4096;
constexpr uint32_t kHealthConfirmTimeoutMs = 120000;
constexpr size_t kUrlLen = 200;
constexpr size_t kVersionLen = 16;
constexpr size_t kMd5Len = 33;
constexpr size_t kRequestIdLen = 40;
constexpr size_t kOtaBufferSize = 1024;

struct Metadata {
    char url[kUrlLen] = {};
    char version[kVersionLen] = {};
    char md5[kMd5Len] = {};
    char requestId[kRequestIdLen] = {};
    uint32_t size = 0;
    int minBatteryPct = 0;
    bool requireCharging = false;
    bool force = false;
};

SemaphoreHandle_t g_mutex = nullptr;
AppIdfTasks::StaticTaskMemory g_otaTaskMemory;
Snapshot g_snapshot;
Metadata g_pendingMetadata;
char g_lastPreflightOkRequestId[kRequestIdLen] = {};
uint64_t g_rollbackDeadlineUs = 0;
bool g_rollbackPossible = false;
volatile bool g_cancelRequested = false;

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

class OtaHandleGuard {
public:
    ~OtaHandleGuard() {
        if (active) {
            esp_ota_abort(handle);
        }
    }

    esp_ota_handle_t handle = 0;
    bool active = false;
};

class HttpClientGuard {
public:
    ~HttpClientGuard() {
        if (handle != nullptr) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
        }
    }

    esp_http_client_handle_t handle = nullptr;
};

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t setLastError(esp_err_t err) {
    g_snapshot.lastError = err;
    return err;
}

void copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstSize, "%s", src);
}

void setReasonLocked(const char* reason) {
    copyText(g_snapshot.lastReason, sizeof(g_snapshot.lastReason), reason != nullptr ? reason : "");
}

const char* firmwareVersion() {
    const esp_app_desc_t* desc = esp_app_get_description();
    return (desc != nullptr && desc->version[0] != '\0') ? desc->version : "IDF";
}

const char* cjsonString(cJSON* object, const char* name) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

uint32_t cjsonUint(cJSON* object, const char* name, uint32_t fallback = 0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0.0) {
        return static_cast<uint32_t>(item->valuedouble);
    }
    return fallback;
}

int cjsonInt(cJSON* object, const char* name, int fallback = 0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

bool cjsonBool(cJSON* object, const char* name, bool fallback = false) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

bool isFirmwareVersionNewer(const char* newVer, const char* currentVer) {
    int nMajor = 0;
    int nMinor = 0;
    int nPatch = 0;
    int cMajor = 0;
    int cMinor = 0;
    int cPatch = 0;

    sscanf(newVer != nullptr ? newVer : "", "%d.%d.%d", &nMajor, &nMinor, &nPatch);
    sscanf(currentVer != nullptr ? currentVer : "", "%d.%d.%d", &cMajor, &cMinor, &cPatch);

    if (nMajor != cMajor) {
        return nMajor > cMajor;
    }
    if (nMinor != cMinor) {
        return nMinor > cMinor;
    }
    return nPatch > cPatch;
}

bool isMd5Hex32(const char* md5) {
    if (md5 == nullptr || strlen(md5) != 32) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        const char c = md5[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

char lowerHex(char c) {
    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool md5EqualsIgnoreCase(const char* lhs, const char* rhs) {
    if (!isMd5Hex32(lhs) || !isMd5Hex32(rhs)) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (lowerHex(lhs[i]) != lowerHex(rhs[i])) {
            return false;
        }
    }
    return true;
}

const char* validateMetadata(const Metadata& metadata) {
    if (metadata.version[0] == '\0') {
        return "Invalid version";
    }
    if (metadata.requestId[0] == '\0') {
        return "Legacy OTA disabled";
    }
    if (metadata.url[0] == '\0') {
        return "Invalid URL";
    }
    if (!isMd5Hex32(metadata.md5)) {
        return "Invalid md5";
    }
    if (metadata.size == 0) {
        return "Invalid size";
    }
    return nullptr;
}

bool parseNotifyMetadata(const char* json, Metadata* outMetadata) {
    if (json == nullptr || outMetadata == nullptr) {
        return false;
    }

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return false;
    }

    Metadata metadata;
    copyText(metadata.url, sizeof(metadata.url), cjsonString(root, "url"));
    copyText(metadata.version, sizeof(metadata.version), cjsonString(root, "version"));
    copyText(metadata.md5, sizeof(metadata.md5), cjsonString(root, "md5"));
    copyText(metadata.requestId, sizeof(metadata.requestId), cjsonString(root, "request_id"));
    metadata.size = cjsonUint(root, "size");
    metadata.minBatteryPct = cjsonInt(root, "min_battery_pct", 0);
    metadata.requireCharging = cjsonBool(root, "require_charging", false);
    metadata.force = cjsonBool(root, "force", false);
    cJSON_Delete(root);

    *outMetadata = metadata;
    return true;
}

bool evaluateReadiness(const char* targetVersion,
                       int minBatteryPct,
                       bool requireCharging,
                       const char** reason,
                       bool* recording,
                       bool* playing,
                       bool* otaBusy) {
    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();
    const AppIdfAudio::Snapshot audio = AppIdfAudio::snapshot();
    const uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    *recording = AppIdfRecorder::isBusy();
    *playing = audio.paEnabled;
    *otaBusy = g_snapshot.busy || g_snapshot.pending || AppFlashGuard::isActive();
    *reason = "ready";

    if (targetVersion == nullptr || targetVersion[0] == '\0' ||
        !isFirmwareVersionNewer(targetVersion, firmwareVersion())) {
        *reason = "up_to_date";
        return false;
    }
    if (*otaBusy) {
        *reason = "ota_busy";
        return false;
    }
    if (*recording) {
        *reason = "recording";
        return false;
    }
    if (*playing) {
        *reason = "playing";
        return false;
    }
    if (requireCharging && !sensors.battery.charging) {
        *reason = "not_charging";
        return false;
    }
    if (minBatteryPct > 0) {
        if (!sensors.battery.valid || sensors.battery.percent < 0) {
            *reason = "battery_unknown";
            return false;
        }
        if (sensors.battery.percent < minBatteryPct) {
            *reason = "low_battery";
            return false;
        }
    }
    if ((!sensors.battery.valid || sensors.battery.percent < 0) && !sensors.battery.charging) {
        *reason = "battery_unknown";
        return false;
    }
    if (freeInternal < 24 * 1024 || largestInternal < 8 * 1024) {
        *reason = "low_internal_sram";
        return false;
    }
    return true;
}

bool activeTransportReady(const char** reason) {
    const AppIdfTransport::ActiveTransport active = AppIdfTransport::activeTransport();
    if (active == AppIdfTransport::ActiveTransport::WIFI) {
        if (AppIdfNetwork::isConnected()) {
            return true;
        }
        *reason = "wifi_disconnected";
        return false;
    }
    if (active == AppIdfTransport::ActiveTransport::PPP_4G) {
        if (AppIdfCellular::isReadyForMqtt()) {
            return true;
        }
        *reason = "ppp_disconnected";
        return false;
    }
    if (AppIdfNetwork::isConnected() || AppIdfCellular::isReadyForMqtt()) {
        return true;
    }
    *reason = "network_disconnected";
    return false;
}

esp_err_t publishResult(const Metadata& metadata, const char* status, const char* message) {
    return AppIdfMqtt::publishOtaResult(status, metadata.version, metadata.requestId, message);
}

esp_err_t failOta(const Metadata& metadata, esp_err_t err, const char* message) {
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            setReasonLocked(message);
            g_snapshot.busy = false;
            g_snapshot.pending = false;
            g_snapshot.progress = -1;
            setLastError(err);
        }
    }
    LOG_E(TAG_OTA_IDF, "OTA failed: %s (%s)", message != nullptr ? message : "unknown", esp_err_to_name(err));
    publishResult(metadata, "failed", message);
    return err;
}

void clearPendingLocked() {
    g_pendingMetadata = {};
    g_snapshot.pending = false;
}

esp_err_t performOta(const Metadata& metadata) {
    g_cancelRequested = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.busy = true;
            g_snapshot.pending = false;
            g_snapshot.progress = 0;
            g_snapshot.expectedSize = metadata.size;
            g_snapshot.writtenSize = 0;
            copyText(g_snapshot.version, sizeof(g_snapshot.version), metadata.version);
            copyText(g_snapshot.requestId, sizeof(g_snapshot.requestId), metadata.requestId);
            setReasonLocked("downloading");
        }
    }

    const char* invalid = validateMetadata(metadata);
    if (invalid != nullptr) {
        return failOta(metadata, ESP_ERR_INVALID_ARG, invalid);
    }
    if (!isFirmwareVersionNewer(metadata.version, firmwareVersion())) {
        return failOta(metadata, ESP_ERR_INVALID_VERSION, "Already up to date");
    }

    bool recording = false;
    bool playing = false;
    bool otaBusy = false;
    const char* reason = "ready";
    bool ready = false;
    bool readinessChecked = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            readinessChecked = true;
            const bool wasBusy = g_snapshot.busy;
            g_snapshot.busy = false;
            ready = evaluateReadiness(metadata.version,
                                      metadata.minBatteryPct,
                                      metadata.requireCharging,
                                      &reason,
                                      &recording,
                                      &playing,
                                      &otaBusy);
            g_snapshot.busy = wasBusy;
        }
    }
    if (!readinessChecked) {
        return failOta(metadata, ESP_ERR_TIMEOUT, "mutex_timeout");
    }
    if (!ready) {
        return failOta(metadata, ESP_ERR_INVALID_STATE, reason);
    }

    ScopedFlashGuard flashGuard("IDF OTA", 30000);
    if (!flashGuard.ok()) {
        return failOta(metadata, ESP_ERR_TIMEOUT, "ota_busy");
    }

    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (updatePartition == nullptr) {
        return failOta(metadata, ESP_ERR_NOT_FOUND, "No partition");
    }
    if (metadata.size > updatePartition->size) {
        return failOta(metadata, ESP_ERR_INVALID_SIZE, "Firmware too large");
    }
    const char* transportReason = "ready";
    if (!activeTransportReady(&transportReason)) {
        return failOta(metadata, ESP_ERR_INVALID_STATE, transportReason);
    }
    LOG_I(TAG_OTA_IDF,
          "OTA download using active transport %s",
          AppIdfTransport::activeTransportName(AppIdfTransport::activeTransport()));

    esp_http_client_config_t httpConfig = {};
    httpConfig.url = metadata.url;
    httpConfig.method = HTTP_METHOD_GET;
    httpConfig.timeout_ms = 30000;
    httpConfig.buffer_size = 1024;
    httpConfig.buffer_size_tx = 512;
    httpConfig.max_redirection_count = 3;
    httpConfig.keep_alive_enable = true;

    HttpClientGuard http;
    http.handle = esp_http_client_init(&httpConfig);
    if (http.handle == nullptr) {
        return failOta(metadata, ESP_ERR_NO_MEM, "HTTP init failed");
    }

    esp_err_t err = esp_http_client_open(http.handle, 0);
    if (err != ESP_OK) {
        return failOta(metadata, err, "HTTP error");
    }

    const int64_t contentLength = esp_http_client_fetch_headers(http.handle);
    const int statusCode = esp_http_client_get_status_code(http.handle);
    if (statusCode != 200) {
        return failOta(metadata, ESP_FAIL, "HTTP error");
    }
    if (contentLength == 0) {
        return failOta(metadata, ESP_ERR_INVALID_SIZE, "Invalid size");
    }
    if (contentLength > 0 && static_cast<uint32_t>(contentLength) != metadata.size) {
        return failOta(metadata, ESP_ERR_INVALID_SIZE, "Invalid size");
    }

    OtaHandleGuard ota;
    err = esp_ota_begin(updatePartition, metadata.size, &ota.handle);
    if (err != ESP_OK) {
        return failOta(metadata, err, "Init failed");
    }
    ota.active = true;

    uint8_t* buffer =
        static_cast<uint8_t*>(heap_caps_malloc(kOtaBufferSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return failOta(metadata, ESP_ERR_NO_MEM, "Out of memory");
    }

    mbedtls_md5_context md5Ctx;
    mbedtls_md5_init(&md5Ctx);
    mbedtls_md5_starts(&md5Ctx);

    uint32_t totalWritten = 0;
    bool downloadOk = true;
    bool cancelled = false;
    int emptyReadCount = 0;
    int lastLoggedPercent = -1;
    while (totalWritten < metadata.size) {
        if (g_cancelRequested) {
            downloadOk = false;
            cancelled = true;
            break;
        }
        const uint32_t remaining = metadata.size - totalWritten;
        const int toRead = remaining < kOtaBufferSize ? static_cast<int>(remaining) : static_cast<int>(kOtaBufferSize);
        const int bytesRead = esp_http_client_read(http.handle, reinterpret_cast<char*>(buffer), toRead);
        if (bytesRead < 0) {
            downloadOk = false;
            break;
        }
        if (bytesRead == 0) {
            ++emptyReadCount;
            if (emptyReadCount > 600) {
                downloadOk = false;
                break;
            }
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        emptyReadCount = 0;
        mbedtls_md5_update(&md5Ctx, buffer, static_cast<size_t>(bytesRead));
        err = esp_ota_write(ota.handle, buffer, static_cast<size_t>(bytesRead));
        if (err != ESP_OK) {
            downloadOk = false;
            break;
        }

        totalWritten += static_cast<uint32_t>(bytesRead);
        const int progress = static_cast<int>((totalWritten * 100ULL) / metadata.size);
        {
            MutexLock lock(pdMS_TO_TICKS(50));
            if (lock.locked()) {
                g_snapshot.progress = progress;
                g_snapshot.writtenSize = totalWritten;
            }
        }
        if (progress / 5 != lastLoggedPercent / 5) {
            lastLoggedPercent = progress;
            LOG_I(TAG_OTA_IDF, "OTA %d%% (%uKB/%uKB)", progress, totalWritten / 1024, metadata.size / 1024);
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t digest[16] = {};
    char actualMd5[33] = {};
    mbedtls_md5_finish(&md5Ctx, digest);
    mbedtls_md5_free(&md5Ctx);
    for (int i = 0; i < 16; ++i) {
        snprintf(actualMd5 + i * 2, sizeof(actualMd5) - i * 2, "%02x", digest[i]);
    }
    heap_caps_free(buffer);

    if (!downloadOk || totalWritten == 0) {
        const char* failMsg = cancelled ? "cancelled" : "Download error";
        return failOta(metadata, cancelled ? ESP_ERR_INVALID_STATE : (err != ESP_OK ? err : ESP_FAIL), failMsg);
    }
    if (totalWritten != metadata.size) {
        return failOta(metadata, ESP_ERR_INVALID_SIZE, "Invalid size");
    }
    if (!md5EqualsIgnoreCase(actualMd5, metadata.md5)) {
        LOG_E(TAG_OTA_IDF, "OTA MD5 mismatch: expected=%s actual=%s", metadata.md5, actualMd5);
        return failOta(metadata, ESP_ERR_INVALID_CRC, "MD5 mismatch");
    }

    err = esp_ota_end(ota.handle);
    ota.active = false;
    if (err != ESP_OK) {
        return failOta(metadata, err, "Verify error");
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
        return failOta(metadata, err, "Boot partition error");
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.busy = false;
            g_snapshot.pending = false;
            g_snapshot.progress = 100;
            setReasonLocked("rebooting");
            setLastError(ESP_OK);
        }
    }
    publishResult(metadata, "success", "Rebooting...");
    LOG_I(TAG_OTA_IDF, "OTA update succeeded; rebooting");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

void checkRollbackTimeout() {
    bool shouldRollback = false;
    bool deadlineReached = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked() || !g_snapshot.rollbackPending || g_rollbackDeadlineUs == 0) {
            return;
        }
        if (esp_timer_get_time() >= static_cast<int64_t>(g_rollbackDeadlineUs)) {
            g_snapshot.rollbackPending = false;
            shouldRollback = g_rollbackPossible;
            deadlineReached = true;
            setReasonLocked(g_rollbackPossible ? "rollback_timeout" : "rollback_unavailable");
        }
    }

    if (!deadlineReached) {
        return;
    }
    if (shouldRollback) {
        LOG_E(TAG_OTA_IDF, "OTA health check timed out; rolling back");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    } else {
        LOG_W(TAG_OTA_IDF, "OTA health check timed out but no rollback image is available");
    }
}

void otaTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_OTA");

    while (true) {
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (notified == 0) {
            checkRollbackTimeout();
            AppIdfTasks::feedCurrentTaskWatchdog();
            continue;
        }

        Metadata metadata;
        {
            MutexLock lock(pdMS_TO_TICKS(100));
            if (!lock.locked() || !g_snapshot.pending) {
                continue;
            }
            metadata = g_pendingMetadata;
            clearPendingLocked();
        }
        performOta(metadata);
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

}  // namespace

esp_err_t start() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return setLastError(ESP_ERR_TIMEOUT);
        }
        if (g_snapshot.started) {
            return ESP_OK;
        }
        g_snapshot.started = true;
        g_snapshot.progress = -1;

        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
        if (running != nullptr && esp_ota_get_state_partition(running, &otaState) == ESP_OK &&
            otaState == ESP_OTA_IMG_PENDING_VERIFY) {
            g_snapshot.rollbackPending = true;
            g_rollbackPossible = esp_ota_check_rollback_is_possible();
            g_rollbackDeadlineUs = esp_timer_get_time() + static_cast<uint64_t>(kHealthConfirmTimeoutMs) * 1000ULL;
            setReasonLocked("pending_verify");
            LOG_W(TAG_OTA_IDF, "Running app is pending OTA verification; waiting for MQTT health confirmation");
        }
    }

    err = AppIdfTasks::createPinnedToCoreInternal(otaTask,
                                                  "IDF_OTA",
                                                  kOtaTaskStackWords,
                                                  nullptr,
                                                  3,
                                                  1,
                                                  &g_otaTaskMemory);
    if (err != ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.started = false;
        }
        return setLastError(err);
    }
    return setLastError(ESP_OK);
}

bool isStarted() {
    return g_snapshot.started;
}

bool isBusy() {
    return g_snapshot.busy;
}

void cancel(const char* reason) {
    g_cancelRequested = true;
    LOG_W(TAG_OTA_IDF, "OTA cancel requested: %s", reason != nullptr ? reason : "unspecified");
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

uint32_t taskStackHighWatermark() {
    if (g_otaTaskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_otaTaskMemory.handle);
}

esp_err_t handlePreflightRequest(const char* json) {
    AppIdfPowerSave::notifyActivity();
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* requestId = cjsonString(root, "request_id");
    const char* targetVersion = cjsonString(root, "target_version");
    const int minBatteryPct = cjsonInt(root, "min_battery_pct", 0);
    const bool requireCharging = cjsonBool(root, "require_charging", false);

    bool recording = false;
    bool playing = false;
    bool otaBusy = false;
    const char* reason = "ready";
    const bool ok = evaluateReadiness(targetVersion, minBatteryPct, requireCharging, &reason, &recording, &playing, &otaBusy);
    if (ok) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            copyText(g_lastPreflightOkRequestId, sizeof(g_lastPreflightOkRequestId), requestId);
        }
        // preflight 通过意味着服务端马上会发 OTA notify 启动下载,提前唤醒屏幕和 CPU,
        // 让用户看到 OTA 进度页,同时跳出 WiFi 省电以加快下载。被拒的 preflight 不唤醒,
        // 避免开放批次期间未充电设备每 5 分钟心跳被亮屏一次。
        (void)AppIdfPowerSave::exitL1();
    }

    const esp_err_t publishErr =
        AppIdfMqtt::publishOtaPreflightAck(requestId, targetVersion, ok, reason, recording, playing, otaBusy);
    LOG_I(TAG_OTA_IDF, "OTA preflight request=%s target=%s ok=%d reason=%s",
          requestId,
          targetVersion,
          ok ? 1 : 0,
          reason);
    cJSON_Delete(root);
    return publishErr;
}

esp_err_t handleNotify(const char* json) {
    AppIdfPowerSave::notifyActivity();
    // OTA notify 一定走到下载,无条件唤醒(preflight 已 exitL1 时这里幂等)。
    (void)AppIdfPowerSave::exitL1();
    Metadata metadata;
    if (!parseNotifyMetadata(json, &metadata)) {
        AppIdfMqtt::publishOtaResult("failed", "", "", "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char* invalid = validateMetadata(metadata);
    if (invalid != nullptr) {
        AppIdfMqtt::publishOtaResult("failed", metadata.version, metadata.requestId, invalid);
        return ESP_ERR_INVALID_ARG;
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(metadata.requestId, g_lastPreflightOkRequestId) != 0) {
            AppIdfMqtt::publishOtaResult("failed", metadata.version, metadata.requestId, "Preflight mismatch");
            return ESP_ERR_INVALID_STATE;
        }
        if (g_snapshot.busy || g_snapshot.pending) {
            AppIdfMqtt::publishOtaResult("failed", metadata.version, metadata.requestId, "ota_busy");
            return ESP_ERR_INVALID_STATE;
        }
        g_pendingMetadata = metadata;
        g_snapshot.pending = true;
        g_snapshot.progress = 0;
        g_snapshot.expectedSize = metadata.size;
        g_snapshot.writtenSize = 0;
        copyText(g_snapshot.version, sizeof(g_snapshot.version), metadata.version);
        copyText(g_snapshot.requestId, sizeof(g_snapshot.requestId), metadata.requestId);
        setReasonLocked("scheduled");
    }

    if (g_otaTaskMemory.handle == nullptr) {
        AppIdfMqtt::publishOtaResult("failed", metadata.version, metadata.requestId, "OTA task unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotifyGive(g_otaTaskMemory.handle);
    LOG_I(TAG_OTA_IDF, "OTA scheduled: version=%s size=%u request=%s force=%d",
          metadata.version,
          static_cast<unsigned>(metadata.size),
          metadata.requestId,
          metadata.force ? 1 : 0);
    return ESP_OK;
}

esp_err_t confirmRunningApp(const char* reason) {
    bool shouldConfirm = false;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        shouldConfirm = g_snapshot.rollbackPending;
    }
    if (!shouldConfirm) {
        return ESP_OK;
    }

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        if (err == ESP_OK) {
            g_snapshot.rollbackPending = false;
            g_rollbackDeadlineUs = 0;
            setReasonLocked("verified");
        } else {
            setReasonLocked("verify_failed");
        }
        setLastError(err);
    }
    if (err == ESP_OK) {
        LOG_I(TAG_OTA_IDF, "OTA health confirmed by %s", reason != nullptr ? reason : "unknown");
    } else {
        LOG_W(TAG_OTA_IDF, "OTA health confirm failed: %s", esp_err_to_name(err));
    }
    return err;
}

}  // namespace AppIdfOta
