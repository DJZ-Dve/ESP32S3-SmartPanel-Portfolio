#include "App_IdfServer.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfScene.h"
#include "App_IdfCommandExecutor.h"
#include "App_IdfLearnFlow.h"
#include "App_IdfCellular.h"
#include "App_IdfMqtt.h"
#include "App_IdfNetwork.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace AppIdfServer {
namespace {

constexpr const char* TAG_SERVER_IDF = "IDF_SERVER";
constexpr uint32_t kSocketTimeoutSec = 10;
constexpr uint32_t kResponseTimeoutMs = 30000;
constexpr size_t kResponseHexMax = 8192;
constexpr size_t kJsonMax = kResponseHexMax / 2;
constexpr size_t kIoChunkSize = 1024;

SemaphoreHandle_t g_mutex = nullptr;
Snapshot g_snapshot;
// 由 uploadPcmAndReceive 入口暂存调用方传入的 outCuePlayed 指针,
// 内部 receiveResponse 在解析完 executor 结果后写回。
// 同一时间只有一路语音上传(recorder 串行),无需互斥。
bool* g_currentCuePlayedOut = nullptr;

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

class SocketGuard {
public:
    ~SocketGuard() {
        if (fd >= 0) {
            close(fd);
        }
    }

    int fd = -1;
};

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
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

esp_err_t setError(esp_err_t err, const char* message) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.lastEspError = err;
        copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), message != nullptr ? message : "");
    }
    return err;
}

uint8_t hexCharToVal(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<uint8_t>(c - 'A' + 10);
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<uint8_t>(c - 'a' + 10);
    }
    return 0;
}

void writeU32Be(uint32_t value, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

bool writeAll(int fd, const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t writtenTotal = 0;
    while (writtenTotal < len) {
        size_t chunk = len - writtenTotal;
        if (chunk > kIoChunkSize) {
            chunk = kIoChunkSize;
        }
        const ssize_t written = send(fd, bytes + writtenTotal, chunk, 0);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        writtenTotal += static_cast<size_t>(written);
        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

esp_err_t connectSocket(SocketGuard* socketGuard) {
    if (socketGuard == nullptr || (!AppIdfNetwork::isConnected() && !AppIdfCellular::isReadyForMqtt())) {
        return setError(ESP_ERR_INVALID_STATE, "network_disconnected");
    }

    char host[sizeof(g_snapshot.host)];
    uint16_t port = 0;
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return setError(ESP_ERR_TIMEOUT, "mutex_timeout");
        }
        copyText(host, sizeof(host), g_snapshot.host);
        port = g_snapshot.port;
    }
    if (host[0] == '\0' || port == 0) {
        return setError(ESP_ERR_INVALID_STATE, "server_not_configured");
    }

    char portText[8];
    snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const int gai = getaddrinfo(host, portText, &hints, &result);
    if (gai != 0 || result == nullptr) {
        return setError(ESP_FAIL, "dns_failed");
    }

    esp_err_t lastErr = ESP_FAIL;
    for (struct addrinfo* item = result; item != nullptr; item = item->ai_next) {
        const int fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            lastErr = ESP_FAIL;
            continue;
        }

        struct timeval timeout = {};
        timeout.tv_sec = kSocketTimeoutSec;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            socketGuard->fd = fd;
            freeaddrinfo(result);
            MutexLock lock(pdMS_TO_TICKS(100));
            if (lock.locked()) {
                ++g_snapshot.connectCount;
                g_snapshot.lastProbeOk = true;
                g_snapshot.lastEspError = ESP_OK;
                g_snapshot.lastError[0] = '\0';
            }
            return ESP_OK;
        }

        close(fd);
        lastErr = ESP_FAIL;
    }

    freeaddrinfo(result);
    return setError(lastErr, "connect_failed");
}

esp_err_t sendIdentity(int fd) {
    char deviceId[24];
    copyText(deviceId, sizeof(deviceId), AppIdfMqtt::deviceId());
    if (deviceId[0] == '\0') {
        copyText(deviceId, sizeof(deviceId), "UNKNOWN");
    }

    const uint32_t idLen = static_cast<uint32_t>(strlen(deviceId));
    uint8_t lenBuf[4];
    writeU32Be(idLen, lenBuf);
    if (!writeAll(fd, lenBuf, sizeof(lenBuf)) || !writeAll(fd, deviceId, idLen)) {
        return setError(ESP_FAIL, "identity_write_failed");
    }

    const AppIdfAppMode::Mode appMode = AppIdfAppMode::current();

    // product_id 跟随 appMode：服务端按 product_id 加载对应 product 目录的 prompt + adapter，
    // 实现 BLE/IR/RF433 三个产品彻底解耦（避免 IR 设备收到 aircon_ble_v1 指令）。
    const char* productId = "esp32s3_ble_aircon";
    switch (appMode) {
        case AppIdfAppMode::Mode::IR:    productId = "esp32s3_ir_panel"; break;
        case AppIdfAppMode::Mode::RF433: productId = "esp32s3_rf433_panel"; break;
        case AppIdfAppMode::Mode::BLE:
        default:                         productId = "esp32s3_ble_aircon"; break;
    }

    // scene_labels：当前模式可见场景的 [{"id":..,"label":..},..]；BLE 模式返回 "[]"。
    char sceneLabels[512];
    AppIdfScene::labelsForServerMeta(sceneLabels, sizeof(sceneLabels));
    if (sceneLabels[0] == '\0') {
        sceneLabels[0] = '[';
        sceneLabels[1] = ']';
        sceneLabels[2] = '\0';
    }

    // pending_signal：仅 LearnFlow AwaitingLabel/UploadingLabel 状态非空。
    char pendingSignal[160];
    AppIdfLearnFlow::describePendingForMeta(pendingSignal, sizeof(pendingSignal));

    char metaJson[1024];
    int metaWritten = 0;
    if (pendingSignal[0] != '\0') {
        metaWritten = snprintf(
            metaJson, sizeof(metaJson),
            "{\"product_id\":\"%s\",\"profile\":\"%s\","
            "\"capabilities\":{\"aircon_ble\":%s,\"ir\":%s,\"rf433\":%s,\"scenes\":%s},"
            "\"scene_labels\":%s,\"pending_signal\":%s,\"idf\":true}",
            productId,
            AppIdfAppMode::nameAscii(appMode),
            appMode == AppIdfAppMode::Mode::BLE ? "true" : "false",
            appMode == AppIdfAppMode::Mode::IR ? "true" : "false",
            appMode == AppIdfAppMode::Mode::RF433 ? "true" : "false",
            AppIdfScene::isStarted() ? "true" : "false",
            sceneLabels, pendingSignal);
    } else {
        metaWritten = snprintf(
            metaJson, sizeof(metaJson),
            "{\"product_id\":\"%s\",\"profile\":\"%s\","
            "\"capabilities\":{\"aircon_ble\":%s,\"ir\":%s,\"rf433\":%s,\"scenes\":%s},"
            "\"scene_labels\":%s,\"idf\":true}",
            productId,
            AppIdfAppMode::nameAscii(appMode),
            appMode == AppIdfAppMode::Mode::BLE ? "true" : "false",
            appMode == AppIdfAppMode::Mode::IR ? "true" : "false",
            appMode == AppIdfAppMode::Mode::RF433 ? "true" : "false",
            AppIdfScene::isStarted() ? "true" : "false",
            sceneLabels);
    }
    if (metaWritten <= 0 || metaWritten >= static_cast<int>(sizeof(metaJson))) {
        return setError(ESP_FAIL, "meta_format_failed");
    }
    constexpr uint8_t kMetaMagic[4] = {'M', 'E', 'T', 'A'};
    const uint32_t metaLen = static_cast<uint32_t>(metaWritten);
    writeU32Be(metaLen, lenBuf);
    if (!writeAll(fd, kMetaMagic, sizeof(kMetaMagic)) || !writeAll(fd, lenBuf, sizeof(lenBuf)) ||
        !writeAll(fd, metaJson, metaLen)) {
        return setError(ESP_FAIL, "meta_write_failed");
    }

    LOG_I(TAG_SERVER_IDF, "Server identity sent: %s", deviceId);
    return ESP_OK;
}

esp_err_t sendAudio(int fd, const int16_t* pcm, size_t sampleCount) {
    const uint32_t audioBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
    LOG_I(TAG_SERVER_IDF, "[LAT] upload_send_start bytes=%u", static_cast<unsigned>(audioBytes));
    uint8_t lenBuf[4];
    writeU32Be(audioBytes, lenBuf);
    if (!writeAll(fd, lenBuf, sizeof(lenBuf))) {
        return setError(ESP_FAIL, "audio_len_write_failed");
    }
    if (audioBytes > 0 && !writeAll(fd, pcm, audioBytes)) {
        return setError(ESP_FAIL, "audio_write_failed");
    }
    LOG_I(TAG_SERVER_IDF, "[LAT] upload_send_done bytes=%u", static_cast<unsigned>(audioBytes));
    return ESP_OK;
}

esp_err_t receiveResponse(int fd) {
    char* hex = static_cast<char*>(heap_caps_malloc(kResponseHexMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (hex == nullptr) {
        hex = static_cast<char*>(heap_caps_malloc(kResponseHexMax + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    char* json = static_cast<char*>(heap_caps_malloc(kJsonMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (json == nullptr) {
        json = static_cast<char*>(heap_caps_malloc(kJsonMax + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (hex == nullptr || json == nullptr) {
        if (hex != nullptr) {
            heap_caps_free(hex);
        }
        if (json != nullptr) {
            heap_caps_free(json);
        }
        return setError(ESP_ERR_NO_MEM, "response_buffer_alloc_failed");
    }

    size_t hexLen = 0;
    bool done = false;
    bool firstByteLogged = false;
    const int64_t deadlineUs = esp_timer_get_time() + static_cast<int64_t>(kResponseTimeoutMs) * 1000;
    while (esp_timer_get_time() < deadlineUs && !done) {
        uint8_t buffer[256];
        const ssize_t readLen = recv(fd, buffer, sizeof(buffer), 0);
        if (readLen < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            heap_caps_free(hex);
            heap_caps_free(json);
            return setError(ESP_FAIL, "response_read_failed");
        }
        if (readLen == 0) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!firstByteLogged) {
            firstByteLogged = true;
            LOG_I(TAG_SERVER_IDF, "[LAT] resp_first_byte bytes=%d", static_cast<int>(readLen));
        }

        for (ssize_t i = 0; i < readLen; ++i) {
            const char c = static_cast<char>(buffer[i]);
            if (c == '*') {
                done = true;
                break;
            }
            if (c == '\n' || c == '\r') {
                continue;
            }
            if (hexLen >= kResponseHexMax) {
                heap_caps_free(hex);
                heap_caps_free(json);
                return setError(ESP_ERR_INVALID_SIZE, "response_too_large");
            }
            hex[hexLen++] = c;
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
    hex[hexLen] = '\0';

    if (!done) {
        heap_caps_free(hex);
        heap_caps_free(json);
        return setError(ESP_ERR_TIMEOUT, "response_timeout");
    }
    if ((hexLen % 2) != 0) {
        heap_caps_free(hex);
        heap_caps_free(json);
        return setError(ESP_ERR_INVALID_SIZE, "response_hex_odd");
    }

    const size_t jsonLen = hexLen / 2;
    for (size_t i = 0; i < jsonLen; ++i) {
        json[i] = static_cast<char>((hexCharToVal(hex[i * 2]) << 4) | hexCharToVal(hex[i * 2 + 1]));
    }
    json[jsonLen] = '\0';
    LOG_I(TAG_SERVER_IDF, "[LAT] resp_full json_len=%u", static_cast<unsigned>(jsonLen));

    AppIdfCommandExecutor::Result result;
    const esp_err_t execErr = AppIdfCommandExecutor::executeControlJson(json, &result);

    // Voice-path fallback: if the JSON parsed but produced neither a device
    // command nor any audible cue, surface "我没听清" so the user always gets
    // a response instead of awkward silence (typical when the AI is uncertain
    // or the upload contained garbled audio).
    if (execErr == ESP_OK && !result.handled && !result.cuePlayed) {
        const esp_err_t cueErr = AppIdfAudio::playLocalCue("not_understood", 8000);
        if (cueErr == ESP_OK) {
            result.cuePlayed = true;
            LOG_I(TAG_SERVER_IDF, "Voice fallback: not_understood cue played");
        } else {
            LOG_W(TAG_SERVER_IDF, "Voice fallback skipped: %s", esp_err_to_name(cueErr));
        }
    }

    // 把"executor 已播 cue"信息上报给调用者,避免双 cue:
    // BLE 写失败时 executor 已播 op_failed,recorder 不应再补一句 network_failed。
    if (g_currentCuePlayedOut != nullptr) {
        *g_currentCuePlayedOut = result.cuePlayed;
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            ++g_snapshot.responseCount;
            g_snapshot.lastResponseJsonOk = true;
            g_snapshot.lastEspError = execErr;
            copyText(g_snapshot.lastSummary,
                     sizeof(g_snapshot.lastSummary),
                     result.summary[0] ? result.summary : (result.handled ? result.error : "no migrated command"));
            g_snapshot.lastError[0] = '\0';
        }
    }

    LOG_I(TAG_SERVER_IDF, "[LAT] pipeline_done command=%s",
          result.summary[0] ? result.summary : (result.handled ? result.error : "none"));
    heap_caps_free(hex);
    heap_caps_free(json);
    return execErr;
}

}  // namespace

esp_err_t start(const char* host, uint16_t port) {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setError(err, "mutex_alloc_failed");
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return setError(ESP_ERR_TIMEOUT, "mutex_timeout");
    }
    copyText(g_snapshot.host, sizeof(g_snapshot.host), host);
    g_snapshot.port = port;
    g_snapshot.started = host != nullptr && host[0] != '\0' && port != 0;
    g_snapshot.lastEspError = g_snapshot.started ? ESP_OK : ESP_ERR_INVALID_ARG;
    copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), g_snapshot.started ? "" : "invalid_server");
    return g_snapshot.lastEspError;
}

bool isStarted() {
    return g_snapshot.started;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

esp_err_t probe() {
    SocketGuard socket;
    esp_err_t err = connectSocket(&socket);
    if (err != ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.lastProbeOk = false;
        }
        return err;
    }
    err = sendIdentity(socket.fd);
    if (err == ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.lastProbeOk = true;
            copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "probe identity sent");
        }
    }
    return err;
}

esp_err_t uploadPcmAndReceive(const int16_t* pcm, size_t sampleCount, bool* outCuePlayed) {
    if (outCuePlayed != nullptr) {
        *outCuePlayed = false;
    }
    if (pcm == nullptr && sampleCount > 0) {
        return setError(ESP_ERR_INVALID_ARG, "invalid_pcm");
    }

    g_currentCuePlayedOut = outCuePlayed;
    SocketGuard socket;
    esp_err_t err = connectSocket(&socket);
    if (err != ESP_OK) {
        g_currentCuePlayedOut = nullptr;
        return err;
    }
    err = sendIdentity(socket.fd);
    if (err != ESP_OK) {
        g_currentCuePlayedOut = nullptr;
        return err;
    }
    err = sendAudio(socket.fd, pcm, sampleCount);
    if (err != ESP_OK) {
        g_currentCuePlayedOut = nullptr;
        return err;
    }
    const esp_err_t result = receiveResponse(socket.fd);
    g_currentCuePlayedOut = nullptr;
    return result;
}

}  // namespace AppIdfServer
