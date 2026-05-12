#include "App_IdfIr.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "App_FlashGuard.h"
#include "App_IdfFilesystem.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "Pin_Config.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace AppIdfIr {
namespace {

// 使用 App_Log.h 全局定义的 TAG_IR 宏（值 "IR"），不再重复定义。
// 用户运行期数据走独立的 userdata 分区，避开 spiffs 资源分区被 idf.py flash 覆盖的副作用。
constexpr const char* kIrJsonPath = "/userdata/ir_codes.json";

constexpr uint32_t kRMTResolutionHz = 1000000UL;
constexpr uint16_t kRxSymbolBufSize = 512;
constexpr uint16_t kRawBufSize = 1024;
constexpr uint16_t kMinRawLen = 10;
constexpr uint32_t kLearnSecondPressTimeoutMs = 8000;
constexpr uint32_t kIrTaskStackWords = 4096;

AppIdfTasks::StaticTaskMemory g_taskMemory;
SemaphoreHandle_t g_mutex = nullptr;
QueueHandle_t g_learnQueue = nullptr;
bool g_started = false;

rmt_channel_handle_t g_txChannel = nullptr;
rmt_channel_handle_t g_rxChannel = nullptr;
rmt_encoder_handle_t g_copyEncoder = nullptr;

DRAM_ATTR rmt_symbol_word_t g_rxSymbols[kRxSymbolBufSize];
volatile bool g_rxDataReady = false;
volatile size_t g_rxNumSymbols = 0;
uint16_t* g_rawData = nullptr;
uint16_t* g_firstRawData = nullptr;

bool g_isLearning = false;
char g_currentLearnName[kMaxNameLen] = {};
bool g_hasFirstCapture = false;
uint16_t g_firstRawLen = 0;
uint32_t g_firstCaptureMs = 0;
bool g_receiveBuffersReady = false;

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
    bool locked() const { return _locked; }

private:
    bool _locked = false;
};

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

bool IRAM_ATTR onRxDone(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_ctx) {
    (void)channel;
    (void)user_ctx;
    g_rxNumSymbols = edata->num_symbols;
    g_rxDataReady = true;
    return false;
}

bool allocateReceiveBuffers() {
    if (g_receiveBuffersReady) {
        return true;
    }

    const size_t rawBytes = kRawBufSize * sizeof(uint16_t);
    g_rawData = static_cast<uint16_t*>(heap_caps_malloc(rawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_firstRawData = static_cast<uint16_t*>(heap_caps_malloc(rawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_rawData == nullptr || g_firstRawData == nullptr) {
        LOG_E(TAG_IR, "IR rx buffers alloc failed (PSRAM)");
        if (g_rawData != nullptr) {
            heap_caps_free(g_rawData);
            g_rawData = nullptr;
        }
        if (g_firstRawData != nullptr) {
            heap_caps_free(g_firstRawData);
            g_firstRawData = nullptr;
        }
        return false;
    }
    memset(g_rawData, 0, rawBytes);
    memset(g_firstRawData, 0, rawBytes);
    g_receiveBuffersReady = true;
    LOG_I(TAG_IR, "IR rx buffers ready in PSRAM (%u bytes)", static_cast<unsigned>(rawBytes * 2));
    return true;
}

void startReceiving() {
    if (g_rxChannel == nullptr || !g_receiveBuffersReady) {
        return;
    }
    rmt_receive_config_t recv_cfg = {};
    recv_cfg.signal_range_min_ns = 1250;
    recv_cfg.signal_range_max_ns = 20000000ULL;
    esp_err_t err = rmt_receive(g_rxChannel, g_rxSymbols, sizeof(g_rxSymbols), &recv_cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        // 兜底：rmt_transmit 之后 RX 偶尔会进入 not-enabled 态，复位一次再重试。
        rmt_disable(g_rxChannel);
        const esp_err_t enErr = rmt_enable(g_rxChannel);
        if (enErr != ESP_OK) {
            LOG_W(TAG_IR, "rmt_enable retry failed: %s", esp_err_to_name(enErr));
            return;
        }
        err = rmt_receive(g_rxChannel, g_rxSymbols, sizeof(g_rxSymbols), &recv_cfg);
    }
    if (err != ESP_OK) {
        LOG_W(TAG_IR, "rmt_receive start failed: %s", esp_err_to_name(err));
    }
}

uint32_t computeHash(const uint16_t* rawData, uint16_t rawLen) {
    uint32_t hash = 5381;
    const uint16_t limit = (rawLen < 20) ? rawLen : 20;
    for (uint16_t i = 0; i < limit; i++) {
        hash = ((hash << 5) + hash) + rawData[i];
    }
    return hash;
}

bool isSimilarRaw(const uint16_t* lhs, uint16_t lhsLen, const uint16_t* rhs, uint16_t rhsLen) {
    if (lhsLen == 0 || rhsLen == 0) {
        return false;
    }
    // 末尾 stop bit / 重复码截断可能差 1~2 个 symbol，超过 2 直接拒。
    const uint16_t lenDiff = (lhsLen > rhsLen) ? (lhsLen - rhsLen) : (rhsLen - lhsLen);
    if (lenDiff > 2) {
        return false;
    }

    // 节拍单位：取两次抓取里的最短有效脉冲（NEC 约 562us）作为 base。
    // 量化后每个 symbol = round(symbol / base) 节拍数，硬件 ±几十 us 抖动吸收进 round 误差，
    // 但任一 bit 翻转（短脉冲 1 节拍 vs 长脉冲 3 节拍）必然漏出。
    uint16_t baseL = 0xFFFF, baseR = 0xFFFF;
    for (uint16_t i = 0; i < lhsLen; i++) {
        if (lhs[i] >= 100 && lhs[i] < baseL) baseL = lhs[i];
    }
    for (uint16_t i = 0; i < rhsLen; i++) {
        if (rhs[i] >= 100 && rhs[i] < baseR) baseR = rhs[i];
    }
    if (baseL == 0xFFFF || baseR == 0xFFFF) {
        return false;
    }
    const uint16_t base = static_cast<uint16_t>((static_cast<uint32_t>(baseL) + baseR) / 2);
    if (base == 0) {
        return false;
    }
    const uint16_t halfBase = base / 2;

    const uint16_t cmp = (lhsLen < rhsLen) ? lhsLen : rhsLen;
    for (uint16_t i = 0; i < cmp; i++) {
        const uint16_t qL = static_cast<uint16_t>((lhs[i] + halfBase) / base);
        const uint16_t qR = static_cast<uint16_t>((rhs[i] + halfBase) / base);
        if (qL != qR) {
            return false;
        }
    }
    return true;
}

// 把整个 /littlefs/ir_codes.json 读到 PSRAM 字符串。文件不存在返回 nullptr 但不视为错误。
char* readJsonFile() {
    struct stat st = {};
    if (stat(kIrJsonPath, &st) != 0) {
        return nullptr;
    }
    if (st.st_size <= 0) {
        return nullptr;
    }
    const size_t size = static_cast<size_t>(st.st_size);
    char* buf = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        LOG_E(TAG_IR, "ir_codes.json buffer alloc failed (%u bytes)", static_cast<unsigned>(size));
        return nullptr;
    }
    FILE* fp = fopen(kIrJsonPath, "rb");
    if (fp == nullptr) {
        heap_caps_free(buf);
        return nullptr;
    }
    const size_t read = fread(buf, 1, size, fp);
    fclose(fp);
    buf[read] = '\0';
    if (read != size) {
        LOG_W(TAG_IR, "ir_codes.json short read: %u/%u", static_cast<unsigned>(read), static_cast<unsigned>(size));
    }
    return buf;
}

cJSON* loadIrRoot() {
    char* json = readJsonFile();
    if (json == nullptr) {
        return cJSON_CreateObject();
    }
    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr || !cJSON_IsObject(root)) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        LOG_W(TAG_IR, "ir_codes.json parse failed, recreating empty");
        return cJSON_CreateObject();
    }
    return root;
}

bool writeIrRoot(cJSON* root) {
    char* serialized = cJSON_PrintUnformatted(root);
    if (serialized == nullptr) {
        LOG_E(TAG_IR, "ir_codes.json serialize failed");
        return false;
    }

    ScopedFlashGuard flashGuard("IR codes save", 5000);
    if (!flashGuard.ok()) {
        cJSON_free(serialized);
        LOG_W(TAG_IR, "save denied: flash guard busy");
        return false;
    }

    FILE* fp = fopen(kIrJsonPath, "wb");
    if (fp == nullptr) {
        cJSON_free(serialized);
        LOG_E(TAG_IR, "open ir_codes.json for write failed");
        return false;
    }
    const size_t len = strlen(serialized);
    const size_t written = fwrite(serialized, 1, len, fp);
    fclose(fp);
    cJSON_free(serialized);
    if (written != len) {
        LOG_E(TAG_IR, "ir_codes.json short write: %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(len));
        return false;
    }
    return true;
}

bool saveRawIrLocked(const char* name, const uint16_t* rawData, uint16_t rawLen) {
    LOG_I(TAG_IR, "saving IR code: name=%s len=%u", name, static_cast<unsigned>(rawLen));

    cJSON* root = loadIrRoot();
    if (root == nullptr) {
        return false;
    }

    cJSON* entry = cJSON_CreateObject();
    cJSON* rawArray = cJSON_CreateArray();
    if (entry == nullptr || rawArray == nullptr) {
        if (entry != nullptr) cJSON_Delete(entry);
        if (rawArray != nullptr) cJSON_Delete(rawArray);
        cJSON_Delete(root);
        return false;
    }
    for (uint16_t i = 0; i < rawLen; i++) {
        cJSON_AddItemToArray(rawArray, cJSON_CreateNumber(rawData[i]));
    }
    cJSON_AddItemToObject(entry, "raw", rawArray);

    cJSON_DeleteItemFromObjectCaseSensitive(root, name);
    cJSON_AddItemToObject(root, name, entry);

    const bool ok = writeIrRoot(root);
    cJSON_Delete(root);
    if (ok) {
        LOG_I(TAG_IR, "IR code saved: %s", name);
    }
    return ok;
}

bool sendRawData(const uint16_t* rawData, uint16_t rawLen) {
    if (g_txChannel == nullptr || g_copyEncoder == nullptr || rawLen == 0) {
        return false;
    }
    const uint16_t numWords = (rawLen + 1) / 2;
    rmt_symbol_word_t* symbols = static_cast<rmt_symbol_word_t*>(
        heap_caps_calloc(numWords, sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (symbols == nullptr) {
        LOG_E(TAG_IR, "RMT symbol buffer alloc failed");
        return false;
    }
    for (uint16_t i = 0; i < rawLen; i++) {
        const uint16_t wordIdx = i / 2;
        const uint16_t duration = rawData[i];
        if ((i % 2) == 0) {
            symbols[wordIdx].duration0 = duration;
            symbols[wordIdx].level0 = 1;
        } else {
            symbols[wordIdx].duration1 = duration;
            symbols[wordIdx].level1 = 0;
        }
    }
    // 发送前停 RX：避免自家 IR LED 发射光被自家接收头收到造成 self-receive，
    // 也保证 ESP-IDF 5.x RMT 内部状态机在 transmit 期间 RX 不会进非 enable 态。
    if (g_rxChannel != nullptr) {
        rmt_disable(g_rxChannel);
    }

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    const esp_err_t ret =
        rmt_transmit(g_txChannel, g_copyEncoder, symbols, static_cast<size_t>(numWords) * sizeof(rmt_symbol_word_t),
                     &tx_cfg);
    if (ret == ESP_OK) {
        rmt_tx_wait_all_done(g_txChannel, 500);
        LOG_I(TAG_IR, "RMT transmit ok: %u words", static_cast<unsigned>(numWords));
    } else {
        LOG_E(TAG_IR, "RMT transmit failed: %s", esp_err_to_name(ret));
    }

    // 发送完成后重新 enable RX，并清掉可能误置的 ready 标记。
    // 后续 startReceiving 才能成功调到 rmt_receive。
    if (g_rxChannel != nullptr) {
        const esp_err_t enErr = rmt_enable(g_rxChannel);
        if (enErr != ESP_OK) {
            LOG_W(TAG_IR, "rmt_enable RX after tx failed: %s", esp_err_to_name(enErr));
        }
        g_rxDataReady = false;
    }

    heap_caps_free(symbols);
    return ret == ESP_OK;
}

bool loadAndSendIrLocked(const char* name, int repeatCount) {
    char* json = readJsonFile();
    if (json == nullptr) {
        LOG_W(TAG_IR, "ir_codes.json missing");
        return false;
    }
    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr) {
        LOG_E(TAG_IR, "ir_codes.json parse failed");
        return false;
    }
    cJSON* entry = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsObject(entry)) {
        LOG_W(TAG_IR, "IR command not found: %s", name);
        cJSON_Delete(root);
        return false;
    }
    cJSON* rawArray = cJSON_GetObjectItemCaseSensitive(entry, "raw");
    if (!cJSON_IsArray(rawArray)) {
        LOG_E(TAG_IR, "IR command has no raw array: %s", name);
        cJSON_Delete(root);
        return false;
    }
    const int rawLen = cJSON_GetArraySize(rawArray);
    if (rawLen <= 0 || rawLen > kRawBufSize) {
        LOG_E(TAG_IR, "IR command raw size invalid: %d", rawLen);
        cJSON_Delete(root);
        return false;
    }

    uint16_t* rawData =
        static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(rawLen) * sizeof(uint16_t), MALLOC_CAP_INTERNAL));
    if (rawData == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    for (int i = 0; i < rawLen; i++) {
        const cJSON* num = cJSON_GetArrayItem(rawArray, i);
        rawData[i] = static_cast<uint16_t>(cJSON_IsNumber(num) ? num->valueint : 0);
    }
    cJSON_Delete(root);

    bool sent = false;
    for (int r = 0; r < repeatCount; r++) {
        if (r > 0) {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        sent = sendRawData(rawData, static_cast<uint16_t>(rawLen));
    }
    heap_caps_free(rawData);

    vTaskDelay(pdMS_TO_TICKS(50));
    startReceiving();
    return sent;
}

void processCapturedSignal(uint16_t rawLen) {
    LearnEvent evt = {};
    evt.isAC = (rawLen > 130);
    evt.rawLen = rawLen;
    evt.stage = LearnStage::Unknown;

    const uint32_t hash = computeHash(g_rawData, rawLen);
    snprintf(evt.signatureHash, sizeof(evt.signatureHash), "RAW%04X%08" PRIx32, rawLen, hash);

    if (g_hasFirstCapture && (nowMs() - g_firstCaptureMs > kLearnSecondPressTimeoutMs)) {
        LOG_W(TAG_IR, "second-press window expired, restart learn");
        g_hasFirstCapture = false;
        g_firstRawLen = 0;
    }

    if (!g_hasFirstCapture) {
        memcpy(g_firstRawData, g_rawData, rawLen * sizeof(uint16_t));
        g_firstRawLen = rawLen;
        g_firstCaptureMs = nowMs();
        g_hasFirstCapture = true;
        evt.learningSuccess = false;
        evt.stage = LearnStage::FirstCaptured;
        LOG_I(TAG_IR, "first capture stored, awaiting confirm");
    } else if (isSimilarRaw(g_firstRawData, g_firstRawLen, g_rawData, rawLen)) {
        g_hasFirstCapture = false;
        g_firstRawLen = 0;
        if (saveRawIrLocked(g_currentLearnName, g_rawData, rawLen)) {
            g_isLearning = false;
            evt.learningSuccess = true;
            evt.stage = LearnStage::Confirmed;
            LOG_I(TAG_IR, "confirmed and saved: %s", g_currentLearnName);
        } else {
            evt.learningSuccess = false;
            evt.stage = LearnStage::SaveFailed;
            LOG_E(TAG_IR, "confirmed but save failed: %s", g_currentLearnName);
        }
    } else {
        g_hasFirstCapture = false;
        g_firstRawLen = 0;
        evt.learningSuccess = false;
        evt.stage = LearnStage::Mismatch;
        LOG_W(TAG_IR, "two captures differ, retry");
    }

    if (g_learnQueue != nullptr) {
        xQueueSend(g_learnQueue, &evt, 0);
    }
}

void irTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_IR");
    startReceiving();

    while (true) {
        AppIdfTasks::feedCurrentTaskWatchdog();
        if (!g_rxDataReady) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const size_t numSymbols = g_rxNumSymbols;
        g_rxDataReady = false;

        uint16_t rawLen = 0;
        for (size_t i = 0; i < numSymbols && rawLen + 1 < kRawBufSize; i++) {
            const rmt_symbol_word_t& s = g_rxSymbols[i];
            if (s.duration0 > 0) g_rawData[rawLen++] = static_cast<uint16_t>(s.duration0);
            if (s.duration1 > 0) g_rawData[rawLen++] = static_cast<uint16_t>(s.duration1);
        }
        if (rawLen < kMinRawLen) {
            startReceiving();
            continue;
        }

        {
            MutexLock lock;
            if (lock.locked() && g_isLearning) {
                processCapturedSignal(rawLen);
            } else {
                LOG_D(TAG_IR, "rx ignored (not learning): %u elements", static_cast<unsigned>(rawLen));
            }
        }
        startReceiving();
    }
}

esp_err_t initRmt() {
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = static_cast<gpio_num_t>(PIN_IR_TX);
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = kRMTResolutionHz;
    tx_cfg.mem_block_symbols = 48;
    tx_cfg.trans_queue_depth = 4;

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &g_txChannel);
    if (err != ESP_OK) {
        LOG_E(TAG_IR, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_carrier_config_t carrier_cfg = {};
    carrier_cfg.frequency_hz = 38000;
    carrier_cfg.duty_cycle = 0.33f;
    rmt_apply_carrier(g_txChannel, &carrier_cfg);

    rmt_copy_encoder_config_t copy_enc_cfg = {};
    err = rmt_new_copy_encoder(&copy_enc_cfg, &g_copyEncoder);
    if (err != ESP_OK) {
        LOG_E(TAG_IR, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return err;
    }
    rmt_enable(g_txChannel);

    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = static_cast<gpio_num_t>(PIN_IR_RX);
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = kRMTResolutionHz;
    rx_cfg.mem_block_symbols = 128;
    rx_cfg.flags.with_dma = true;

    err = rmt_new_rx_channel(&rx_cfg, &g_rxChannel);
    if (err != ESP_OK) {
        LOG_E(TAG_IR, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_rx_event_callbacks_t rx_cbs = {};
    rx_cbs.on_recv_done = onRxDone;
    rmt_rx_register_event_callbacks(g_rxChannel, &rx_cbs, nullptr);
    rmt_enable(g_rxChannel);
    return ESP_OK;
}

}  // namespace

esp_err_t start() {
    if (g_started) {
        return ESP_OK;
    }
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    if (g_learnQueue == nullptr) {
        g_learnQueue = xQueueCreate(8, sizeof(LearnEvent));
    }
    if (g_mutex == nullptr || g_learnQueue == nullptr) {
        LOG_E(TAG_IR, "mutex/queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    LOG_I(TAG_IR, "starting IR module on RX=%d TX=%d", PIN_IR_RX, PIN_IR_TX);
    const esp_err_t rmtErr = initRmt();
    if (rmtErr != ESP_OK) {
        return rmtErr;
    }
    if (!allocateReceiveBuffers()) {
        LOG_W(TAG_IR, "rx buffers unavailable, send-only mode");
    }

    const esp_err_t taskErr = AppIdfTasks::createPinnedToCoreInternal(irTask, "IDF_IR", kIrTaskStackWords, nullptr, 2,
                                                                     1, &g_taskMemory);
    if (taskErr != ESP_OK) {
        LOG_E(TAG_IR, "IR task creation failed: %s", esp_err_to_name(taskErr));
        return taskErr;
    }
    g_started = true;
    LOG_I(TAG_IR, "IR module ready");
    return ESP_OK;
}

bool isStarted() { return g_started; }

bool isLearning() {
    MutexLock lock;
    return lock.locked() && g_isLearning;
}

const char* currentLearnName() { return g_currentLearnName; }

esp_err_t startLearning(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    MutexLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    if (!g_receiveBuffersReady) {
        LOG_W(TAG_IR, "rx buffers not ready, cannot learn");
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(g_currentLearnName, name, sizeof(g_currentLearnName) - 1);
    g_currentLearnName[sizeof(g_currentLearnName) - 1] = '\0';
    g_isLearning = true;
    g_hasFirstCapture = false;
    g_firstRawLen = 0;
    g_firstCaptureMs = 0;
    LOG_I(TAG_IR, "learning started: %s", g_currentLearnName);
    return ESP_OK;
}

void stopLearning() {
    MutexLock lock;
    if (!lock.locked()) {
        return;
    }
    g_isLearning = false;
    g_currentLearnName[0] = '\0';
    g_hasFirstCapture = false;
    g_firstRawLen = 0;
    g_firstCaptureMs = 0;
    LOG_I(TAG_IR, "learning stopped");
}

bool sendLearned(const char* name, int repeatCount) {
    if (name == nullptr || name[0] == '\0' || repeatCount <= 0) {
        return false;
    }
    MutexLock lock;
    if (!lock.locked()) {
        return false;
    }
    return loadAndSendIrLocked(name, repeatCount);
}

esp_err_t clearAllLearned() {
    MutexLock lock;
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    struct stat st = {};
    if (stat(kIrJsonPath, &st) != 0) {
        LOG_D(TAG_IR, "ir_codes.json absent, nothing to clear");
        return ESP_OK;
    }

    ScopedFlashGuard flashGuard("IR codes clear", 5000);
    if (!flashGuard.ok()) {
        return ESP_ERR_TIMEOUT;
    }
    if (unlink(kIrJsonPath) != 0) {
        LOG_E(TAG_IR, "unlink ir_codes.json failed");
        return ESP_FAIL;
    }
    LOG_I(TAG_IR, "ir_codes.json cleared");
    return ESP_OK;
}

size_t listLearnedNames(char names[][kMaxNameLen], size_t maxNames) {
    if (names == nullptr || maxNames == 0) {
        return 0;
    }
    MutexLock lock;
    if (!lock.locked()) {
        return 0;
    }
    char* json = readJsonFile();
    if (json == nullptr) {
        return 0;
    }
    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr || !cJSON_IsObject(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return 0;
    }
    size_t count = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (count >= maxNames) {
            break;
        }
        const char* name = item->string;
        if (name == nullptr) {
            continue;
        }
        strncpy(names[count], name, kMaxNameLen - 1);
        names[count][kMaxNameLen - 1] = '\0';
        ++count;
    }
    cJSON_Delete(root);
    return count;
}

QueueHandle_t learnEventQueue() { return g_learnQueue; }

uint32_t taskStackHighWatermark() {
    if (g_taskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_taskMemory.handle);
}

}  // namespace AppIdfIr
