#include "App_IdfBleAircon.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfAppMode.h"
#include "App_IdfPowerSave.h"
#include "App_IdfTasks.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#include "services/gap/ble_svc_gap.h"
#endif

#ifdef LOG_LEVEL_ERROR
#undef LOG_LEVEL_ERROR
#endif
#ifdef LOG_LEVEL_WARN
#undef LOG_LEVEL_WARN
#endif
#ifdef LOG_LEVEL_INFO
#undef LOG_LEVEL_INFO
#endif
#ifdef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_DEBUG
#endif

#include "App_Log.h"

namespace AppIdfBleAircon {
namespace {

constexpr const char* TAG_BLE_IDF = "IDF_BLE";
constexpr const char* kPrefsNamespace = "ble_aircon";
constexpr const char* kPrefsMacKey = "target_mac";
constexpr const char* kPrefsTypeKey = "addr_type";
constexpr const char* kDefaultTargetMac = "48:87:2d:c2:61:60";
constexpr uint16_t kServiceUuid = 0xFFE0;
constexpr uint16_t kTxUuid = 0xFFE2;
constexpr uint16_t kRxUuid = 0xFFE1;          // AC echoes commands back here as NOTIFY
constexpr uint16_t kCccdUuid = 0x2902;        // CCCD descriptor that gates NOTIFY/INDICATE
constexpr uint8_t kProductCode = 0x01;
constexpr size_t kMaxFrameLen = 16;
const ble_uuid16_t kServiceUuidDef = BLE_UUID16_INIT(kServiceUuid);
const ble_uuid16_t kTxUuidDef = BLE_UUID16_INIT(kTxUuid);
const ble_uuid16_t kRxUuidDef = BLE_UUID16_INIT(kRxUuid);
const ble_uuid16_t kCccdUuidDef = BLE_UUID16_INIT(kCccdUuid);
constexpr uint32_t kEchoTimeoutMs = 800;       // upper bound observed ~< 200ms; 800ms is conservative
constexpr uint32_t kWorkerTaskStackWords = 3072;
constexpr uint32_t kScanDurationMs = 4500;
constexpr uint32_t kPairConnectTimeoutMs = 8000;
constexpr uint32_t kPairProcedureTimeoutMs = 12000;
constexpr uint32_t kControlProcedureTimeoutMs = 12000;
constexpr uint32_t kControlApiTimeoutMs = 16000;
constexpr uint32_t kWriteResponseTimeoutMs = 3000;
constexpr uint32_t kDisconnectDelayMs = 120;
// 配合 BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL,NimBLE host 走 PSRAM,
// 内部 SRAM 主要给 BT controller 的 HCI/DMA 队列用。
// 实测一次完整 BLE 写入 min_free 约 21KB(67K → 21K dip),所以门槛设
// 22K free / 14K largest——比实测峰值占用略高,挡明显不够的场景,
// 不至于把成功概率较高的边界场景也拒掉。失败立即返回 ESP_ERR_NO_MEM。
// 守门只是开发期预警阀，不是 NimBLE 的硬性下限。历史上已从 32K/20K 降到 22K/14K；
// 移植 IR/433/场景模块后 .bss/heap 进一步紧张，largest 实测稳定在 ~8K 量级，但实际
// scan/pair 能跑通（已验证）。再降到 14K free + 7K largest，留 1-2K 余量给瞬时波动。
// 2026-05-09 再降到 12K free + 6K largest：实测发现 NimBLE 一旦 initStack 即常驻
// （nimble_port_init + BT controller 的 HCI/DMA pool 不会随 cancelPairing/断连释放,
// 当前没有 nimble_port_deinit/esp_bt_controller_disable 的回收路径）。boot 后第一次
// 进按钮扫描会把 internal_free 一次性压到 ~13759 并稳定下来,导致原 14K 阈值在第二次
// 进入扫描或控制路径（runControlCommand 用同一 logBleMemory 守门）时直接 abort。
// 由于这是一次性结构性占用而非持续泄漏,降阈值不会陷入"越降越深"。下次再撞上时
// 应该考虑实现真正的 stack teardown,而不是继续往下调阈值。
constexpr uint32_t kMinBleInternalFree = 12 * 1024;
constexpr uint32_t kMinBleInternalLargest = 6 * 1024;
constexpr uint16_t kInvalidConnHandle = 0xffff;

enum class WorkerCommand : uint8_t {
    None = 0,
    Scan,
    Pair,
    Control,
};

struct PairContext {
    uint16_t connHandle = kInvalidConnHandle;
    uint16_t serviceStart = 0;
    uint16_t serviceEnd = 0;
    uint16_t txHandle = 0;        // FFE2 value handle
    uint8_t txProperties = 0;
    uint16_t rxHandle = 0;        // FFE1 value handle (0 if not present or no NOTIFY)
    uint16_t rxEndHandle = 0;     // FFE1 declaration end (for descriptor discovery range)
    uint16_t rxCccdHandle = 0;    // 0x2902 descriptor handle on FFE1
    uint8_t rxProperties = 0;
    bool notifyEnabled = false;   // true once CCCD = 0x0001 write succeeded
    int status = BLE_HS_EUNKNOWN;
    bool done = false;
};

struct ControlContext {
    uint8_t frame[kMaxFrameLen] = {};
    size_t frameLen = 0;
    esp_err_t result = ESP_FAIL;
    bool done = true;
};

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_stackSyncSemaphore = nullptr;
SemaphoreHandle_t g_scanDoneSemaphore = nullptr;
SemaphoreHandle_t g_pairDoneSemaphore = nullptr;
SemaphoreHandle_t g_controlDoneSemaphore = nullptr;
SemaphoreHandle_t g_writeDoneSemaphore = nullptr;
SemaphoreHandle_t g_echoSemaphore = nullptr;
AppIdfTasks::StaticTaskMemory g_workerTaskMemory;

bool g_started = false;
bool g_stackStarting = false;
bool g_stackReady = false;
bool g_hostTaskStarted = false;
bool g_hasStoredTarget = false;
bool g_cancelRequested = false;
bool g_disconnectAfterCommand = false;
char g_targetAddress[18] = {};
uint8_t g_targetAddressType = BLE_ADDR_PUBLIC;
char g_lastError[96] = {};
PairingState g_pairingState = PairingState::Idle;
PairScanEntry g_pairingResults[kMaxPairScanResults];
size_t g_pairingResultCount = 0;
PairScanEntry g_scanResults[kMaxPairScanResults];
size_t g_scanResultCount = 0;
size_t g_selectedPairIndex = 0;
WorkerCommand g_pendingCommand = WorkerCommand::None;
PairContext g_pairContext;
ControlContext g_controlContext;
int g_writeStatus = BLE_HS_EUNKNOWN;
bool g_ackVerificationEnabled = true;   // FFE1 echo gating; off => legacy fire-and-forget
uint8_t g_lastEcho[kMaxFrameLen] = {};
size_t g_lastEchoLen = 0;

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

void clearLastErrorLocked() {
    g_lastError[0] = '\0';
}

void setLastErrorLocked(const char* fmt, ...) {
    if (fmt == nullptr) {
        clearLastErrorLocked();
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(g_lastError, sizeof(g_lastError), fmt, args);
    va_end(args);
}

bool ensureMutexes() {
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
    }
    if (g_stackSyncSemaphore == nullptr) {
        g_stackSyncSemaphore = xSemaphoreCreateBinary();
    }
    if (g_scanDoneSemaphore == nullptr) {
        g_scanDoneSemaphore = xSemaphoreCreateBinary();
    }
    if (g_pairDoneSemaphore == nullptr) {
        g_pairDoneSemaphore = xSemaphoreCreateBinary();
    }
    if (g_controlDoneSemaphore == nullptr) {
        g_controlDoneSemaphore = xSemaphoreCreateBinary();
    }
    if (g_writeDoneSemaphore == nullptr) {
        g_writeDoneSemaphore = xSemaphoreCreateBinary();
    }
    if (g_echoSemaphore == nullptr) {
        g_echoSemaphore = xSemaphoreCreateBinary();
    }
    return g_mutex != nullptr && g_stackSyncSemaphore != nullptr && g_scanDoneSemaphore != nullptr &&
           g_pairDoneSemaphore != nullptr && g_controlDoneSemaphore != nullptr && g_writeDoneSemaphore != nullptr &&
           g_echoSemaphore != nullptr;
}

bool takeMutex(TickType_t ticks = pdMS_TO_TICKS(100)) {
    return g_mutex != nullptr && xSemaphoreTake(g_mutex, ticks) == pdTRUE;
}

void giveMutex() {
    if (g_mutex != nullptr) {
        xSemaphoreGive(g_mutex);
    }
}

void setLastError(const char* fmt, ...) {
    if (!takeMutex()) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(g_lastError, sizeof(g_lastError), fmt, args);
    va_end(args);
    giveMutex();
}

bool isValidMacString(const char* text) {
    if (text == nullptr || strlen(text) != 17) {
        return false;
    }

    for (int i = 0; i < 17; ++i) {
        const char c = text[i];
        if ((i + 1) % 3 == 0) {
            if (c != ':') {
                return false;
            }
            continue;
        }
        if (!isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool parseMacString(const char* text, ble_addr_t* out, uint8_t addressType) {
    if (!isValidMacString(text) || out == nullptr) {
        return false;
    }

    unsigned values[6] = {};
    if (sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &values[0], &values[1], &values[2], &values[3], &values[4],
               &values[5]) != 6) {
        return false;
    }

    out->type = addressType;
    for (int i = 0; i < 6; ++i) {
        out->val[5 - i] = static_cast<uint8_t>(values[i]);
    }
    return true;
}

void formatMacString(const ble_addr_t& address, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return;
    }
    snprintf(out,
             outSize,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             address.val[5],
             address.val[4],
             address.val[3],
             address.val[2],
             address.val[1],
             address.val[0]);
}

esp_err_t bleRcToEspErr(int rc) {
    switch (rc) {
        case 0:
            return ESP_OK;
        case BLE_HS_ETIMEOUT:
        case BLE_HS_ETIMEOUT_HCI:
            return ESP_ERR_TIMEOUT;
        case BLE_HS_ENOMEM:
        case BLE_HS_ENOMEM_EVT:
            return ESP_ERR_NO_MEM;
        case BLE_HS_EINVAL:
        case BLE_HS_EMSGSIZE:
            return ESP_ERR_INVALID_ARG;
        case BLE_HS_ENOTCONN:
        case BLE_HS_ENOENT:
            return ESP_ERR_NOT_FOUND;
        case BLE_HS_EALREADY:
        case BLE_HS_EBUSY:
            return ESP_ERR_INVALID_STATE;
        default:
            return ESP_FAIL;
    }
}

bool advHasService(const ble_hs_adv_fields& fields, uint16_t serviceUuid) {
    for (uint8_t i = 0; i < fields.num_uuids16; ++i) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == serviceUuid) {
            return true;
        }
    }
    return false;
}

bool isConnectableEvent(uint8_t eventType) {
    return eventType == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || eventType == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
}

bool isScanResponseEvent(uint8_t eventType) {
    return eventType == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
}

void sortPairResultsBySignal(PairScanEntry* entries, size_t count) {
    if (entries == nullptr || count < 2) {
        return;
    }

    for (size_t i = 1; i < count; ++i) {
        PairScanEntry key = entries[i];
        size_t j = i;
        while (j > 0) {
            const bool keyPreferred = (key.hasService && !entries[j - 1].hasService) ||
                                      (key.hasService == entries[j - 1].hasService && key.rssi > entries[j - 1].rssi);
            if (!keyPreferred) {
                break;
            }
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = key;
    }
}

void addScanResult(const struct ble_gap_disc_desc& disc) {
    struct ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data) != 0) {
        return;
    }

    char address[18] = {};
    formatMacString(disc.addr, address, sizeof(address));
    if (!isValidMacString(address)) {
        return;
    }

    for (size_t i = 0; i < g_scanResultCount; ++i) {
        if (strcmp(g_scanResults[i].address, address) == 0) {
            if (disc.rssi > g_scanResults[i].rssi) {
                g_scanResults[i].rssi = disc.rssi;
            }
            if (fields.name != nullptr && fields.name_len > 0 && g_scanResults[i].name[0] == '\0') {
                const size_t copyLen =
                    fields.name_len < sizeof(g_scanResults[i].name) - 1 ? fields.name_len : sizeof(g_scanResults[i].name) - 1;
                memcpy(g_scanResults[i].name, fields.name, copyLen);
                g_scanResults[i].name[copyLen] = '\0';
            }
            g_scanResults[i].hasService = g_scanResults[i].hasService || advHasService(fields, kServiceUuid);
            return;
        }
    }

    if (!isConnectableEvent(disc.event_type)) {
        return;
    }

    if (g_scanResultCount >= kMaxPairScanResults) {
        return;
    }

    PairScanEntry entry;
    copyText(entry.address, sizeof(entry.address), address);
    if (fields.name != nullptr && fields.name_len > 0) {
        const size_t copyLen = fields.name_len < sizeof(entry.name) - 1 ? fields.name_len : sizeof(entry.name) - 1;
        memcpy(entry.name, fields.name, copyLen);
        entry.name[copyLen] = '\0';
    }
    // 没有 name 时不预填占位符,保持 entry.name[0]='\0',
    // 后续 SCAN_RSP 携带真名时 dedup 分支才能正常覆盖;UI 层会把空名兜底成"BLE设备"。
    entry.rssi = disc.rssi;
    entry.addressType = disc.addr.type;
    entry.connectable = isConnectableEvent(disc.event_type);
    entry.hasService = advHasService(fields, kServiceUuid);
    g_scanResults[g_scanResultCount++] = entry;
}

esp_err_t logBleMemory(const char* phase) {
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    LOG_I(TAG_BLE_IDF,
          "%s: internal=%u largest=%u psram=%u",
          phase != nullptr ? phase : "BLE memory",
          static_cast<unsigned>(internalFree),
          static_cast<unsigned>(internalLargest),
          static_cast<unsigned>(psramFree));

    if (internalFree < kMinBleInternalFree || internalLargest < kMinBleInternalLargest) {
        LOG_E(TAG_BLE_IDF,
              "Internal SRAM insufficient for BLE (free=%u/%u largest=%u/%u), abort",
              static_cast<unsigned>(internalFree), static_cast<unsigned>(kMinBleInternalFree),
              static_cast<unsigned>(internalLargest), static_cast<unsigned>(kMinBleInternalLargest));
        setLastError("internal SRAM low: free=%u largest=%u (need %u/%u)",
                     static_cast<unsigned>(internalFree), static_cast<unsigned>(internalLargest),
                     static_cast<unsigned>(kMinBleInternalFree), static_cast<unsigned>(kMinBleInternalLargest));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void onHostSync() {
    if (takeMutex(pdMS_TO_TICKS(50))) {
        g_stackReady = true;
        g_stackStarting = false;
        giveMutex();
    }
    xSemaphoreGive(g_stackSyncSemaphore);
}

void onHostReset(int reason) {
    LOG_W(TAG_BLE_IDF, "NimBLE host reset reason=%d", reason);
    if (takeMutex(pdMS_TO_TICKS(50))) {
        g_stackReady = false;
        g_stackStarting = false;
        giveMutex();
    }
}

void nimbleHostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t initStack() {
    if (!ensureMutexes()) {
        return ESP_ERR_NO_MEM;
    }

    if (takeMutex()) {
        if (g_stackReady) {
            giveMutex();
            return ESP_OK;
        }
        if (g_stackStarting) {
            giveMutex();
            if (xSemaphoreTake(g_stackSyncSemaphore, pdMS_TO_TICKS(3000)) == pdTRUE) {
                return g_stackReady ? ESP_OK : ESP_FAIL;
            }
            return ESP_ERR_TIMEOUT;
        }
        g_stackStarting = true;
        clearLastErrorLocked();
        giveMutex();
    }

    const esp_err_t memErr = logBleMemory("before NimBLE init");
    if (memErr != ESP_OK) {
        if (takeMutex()) {
            g_stackStarting = false;
            giveMutex();
        }
        return memErr;
    }
    const esp_err_t initErr = nimble_port_init();
    if (initErr != ESP_OK && initErr != ESP_ERR_INVALID_STATE) {
        setLastError("NimBLE init failed: %s", esp_err_to_name(initErr));
        if (takeMutex()) {
            g_stackStarting = false;
            giveMutex();
        }
        return initErr;
    }

    ble_hs_cfg.reset_cb = onHostReset;
    ble_hs_cfg.sync_cb = onHostSync;
#if CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("ESP32S3-IDF");
#endif

    if (!g_hostTaskStarted) {
        nimble_port_freertos_init(nimbleHostTask);
        g_hostTaskStarted = true;
    }

    if (xSemaphoreTake(g_stackSyncSemaphore, pdMS_TO_TICKS(3000)) != pdTRUE) {
        setLastError("NimBLE host sync timed out");
        if (takeMutex()) {
            g_stackStarting = false;
            giveMutex();
        }
        return ESP_ERR_TIMEOUT;
    }

    return g_stackReady ? ESP_OK : ESP_FAIL;
}

int scanGapEvent(struct ble_gap_event* event, void*) {
    if (event == nullptr) {
        return 0;
    }

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            addScanResult(event->disc);
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            xSemaphoreGive(g_scanDoneSemaphore);
            return 0;
        default:
            return 0;
    }
}

void finishPairContext(int status) {
    g_pairContext.status = status;
    g_pairContext.done = true;
    xSemaphoreGive(g_pairDoneSemaphore);
}

int cccdWriteCallback(uint16_t, const struct ble_gatt_error* error, struct ble_gatt_attr*, void*) {
    const int status = error != nullptr ? error->status : BLE_HS_EUNKNOWN;
    if (status == 0) {
        g_pairContext.notifyEnabled = true;
    } else {
        LOG_W(TAG_BLE_IDF, "FFE1 CCCD write failed rc=%d (continuing without ACK verification)", status);
    }
    // Pair is considered successful as long as FFE2 is writable; ACK is best-effort.
    finishPairContext(0);
    return 0;
}

int pairDscCallback(uint16_t connHandle,
                    const struct ble_gatt_error* error,
                    uint16_t chrValHandle,
                    const struct ble_gatt_dsc* dsc,
                    void*) {
    if (error == nullptr) {
        finishPairContext(0);
        return 0;
    }

    if (error->status == 0 && dsc != nullptr && chrValHandle == g_pairContext.rxHandle &&
        g_pairContext.rxCccdHandle == 0 && ble_uuid_cmp(&dsc->uuid.u, &kCccdUuidDef.u) == 0) {
        g_pairContext.rxCccdHandle = dsc->handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (g_pairContext.rxCccdHandle == 0) {
            LOG_W(TAG_BLE_IDF, "FFE1 CCCD not found; ACK verification will be disabled");
            finishPairContext(0);
            return 0;
        }
        const uint8_t enableNotify[2] = {0x01, 0x00};
        const int rc = ble_gattc_write_flat(connHandle, g_pairContext.rxCccdHandle,
                                            enableNotify, sizeof(enableNotify),
                                            cccdWriteCallback, nullptr);
        if (rc != 0) {
            LOG_W(TAG_BLE_IDF, "FFE1 CCCD write start failed rc=%d", rc);
            finishPairContext(0);
        }
        return 0;
    }

    LOG_W(TAG_BLE_IDF, "FFE1 descriptor discovery failed rc=%d", error->status);
    finishPairContext(0);
    return 0;
}

int pairCharCallback(uint16_t connHandle, const struct ble_gatt_error* error,
                     const struct ble_gatt_chr* chr, void*) {
    if (error == nullptr) {
        finishPairContext(BLE_HS_EUNKNOWN);
        return 0;
    }

    if (error->status == 0 && chr != nullptr) {
        if (ble_uuid_cmp(&chr->uuid.u, &kTxUuidDef.u) == 0) {
            g_pairContext.txHandle = chr->val_handle;
            g_pairContext.txProperties = chr->properties;
        } else if (ble_uuid_cmp(&chr->uuid.u, &kRxUuidDef.u) == 0) {
            g_pairContext.rxHandle = chr->val_handle;
            g_pairContext.rxProperties = chr->properties;
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (g_pairContext.txHandle == 0) {
            finishPairContext(BLE_HS_ENOENT);
            return 0;
        }
        const bool wantsNotify =
            g_pairContext.rxHandle != 0 && (g_pairContext.rxProperties & BLE_GATT_CHR_F_NOTIFY) != 0;
        if (!wantsNotify) {
            finishPairContext(0);
            return 0;
        }
        // Walk descriptors between FFE1's value handle and the service end so we
        // can find FFE1's CCCD (0x2902). Other chars in this service have no
        // CCCD per probe, so the filter inside the callback is enough.
        const int rc = ble_gattc_disc_all_dscs(connHandle, g_pairContext.rxHandle,
                                               g_pairContext.serviceEnd,
                                               pairDscCallback, nullptr);
        if (rc != 0) {
            LOG_W(TAG_BLE_IDF, "FFE1 descriptor discovery start failed rc=%d", rc);
            finishPairContext(0);
        }
        return 0;
    }

    finishPairContext(error->status);
    return 0;
}

int pairServiceCallback(uint16_t connHandle,
                        const struct ble_gatt_error* error,
                        const struct ble_gatt_svc* service,
                        void*) {
    if (error == nullptr) {
        finishPairContext(BLE_HS_EUNKNOWN);
        return 0;
    }

    if (error->status == 0 && service != nullptr) {
        g_pairContext.serviceStart = service->start_handle;
        g_pairContext.serviceEnd = service->end_handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (g_pairContext.serviceStart == 0 || g_pairContext.serviceEnd == 0) {
            finishPairContext(BLE_HS_ENOENT);
            return 0;
        }

        // Enumerate ALL chars in FFE0 instead of looking up FFE2 by UUID, so we
        // can also pick up FFE1 (the AC's NOTIFY echo channel) in the same pass.
        const int rc = ble_gattc_disc_all_chrs(connHandle,
                                               g_pairContext.serviceStart,
                                               g_pairContext.serviceEnd,
                                               pairCharCallback,
                                               nullptr);
        if (rc != 0) {
            finishPairContext(rc);
        }
        return 0;
    }

    finishPairContext(error->status);
    return 0;
}

int pairMtuCallback(uint16_t connHandle, const struct ble_gatt_error* /*error*/, uint16_t /*mtu*/, void*) {
    // 抢在对端发 MTU req 之前完成协商;部分外设(如目标空调)如果先发 MTU req
    // 而我们没响应(本工程只编 Central 角色,无 ATT 服务端),会在数百 ms 内主动断开。
    // 不管 MTU exchange 成功与否,都继续做 service discovery。
    const int rc = ble_gattc_disc_svc_by_uuid(connHandle, &kServiceUuidDef.u, pairServiceCallback, nullptr);
    if (rc != 0) {
        finishPairContext(rc);
    }
    return 0;
}

int pairGapEvent(struct ble_gap_event* event, void*) {
    if (event == nullptr) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_CONNECT || event->type == BLE_GAP_EVENT_LINK_ESTAB) {
        const int status = event->type == BLE_GAP_EVENT_CONNECT ? event->connect.status : event->link_estab.status;
        const uint16_t connHandle =
            event->type == BLE_GAP_EVENT_CONNECT ? event->connect.conn_handle : event->link_estab.conn_handle;

        if (status != 0) {
            finishPairContext(status);
            return 0;
        }

        // NimBLE 在某些配置下会同时投递 BLE_GAP_EVENT_CONNECT 和 BLE_GAP_EVENT_LINK_ESTAB,
        // 用 connHandle 守卫避免重复发起 MTU/discovery。
        if (g_pairContext.connHandle != kInvalidConnHandle) {
            return 0;
        }
        g_pairContext.connHandle = connHandle;
        const int rc = ble_gattc_exchange_mtu(connHandle, pairMtuCallback, nullptr);
        if (rc != 0) {
            finishPairContext(rc);
        }
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        if (takeMutex(pdMS_TO_TICKS(20))) {
            if (g_pairContext.connHandle == event->disconnect.conn.conn_handle) {
                g_pairContext.connHandle = kInvalidConnHandle;
                g_pairContext.txHandle = 0;
                g_pairContext.txProperties = 0;
                g_pairContext.rxHandle = 0;
                g_pairContext.rxCccdHandle = 0;
                g_pairContext.rxProperties = 0;
                g_pairContext.notifyEnabled = false;
            }
            giveMutex();
        }
        if (!g_pairContext.done) {
            finishPairContext(event->disconnect.reason);
        }
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
        // FFE1 echo from the AC. Capture the frame into g_lastEcho and signal
        // writeControlFrame() that an ACK arrived. We accept indications too
        // (indication:1) although the AC uses notifications.
        if (event->notify_rx.attr_handle == g_pairContext.rxHandle &&
            g_pairContext.rxHandle != 0 && event->notify_rx.om != nullptr) {
            const uint16_t pktLen = OS_MBUF_PKTLEN(event->notify_rx.om);
            const uint16_t copyLen = pktLen > kMaxFrameLen ? kMaxFrameLen : pktLen;
            uint16_t actualLen = 0;
            const int rc = ble_hs_mbuf_to_flat(event->notify_rx.om, g_lastEcho, copyLen, &actualLen);
            if (rc == 0 && actualLen > 0) {
                g_lastEchoLen = actualLen;
                xSemaphoreGive(g_echoSemaphore);
            }
        }
        return 0;
    }

    return 0;
}

bool loadStoredTarget() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kPrefsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (takeMutex()) {
            g_hasStoredTarget = false;
            copyText(g_targetAddress, sizeof(g_targetAddress), kDefaultTargetMac);
            g_targetAddressType = BLE_ADDR_PUBLIC;
            giveMutex();
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG_BLE_IDF, "Failed to open BLE target namespace: %s", esp_err_to_name(err));
        }
        return false;
    }

    char mac[18] = {};
    size_t macLen = sizeof(mac);
    uint8_t addressType = BLE_ADDR_PUBLIC;
    err = nvs_get_str(handle, kPrefsMacKey, mac, &macLen);
    const esp_err_t typeErr = nvs_get_u8(handle, kPrefsTypeKey, &addressType);
    nvs_close(handle);

    if (err != ESP_OK || typeErr != ESP_OK || !isValidMacString(mac)) {
        if (takeMutex()) {
            g_hasStoredTarget = false;
            copyText(g_targetAddress, sizeof(g_targetAddress), kDefaultTargetMac);
            g_targetAddressType = BLE_ADDR_PUBLIC;
            giveMutex();
        }
        LOG_I(TAG_BLE_IDF, "No stored BLE target, using default %s", kDefaultTargetMac);
        return false;
    }

    if (takeMutex()) {
        g_hasStoredTarget = true;
        copyText(g_targetAddress, sizeof(g_targetAddress), mac);
        g_targetAddressType = addressType;
        giveMutex();
    }
    LOG_I(TAG_BLE_IDF, "Loaded BLE target from NVS: %s type=%u", mac, static_cast<unsigned>(addressType));
    return true;
}

esp_err_t saveTargetToFlash(const char* addressText, uint8_t addressType) {
    if (!isValidMacString(addressText)) {
        return ESP_ERR_INVALID_ARG;
    }

    ScopedFlashGuard flashGuard("IDF BLE target save", 5000);
    if (!flashGuard.ok()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kPrefsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, kPrefsMacKey, addressText);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kPrefsTypeKey, addressType);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK && takeMutex()) {
        g_hasStoredTarget = true;
        copyText(g_targetAddress, sizeof(g_targetAddress), addressText);
        g_targetAddressType = addressType;
        giveMutex();
    }
    return err;
}

void setPairingState(PairingState state) {
    if (takeMutex()) {
        g_pairingState = state;
        giveMutex();
    }
}

bool buildFrame(uint8_t functionCode,
                const uint8_t* payload,
                size_t payloadLen,
                uint8_t* outFrame,
                size_t outCapacity,
                size_t* outLen) {
    if (outLen == nullptr) {
        return false;
    }
    *outLen = 0;

    const size_t frameLen = 2 + 1 + 1 + 1 + payloadLen + 1 + 2;
    if (outFrame == nullptr || frameLen > outCapacity || payloadLen > kMaxFrameLen) {
        return false;
    }

    outFrame[0] = 0x5B;
    outFrame[1] = 0x5B;
    outFrame[2] = static_cast<uint8_t>(payloadLen + 5);
    outFrame[3] = kProductCode;
    outFrame[4] = functionCode;

    for (size_t i = 0; i < payloadLen; ++i) {
        outFrame[5 + i] = payload[i];
    }

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5 + payloadLen; ++i) {
        checksum = static_cast<uint8_t>(checksum + outFrame[i]);
    }

    outFrame[5 + payloadLen] = checksum;
    outFrame[6 + payloadLen] = 0x0D;
    outFrame[7 + payloadLen] = 0x0B;
    *outLen = frameLen;
    return true;
}

bool hasUsableActiveConnection() {
    if (g_pairContext.connHandle == kInvalidConnHandle || g_pairContext.txHandle == 0) {
        return false;
    }

    struct ble_gap_conn_desc desc = {};
    const int rc = ble_gap_conn_find(g_pairContext.connHandle, &desc);
    if (rc == 0) {
        return true;
    }

    LOG_W(TAG_BLE_IDF, "BLE connection handle is stale rc=%d", rc);
    g_pairContext.connHandle = kInvalidConnHandle;
    g_pairContext.txHandle = 0;
    g_pairContext.txProperties = 0;
    return false;
}

void terminateActiveConnection() {
    if (g_pairContext.connHandle == kInvalidConnHandle) {
        g_pairContext.txHandle = 0;
        g_pairContext.txProperties = 0;
        return;
    }

    const uint16_t connHandle = g_pairContext.connHandle;
    const int rc = ble_gap_terminate(connHandle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        LOG_W(TAG_BLE_IDF, "BLE terminate failed rc=%d", rc);
    }
    g_pairContext.connHandle = kInvalidConnHandle;
    g_pairContext.txHandle = 0;
    g_pairContext.txProperties = 0;
    vTaskDelay(pdMS_TO_TICKS(kDisconnectDelayMs));
}

bool connectTargetForControl() {
    if (hasUsableActiveConnection()) {
        return true;
    }

    char targetAddress[18] = {};
    uint8_t targetAddressType = BLE_ADDR_PUBLIC;
    if (takeMutex(pdMS_TO_TICKS(100))) {
        copyText(targetAddress,
                 sizeof(targetAddress),
                 g_targetAddress[0] != '\0' ? g_targetAddress : kDefaultTargetMac);
        targetAddressType = g_targetAddress[0] != '\0' ? g_targetAddressType : BLE_ADDR_PUBLIC;
        giveMutex();
    } else {
        setLastError("BLE target mutex timed out");
        return false;
    }

    ble_addr_t peerAddress = {};
    if (!parseMacString(targetAddress, &peerAddress, targetAddressType)) {
        setLastError("Invalid BLE target address");
        return false;
    }

    terminateActiveConnection();
    g_pairContext = PairContext();

    uint8_t ownAddressType = BLE_OWN_ADDR_PUBLIC;
    int rc = ble_hs_id_infer_auto(0, &ownAddressType);
    if (rc != 0) {
        setLastError("BLE address type error rc=%d", rc);
        return false;
    }

    while (xSemaphoreTake(g_pairDoneSemaphore, 0) == pdTRUE) {
    }

    rc = ble_gap_connect(ownAddressType, &peerAddress, kPairConnectTimeoutMs, nullptr, pairGapEvent, nullptr);
    if (rc != 0) {
        setLastError("BLE connect start failed rc=%d", rc);
        return false;
    }

    if (xSemaphoreTake(g_pairDoneSemaphore, pdMS_TO_TICKS(kControlProcedureTimeoutMs)) != pdTRUE) {
        terminateActiveConnection();
        setLastError("BLE control connect timed out");
        return false;
    }

    if (g_pairContext.status != 0 || g_pairContext.txHandle == 0) {
        const int status = g_pairContext.status;
        terminateActiveConnection();
        setLastError("BLE FFE0/FFE2 validation failed rc=%d", status);
        return false;
    }

    LOG_I(TAG_BLE_IDF,
          "BLE control connection ready: %s type=%u handle=%u tx=%u props=0x%02x rx=%u notify=%d",
          targetAddress,
          static_cast<unsigned>(targetAddressType),
          static_cast<unsigned>(g_pairContext.connHandle),
          static_cast<unsigned>(g_pairContext.txHandle),
          static_cast<unsigned>(g_pairContext.txProperties),
          static_cast<unsigned>(g_pairContext.rxHandle),
          g_pairContext.notifyEnabled ? 1 : 0);
    return true;
}

int writeResponseCallback(uint16_t, const struct ble_gatt_error* error, struct ble_gatt_attr*, void*) {
    g_writeStatus = error != nullptr ? error->status : BLE_HS_EUNKNOWN;
    xSemaphoreGive(g_writeDoneSemaphore);
    return 0;
}

esp_err_t writeControlFrameOnce(const uint8_t* frame, size_t frameLen) {
    if (!connectTargetForControl()) {
        return ESP_FAIL;
    }

    if ((g_pairContext.txProperties & (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE)) == 0) {
        setLastError("BLE FFE2 characteristic is not writable");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const bool ackEnabled =
        g_ackVerificationEnabled && g_pairContext.notifyEnabled && g_pairContext.rxHandle != 0;
    if (ackEnabled) {
        // Drain any stale FFE1 echo before issuing the new write so the wait
        // below only consumes this command's response.
        while (xSemaphoreTake(g_echoSemaphore, 0) == pdTRUE) {
        }
        g_lastEchoLen = 0;
    }

    LOG_I(TAG_BLE_IDF, "[LAT] gatt_write len=%u no_rsp=%d ack=%d",
          static_cast<unsigned>(frameLen),
          (g_pairContext.txProperties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0 ? 1 : 0,
          ackEnabled ? 1 : 0);
    if ((g_pairContext.txProperties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0) {
        const int rc = ble_gattc_write_no_rsp_flat(
            g_pairContext.connHandle, g_pairContext.txHandle, frame, static_cast<uint16_t>(frameLen));
        if (rc != 0) {
            terminateActiveConnection();
            setLastError("BLE write-no-response failed rc=%d", rc);
            return bleRcToEspErr(rc);
        }
    } else {
        while (xSemaphoreTake(g_writeDoneSemaphore, 0) == pdTRUE) {
        }
        g_writeStatus = BLE_HS_EUNKNOWN;
        const int rc = ble_gattc_write_flat(g_pairContext.connHandle,
                                            g_pairContext.txHandle,
                                            frame,
                                            static_cast<uint16_t>(frameLen),
                                            writeResponseCallback,
                                            nullptr);
        if (rc != 0) {
            terminateActiveConnection();
            setLastError("BLE write failed rc=%d", rc);
            return bleRcToEspErr(rc);
        }
        if (xSemaphoreTake(g_writeDoneSemaphore, pdMS_TO_TICKS(kWriteResponseTimeoutMs)) != pdTRUE) {
            terminateActiveConnection();
            setLastError("BLE write response timed out");
            return ESP_ERR_TIMEOUT;
        }
        if (g_writeStatus != 0) {
            terminateActiveConnection();
            setLastError("BLE write response failed rc=%d", g_writeStatus);
            return bleRcToEspErr(g_writeStatus);
        }
    }

    if (!ackEnabled) {
        return ESP_OK;
    }

    // Wait for FFE1 echo. The AC re-emits the exact frame it received, so a
    // byte-perfect match is the strongest end-to-end ACK we have for this
    // small-brand controller (no application-layer status field).
    if (xSemaphoreTake(g_echoSemaphore, pdMS_TO_TICKS(kEchoTimeoutMs)) != pdTRUE) {
        terminateActiveConnection();
        setLastError("BLE ACK timeout: no FFE1 echo in %ums", static_cast<unsigned>(kEchoTimeoutMs));
        return ESP_ERR_TIMEOUT;
    }
    if (g_lastEchoLen != frameLen || memcmp(g_lastEcho, frame, frameLen) != 0) {
        char hexEcho[3 * kMaxFrameLen + 1] = {};
        const size_t hexCopyLen = g_lastEchoLen > kMaxFrameLen ? kMaxFrameLen : g_lastEchoLen;
        for (size_t i = 0; i < hexCopyLen; ++i) {
            snprintf(hexEcho + i * 3, sizeof(hexEcho) - i * 3, "%02x ", g_lastEcho[i]);
        }
        terminateActiveConnection();
        setLastError("BLE ACK mismatch len=%u echo=%s",
                     static_cast<unsigned>(g_lastEchoLen), hexEcho);
        return ESP_ERR_INVALID_RESPONSE;
    }
    LOG_I(TAG_BLE_IDF, "[LAT] gatt_ack ok len=%u", static_cast<unsigned>(g_lastEchoLen));
    return ESP_OK;
}

// Validate then attempt up to 2 writes. Transient failures (ACK timeout, ACK
// mismatch, connection drop, write rc != 0) trigger one retry: the prior
// attempt has already terminated the connection, so the next call reconnects
// from scratch and re-discovers FFE1 + re-enables NOTIFY. Configuration errors
// (invalid frame, FFE2 not writable) bail out immediately.
esp_err_t writeControlFrame(const uint8_t* frame, size_t frameLen) {
    if (frame == nullptr || frameLen == 0 || frameLen > UINT16_MAX) {
        setLastError("BLE command frame is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    constexpr int kMaxAttempts = 2;
    constexpr uint32_t kRetryBackoffMs = 150;
    esp_err_t lastErr = ESP_FAIL;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            LOG_W(TAG_BLE_IDF, "BLE write retry %d/%d after %s",
                  attempt, kMaxAttempts - 1, esp_err_to_name(lastErr));
            vTaskDelay(pdMS_TO_TICKS(kRetryBackoffMs));
        }
        lastErr = writeControlFrameOnce(frame, frameLen);
        if (lastErr == ESP_OK) {
            return ESP_OK;
        }
        if (lastErr == ESP_ERR_INVALID_ARG || lastErr == ESP_ERR_NOT_SUPPORTED) {
            return lastErr;  // non-transient: don't burn another reconnect on it
        }
    }
    return lastErr;
}

void finishControlCommand(esp_err_t result) {
    if (takeMutex(pdMS_TO_TICKS(100))) {
        g_controlContext.result = result;
        g_controlContext.done = true;
        giveMutex();
    } else {
        g_controlContext.result = result;
        g_controlContext.done = true;
    }
    xSemaphoreGive(g_controlDoneSemaphore);
}

void runControlCommand() {
    LOG_I(TAG_BLE_IDF, "[LAT] ble_worker_run");
    uint8_t frame[kMaxFrameLen] = {};
    size_t frameLen = 0;
    bool disconnectAfter = false;
    if (takeMutex(pdMS_TO_TICKS(100))) {
        frameLen = g_controlContext.frameLen;
        if (frameLen <= sizeof(frame)) {
            memcpy(frame, g_controlContext.frame, frameLen);
        }
        disconnectAfter = g_disconnectAfterCommand;
        clearLastErrorLocked();
        giveMutex();
    }

    if (frameLen == 0 || frameLen > sizeof(frame)) {
        setLastError("BLE command frame is empty");
        finishControlCommand(ESP_ERR_INVALID_ARG);
        return;
    }

    // 二层防线：上层 dispatch 应已拒非 BLE 模式；这里再兜底，避免任何路径误触发 NimBLE init。
    if (!AppIdfAppMode::isBle()) {
        setLastError("BLE control rejected: device not in BLE mode (%s)",
                     AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
        finishControlCommand(ESP_ERR_INVALID_STATE);
        return;
    }

    const esp_err_t controlMemErr = logBleMemory("before BLE control");
    if (controlMemErr != ESP_OK) {
        finishControlCommand(controlMemErr);
        return;
    }
    const esp_err_t stackErr = initStack();
    if (stackErr != ESP_OK) {
        finishControlCommand(stackErr);
        return;
    }

    const esp_err_t writeErr = writeControlFrame(frame, frameLen);
    if (writeErr == ESP_OK) {
        LOG_I(TAG_BLE_IDF, "[LAT] gatt_done");
    } else {
        LOG_W(TAG_BLE_IDF, "[LAT] gatt_done err=%s", esp_err_to_name(writeErr));
    }

    if (disconnectAfter) {
        terminateActiveConnection();
        LOG_I(TAG_BLE_IDF, "BLE command connection released after write");
    }

    finishControlCommand(writeErr);
}

esp_err_t ensureWorkerTask() {
    if (g_workerTaskMemory.handle != nullptr) {
        return ESP_OK;
    }
    return AppIdfTasks::createPinnedToCoreInternal(
        [](void*) {
            while (true) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

                WorkerCommand command = WorkerCommand::None;
                if (takeMutex()) {
                    command = g_pendingCommand;
                    g_pendingCommand = WorkerCommand::None;
                    giveMutex();
                }

                if (command == WorkerCommand::Scan) {
                    if (logBleMemory("before BLE scan") != ESP_OK) {
                        setPairingState(PairingState::Error);
                        continue;
                    }
                    if (initStack() != ESP_OK) {
                        setPairingState(PairingState::Error);
                        continue;
                    }
                    if (g_cancelRequested) {
                        setPairingState(PairingState::Idle);
                        g_cancelRequested = false;
                        continue;
                    }

                    if (takeMutex()) {
                        g_scanResultCount = 0;
                        memset(g_scanResults, 0, sizeof(g_scanResults));
                        clearLastErrorLocked();
                        giveMutex();
                    }

                    uint8_t ownAddressType = BLE_OWN_ADDR_PUBLIC;
                    int rc = ble_hs_id_infer_auto(0, &ownAddressType);
                    if (rc != 0) {
                        setLastError("BLE address type error rc=%d", rc);
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    struct ble_gap_disc_params discParams = {};
                    discParams.filter_duplicates = 1;
                    discParams.passive = 0;
                    discParams.itvl = 80;
                    discParams.window = 60;
                    while (xSemaphoreTake(g_scanDoneSemaphore, 0) == pdTRUE) {
                    }
                    rc = ble_gap_disc(ownAddressType, kScanDurationMs, &discParams, scanGapEvent, nullptr);
                    if (rc != 0) {
                        setLastError("BLE scan start failed rc=%d", rc);
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (xSemaphoreTake(g_scanDoneSemaphore, pdMS_TO_TICKS(kScanDurationMs + 1500)) != pdTRUE) {
                        ble_gap_disc_cancel();
                        setLastError("BLE scan timed out");
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (takeMutex()) {
                        sortPairResultsBySignal(g_scanResults, g_scanResultCount);
                        memset(g_pairingResults, 0, sizeof(g_pairingResults));
                        for (size_t i = 0; i < g_scanResultCount; ++i) {
                            g_pairingResults[i] = g_scanResults[i];
                        }
                        g_pairingResultCount = g_scanResultCount;
                        g_pairingState = g_cancelRequested ? PairingState::Idle : PairingState::Ready;
                        g_cancelRequested = false;
                        giveMutex();
                    }
                    LOG_I(TAG_BLE_IDF, "BLE scan completed, devices=%u", static_cast<unsigned>(g_scanResultCount));
                } else if (command == WorkerCommand::Pair) {
                    PairScanEntry selected;
                    bool selectedOk = false;
                    if (takeMutex()) {
                        if (g_selectedPairIndex < g_pairingResultCount) {
                            selected = g_pairingResults[g_selectedPairIndex];
                            selectedOk = isValidMacString(selected.address);
                        }
                        clearLastErrorLocked();
                        giveMutex();
                    }

                    if (!selectedOk) {
                        setLastError("No BLE device selected");
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (logBleMemory("before BLE pair") != ESP_OK) {
                        setPairingState(PairingState::Error);
                        continue;
                    }
                    if (initStack() != ESP_OK) {
                        setPairingState(PairingState::Error);
                        continue;
                    }
                    if (g_cancelRequested) {
                        setPairingState(PairingState::Idle);
                        g_cancelRequested = false;
                        continue;
                    }

                    ble_addr_t peerAddress = {};
                    if (!parseMacString(selected.address, &peerAddress, selected.addressType)) {
                        setLastError("Invalid BLE peer address");
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (g_pairContext.connHandle != kInvalidConnHandle) {
                        ble_gap_terminate(g_pairContext.connHandle, BLE_ERR_REM_USER_CONN_TERM);
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    g_pairContext = PairContext();

                    uint8_t ownAddressType = BLE_OWN_ADDR_PUBLIC;
                    int rc = ble_hs_id_infer_auto(0, &ownAddressType);
                    if (rc != 0) {
                        setLastError("BLE address type error rc=%d", rc);
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    while (xSemaphoreTake(g_pairDoneSemaphore, 0) == pdTRUE) {
                    }
                    rc = ble_gap_connect(ownAddressType, &peerAddress, kPairConnectTimeoutMs, nullptr, pairGapEvent, nullptr);
                    if (rc != 0) {
                        setLastError("BLE connect start failed rc=%d", rc);
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (xSemaphoreTake(g_pairDoneSemaphore, pdMS_TO_TICKS(kPairProcedureTimeoutMs)) != pdTRUE) {
                        if (g_pairContext.connHandle != kInvalidConnHandle) {
                            ble_gap_terminate(g_pairContext.connHandle, BLE_ERR_REM_USER_CONN_TERM);
                        }
                        setLastError("BLE pair timed out");
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    if (g_pairContext.status != 0 || g_pairContext.txHandle == 0) {
                        if (g_pairContext.connHandle != kInvalidConnHandle) {
                            ble_gap_terminate(g_pairContext.connHandle, BLE_ERR_REM_USER_CONN_TERM);
                        }
                        setLastError("BLE FFE0/FFE2 validation failed rc=%d", g_pairContext.status);
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    const esp_err_t saveErr = saveTargetToFlash(selected.address, selected.addressType);
                    if (saveErr != ESP_OK) {
                        setLastError("BLE target save failed: %s", esp_err_to_name(saveErr));
                        setPairingState(PairingState::Error);
                        continue;
                    }

                    setPairingState(PairingState::Success);
                    LOG_I(TAG_BLE_IDF,
                          "BLE pairing succeeded: %s type=%u handle=%u tx=%u rx=%u notify=%d",
                          selected.address,
                          static_cast<unsigned>(selected.addressType),
                          static_cast<unsigned>(g_pairContext.connHandle),
                          static_cast<unsigned>(g_pairContext.txHandle),
                          static_cast<unsigned>(g_pairContext.rxHandle),
                          g_pairContext.notifyEnabled ? 1 : 0);
                } else if (command == WorkerCommand::Control) {
                    runControlCommand();
                }
            }
        },
        "IDF_BLE",
        kWorkerTaskStackWords,
        nullptr,
        2,
        0,
        &g_workerTaskMemory);
}

}  // namespace

esp_err_t start() {
    if (g_started) {
        return ESP_OK;
    }
    if (!ensureMutexes()) {
        return ESP_ERR_NO_MEM;
    }
    loadStoredTarget();
    g_started = true;
    LOG_I(TAG_BLE_IDF, "IDF BLE aircon bridge started");
    return ESP_OK;
}

bool isStarted() {
    return g_started;
}

bool isStackReady() {
    return g_stackReady;
}

const char* getLastError() {
    return g_lastError[0] != '\0' ? g_lastError : "none";
}

bool hasStoredTarget() {
    return g_hasStoredTarget;
}

bool hasActiveConnection() {
    return g_pairContext.connHandle != kInvalidConnHandle && g_pairContext.txHandle != 0;
}

const char* getTargetAddressString() {
    return g_targetAddress[0] != '\0' ? g_targetAddress : kDefaultTargetMac;
}

uint8_t getTargetAddressType() {
    return g_targetAddress[0] != '\0' ? g_targetAddressType : BLE_ADDR_PUBLIC;
}

bool isDisconnectAfterCommandEnabled() {
    return g_disconnectAfterCommand;
}

void setDisconnectAfterCommand(bool enabled) {
    if (takeMutex(pdMS_TO_TICKS(100))) {
        g_disconnectAfterCommand = enabled;
        giveMutex();
    } else {
        g_disconnectAfterCommand = enabled;
    }
    LOG_I(TAG_BLE_IDF, "BLE disconnect-after-command %s", enabled ? "enabled" : "disabled");
    if (enabled) {
        releaseConnection();
    }
}

bool isAckVerificationEnabled() {
    return g_ackVerificationEnabled;
}

void setAckVerificationEnabled(bool enabled) {
    if (takeMutex(pdMS_TO_TICKS(100))) {
        g_ackVerificationEnabled = enabled;
        giveMutex();
    } else {
        g_ackVerificationEnabled = enabled;
    }
    LOG_I(TAG_BLE_IDF, "BLE FFE1 ACK verification %s", enabled ? "enabled" : "disabled");
}

bool isNotifySubscribed() {
    return g_pairContext.notifyEnabled;
}

esp_err_t sendFrame(uint8_t functionCode, const uint8_t* payload, size_t payloadLen) {
    if (!g_started) {
        const esp_err_t startErr = start();
        if (startErr != ESP_OK) {
            return startErr;
        }
    }

    const esp_err_t workerErr = ensureWorkerTask();
    if (workerErr != ESP_OK) {
        return workerErr;
    }

    uint8_t frame[kMaxFrameLen] = {};
    size_t frameLen = 0;
    if (!buildFrame(functionCode, payload, payloadLen, frame, sizeof(frame), &frameLen)) {
        setLastError("Failed to build BLE command frame");
        return ESP_ERR_INVALID_ARG;
    }

    while (xSemaphoreTake(g_controlDoneSemaphore, 0) == pdTRUE) {
    }

    if (!takeMutex(pdMS_TO_TICKS(250))) {
        setLastError("BLE command mutex timed out");
        return ESP_ERR_TIMEOUT;
    }
    if (g_pairingState == PairingState::Scanning || g_pairingState == PairingState::Pairing ||
        g_pendingCommand != WorkerCommand::None || !g_controlContext.done) {
        setLastErrorLocked("BLE is busy");
        giveMutex();
        return ESP_ERR_INVALID_STATE;
    }

    memset(&g_controlContext, 0, sizeof(g_controlContext));
    memcpy(g_controlContext.frame, frame, frameLen);
    g_controlContext.frameLen = frameLen;
    g_controlContext.result = ESP_FAIL;
    g_controlContext.done = false;
    g_pendingCommand = WorkerCommand::Control;
    clearLastErrorLocked();
    giveMutex();

    LOG_I(TAG_BLE_IDF, "[LAT] ble_send_call func=0x%02X frame_len=%u",
          static_cast<unsigned>(functionCode),
          static_cast<unsigned>(frameLen));
    xTaskNotifyGive(g_workerTaskMemory.handle);

    if (xSemaphoreTake(g_controlDoneSemaphore, pdMS_TO_TICKS(kControlApiTimeoutMs)) != pdTRUE) {
        setLastError("BLE command timed out");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_FAIL;
    if (takeMutex(pdMS_TO_TICKS(100))) {
        result = g_controlContext.result;
        giveMutex();
    }
    return result;
}

esp_err_t powerOn() {
    const uint8_t payload = 0x02;
    return sendFrame(0x01, &payload, 1);
}

esp_err_t powerOff() {
    const uint8_t payload = 0x01;
    return sendFrame(0x01, &payload, 1);
}

esp_err_t setCoolingMode() {
    const uint8_t payload = 0x01;
    return sendFrame(0x02, &payload, 1);
}

esp_err_t setVentMode() {
    const uint8_t payload = 0x03;
    return sendFrame(0x02, &payload, 1);
}

esp_err_t setEcoMode() {
    const uint8_t payload = 0x04;
    return sendFrame(0x02, &payload, 1);
}

esp_err_t setSleepMode() {
    const uint8_t payload = 0x05;
    return sendFrame(0x02, &payload, 1);
}

esp_err_t setTemperature(uint8_t tempC) {
    if (tempC < 18 || tempC > 31) {
        setLastError("Temperature is only supported in the 18~31 range");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t payload = tempC;
    return sendFrame(0x03, &payload, 1);
}

esp_err_t setFanSpeed(uint8_t level) {
    if (level < 1 || level > 5) {
        setLastError("Fan speed is only supported in the 1~5 range");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t payload = level;
    return sendFrame(0x04, &payload, 1);
}

esp_err_t setDisplayOn(bool on) {
    const uint8_t payload = on ? 0x02 : 0x01;
    return sendFrame(0x0A, &payload, 1);
}

esp_err_t setLightOn(bool on) {
    const uint8_t payload = on ? 0x01 : 0x02;
    return sendFrame(0x1C, &payload, 1);
}

esp_err_t setSwingHorizontal() {
    const uint8_t payload = 0x01;
    return sendFrame(0x10, &payload, 1);
}

esp_err_t setSwingVertical() {
    const uint8_t payload = 0x02;
    return sendFrame(0x10, &payload, 1);
}

esp_err_t startPairingScan() {
    AppIdfPowerSave::notifyActivity();
    if (!g_started) {
        const esp_err_t err = start();
        if (err != ESP_OK) {
            return err;
        }
    }

    const esp_err_t workerErr = ensureWorkerTask();
    if (workerErr != ESP_OK) {
        return workerErr;
    }

    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_pairingState == PairingState::Scanning || g_pairingState == PairingState::Pairing) {
        giveMutex();
        return ESP_ERR_INVALID_STATE;
    }

    g_cancelRequested = false;
    g_pairingState = PairingState::Scanning;
    g_pairingResultCount = 0;
    memset(g_pairingResults, 0, sizeof(g_pairingResults));
    g_pendingCommand = WorkerCommand::Scan;
    clearLastErrorLocked();
    giveMutex();

    xTaskNotifyGive(g_workerTaskMemory.handle);
    return ESP_OK;
}

esp_err_t startPairing(size_t index) {
    AppIdfPowerSave::notifyActivity();
    if (!g_started) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t workerErr = ensureWorkerTask();
    if (workerErr != ESP_OK) {
        return workerErr;
    }

    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_pairingState == PairingState::Scanning || g_pairingState == PairingState::Pairing) {
        giveMutex();
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= g_pairingResultCount || !isValidMacString(g_pairingResults[index].address)) {
        g_pairingState = PairingState::Error;
        setLastErrorLocked("No BLE device selected");
        giveMutex();
        return ESP_ERR_INVALID_ARG;
    }

    g_selectedPairIndex = index;
    g_cancelRequested = false;
    g_pairingState = PairingState::Pairing;
    g_pendingCommand = WorkerCommand::Pair;
    clearLastErrorLocked();
    giveMutex();

    xTaskNotifyGive(g_workerTaskMemory.handle);
    return ESP_OK;
}

void cancelPairing() {
    if (!g_started) {
        return;
    }
    g_cancelRequested = true;
    if (g_stackReady && g_pairingState == PairingState::Scanning && ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    if (g_stackReady && g_pairingState == PairingState::Pairing && g_pairContext.connHandle != kInvalidConnHandle) {
        ble_gap_terminate(g_pairContext.connHandle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (g_stackReady && g_pairingState == PairingState::Pairing) {
        ble_gap_conn_cancel();
    }
    if (takeMutex()) {
        if (g_pairingState != PairingState::Pairing) {
            g_pairingState = PairingState::Idle;
        }
        giveMutex();
    }
}

esp_err_t releaseConnection() {
    if (g_pairingState == PairingState::Scanning || g_pairingState == PairingState::Pairing ||
        g_pendingCommand != WorkerCommand::None || !g_controlContext.done) {
        setLastError("BLE is busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (g_pairContext.connHandle == kInvalidConnHandle) {
        g_pairContext.txHandle = 0;
        g_pairContext.txProperties = 0;
        return ESP_OK;
    }
    terminateActiveConnection();
    return ESP_OK;
}

PairingState getPairingState() {
    return g_pairingState;
}

const char* pairingStateName(PairingState state) {
    switch (state) {
        case PairingState::Idle:
            return "idle";
        case PairingState::Scanning:
            return "scanning";
        case PairingState::Ready:
            return "ready";
        case PairingState::Pairing:
            return "pairing";
        case PairingState::Success:
            return "success";
        case PairingState::Error:
            return "error";
        default:
            return "unknown";
    }
}

size_t getPairingResultCount() {
    return g_pairingResultCount;
}

bool getPairingResult(size_t index, PairScanEntry* out) {
    if (out == nullptr || index >= g_pairingResultCount) {
        return false;
    }
    *out = g_pairingResults[index];
    return true;
}

uint32_t workerTaskStackHighWatermark() {
    if (g_workerTaskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_workerTaskMemory.handle);
}

}  // namespace AppIdfBleAircon
