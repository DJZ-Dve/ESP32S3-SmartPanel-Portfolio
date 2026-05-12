#include "App_IdfScene.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "App_FlashGuard.h"
#include "App_IdfAppMode.h"
#include "App_IdfBlePresetScenes.h"
#include "App_IdfIr.h"
#include "App_IdfRf433.h"
#include "App_Log.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace AppIdfScene {
namespace {

constexpr const char* TAG_SCENE = "IDF_SCENE";
// 用户运行期数据走独立的 userdata 分区，避开 spiffs 资源分区被 idf.py flash 覆盖的副作用。
constexpr const char* kScenesPath = "/userdata/scenes.json";

SemaphoreHandle_t g_mutex = nullptr;
bool g_started = false;
// 持久场景表放 PSRAM：节省 ~2KB internal SRAM 给 NimBLE 的 14KB largest-block 守门用。
SceneItem* g_scenes = nullptr;
size_t g_count = 0;
uint8_t g_nextId = 1;

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

bool isBlePresetId(uint8_t id) {
    return id >= 0xE0 && id <= 0xEF;
}

char* readJsonFile() {
    struct stat st = {};
    if (stat(kScenesPath, &st) != 0 || st.st_size <= 0) {
        return nullptr;
    }
    const size_t size = static_cast<size_t>(st.st_size);
    char* buf = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        buf = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buf == nullptr) {
        LOG_E(TAG_SCENE, "scenes.json buffer alloc failed (%u bytes)", static_cast<unsigned>(size));
        return nullptr;
    }
    FILE* fp = fopen(kScenesPath, "rb");
    if (fp == nullptr) {
        heap_caps_free(buf);
        return nullptr;
    }
    const size_t read = fread(buf, 1, size, fp);
    fclose(fp);
    buf[read] = '\0';
    return buf;
}

bool writeJsonFile(const char* json) {
    ScopedFlashGuard flashGuard("scenes save", 5000);
    if (!flashGuard.ok()) {
        LOG_W(TAG_SCENE, "save denied: flash guard busy");
        return false;
    }
    FILE* fp = fopen(kScenesPath, "wb");
    if (fp == nullptr) {
        LOG_E(TAG_SCENE, "open scenes.json for write failed");
        return false;
    }
    const size_t len = strlen(json);
    const size_t written = fwrite(json, 1, len, fp);
    fclose(fp);
    if (written != len) {
        LOG_E(TAG_SCENE, "scenes.json short write: %u/%u", static_cast<unsigned>(written),
              static_cast<unsigned>(len));
        return false;
    }
    return true;
}

bool saveLocked() {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_CreateArray();
    if (root == nullptr || arr == nullptr) {
        if (root != nullptr) cJSON_Delete(root);
        if (arr != nullptr) cJSON_Delete(arr);
        return false;
    }
    cJSON_AddItemToObject(root, "scenes", arr);

    for (size_t i = 0; i < g_count; i++) {
        const SceneItem& s = g_scenes[i];
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", s.id);
        cJSON_AddNumberToObject(obj, "type", static_cast<int>(s.type));
        // 双写 label + desc：旧固件读 desc，新固件读 label，互不打架。
        cJSON_AddStringToObject(obj, "label", s.label);
        cJSON_AddStringToObject(obj, "desc", s.label);
        if (s.type == SignalType::IR) {
            cJSON_AddStringToObject(obj, "ir_name", s.irName);
        } else if (s.type == SignalType::RF433) {
            const uint32_t codeHigh = static_cast<uint32_t>(s.code433 >> 32);
            const uint32_t codeLow = static_cast<uint32_t>(s.code433);
            cJSON_AddNumberToObject(obj, "code_high", codeHigh);
            cJSON_AddNumberToObject(obj, "code_low", codeLow);
            cJSON_AddNumberToObject(obj, "len", s.len433);
            cJSON_AddNumberToObject(obj, "T", s.T433);
        }
        cJSON_AddItemToArray(arr, obj);
    }

    char* serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == nullptr) {
        LOG_E(TAG_SCENE, "serialize failed");
        return false;
    }
    const bool ok = writeJsonFile(serialized);
    cJSON_free(serialized);
    return ok;
}

void loadFromFileLocked() {
    g_count = 0;
    g_nextId = 1;
    char* json = readJsonFile();
    if (json == nullptr) {
        LOG_D(TAG_SCENE, "scenes.json absent or empty");
        return;
    }
    cJSON* root = cJSON_Parse(json);
    heap_caps_free(json);
    if (root == nullptr) {
        LOG_W(TAG_SCENE, "scenes.json parse failed");
        return;
    }
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "scenes");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return;
    }
    const int total = cJSON_GetArraySize(arr);
    for (int i = 0; i < total && g_count < kMaxScenes; i++) {
        const cJSON* obj = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(obj)) continue;

        SceneItem& s = g_scenes[g_count];
        memset(&s, 0, sizeof(s));

        const cJSON* idItem = cJSON_GetObjectItemCaseSensitive(obj, "id");
        const cJSON* typeItem = cJSON_GetObjectItemCaseSensitive(obj, "type");
        // label 优先；缺失 fallback 旧字段 desc。
        const cJSON* labelItem = cJSON_GetObjectItemCaseSensitive(obj, "label");
        if (!cJSON_IsString(labelItem) || labelItem->valuestring == nullptr) {
            labelItem = cJSON_GetObjectItemCaseSensitive(obj, "desc");
        }

        s.id = cJSON_IsNumber(idItem) ? static_cast<uint8_t>(idItem->valueint) : 0;
        // BLE=2 不应在 scenes.json 出现；当作 IR 兜底，避免污染数组。
        const int typeInt = cJSON_IsNumber(typeItem) ? typeItem->valueint : 0;
        if (typeInt == 1) {
            s.type = SignalType::RF433;
        } else {
            s.type = SignalType::IR;
        }
        if (cJSON_IsString(labelItem) && labelItem->valuestring != nullptr) {
            strncpy(s.label, labelItem->valuestring, sizeof(s.label) - 1);
        }

        if (s.type == SignalType::IR) {
            const cJSON* nameItem = cJSON_GetObjectItemCaseSensitive(obj, "ir_name");
            if (cJSON_IsString(nameItem) && nameItem->valuestring != nullptr) {
                strncpy(s.irName, nameItem->valuestring, sizeof(s.irName) - 1);
            }
        } else {
            const cJSON* codeHi = cJSON_GetObjectItemCaseSensitive(obj, "code_high");
            const cJSON* codeLo = cJSON_GetObjectItemCaseSensitive(obj, "code_low");
            const cJSON* codeLegacy = cJSON_GetObjectItemCaseSensitive(obj, "code");
            const cJSON* lenItem = cJSON_GetObjectItemCaseSensitive(obj, "len");
            const cJSON* tItem = cJSON_GetObjectItemCaseSensitive(obj, "T");
            uint64_t code = 0;
            if (cJSON_IsNumber(codeHi) || cJSON_IsNumber(codeLo)) {
                const uint32_t hi = cJSON_IsNumber(codeHi) ? static_cast<uint32_t>(codeHi->valuedouble) : 0;
                const uint32_t lo = cJSON_IsNumber(codeLo) ? static_cast<uint32_t>(codeLo->valuedouble) : 0;
                code = (static_cast<uint64_t>(hi) << 32) | lo;
            } else if (cJSON_IsNumber(codeLegacy)) {
                code = static_cast<uint64_t>(codeLegacy->valuedouble);
            }
            s.code433 = code;
            s.len433 = cJSON_IsNumber(lenItem) ? static_cast<uint8_t>(lenItem->valueint) : 0;
            s.T433 = cJSON_IsNumber(tItem) ? static_cast<uint16_t>(tItem->valueint) : 0;
        }

        // BLE 预制 id 段不允许进持久数组（理论上磁盘里也不会有），防御性丢弃。
        if (isBlePresetId(s.id)) {
            LOG_W(TAG_SCENE, "skip persisted entry with reserved id %u", s.id);
            memset(&s, 0, sizeof(s));
            continue;
        }

        if (s.id >= g_nextId && s.id < 0xE0) {
            g_nextId = static_cast<uint8_t>(s.id + 1);
        }
        g_count++;
    }
    cJSON_Delete(root);
    LOG_I(TAG_SCENE, "scenes loaded: %u entries", static_cast<unsigned>(g_count));
}

}  // namespace

esp_err_t start() {
    if (g_started) return ESP_OK;
    if (g_scenes == nullptr) {
        g_scenes = static_cast<SceneItem*>(
            heap_caps_calloc(kMaxScenes, sizeof(SceneItem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_scenes == nullptr) {
            // PSRAM 不可用（理论上不会，开发期保险）→ 回退到内部 SRAM 但日志告警。
            g_scenes = static_cast<SceneItem*>(
                heap_caps_calloc(kMaxScenes, sizeof(SceneItem), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (g_scenes == nullptr) {
                return ESP_ERR_NO_MEM;
            }
            LOG_W(TAG_SCENE, "scenes table fell back to internal SRAM");
        }
    }
    if (g_mutex == nullptr) {
        g_mutex = xSemaphoreCreateMutex();
        if (g_mutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    {
        MutexLock lock;
        loadFromFileLocked();
    }
    g_started = true;
    return ESP_OK;
}

bool isStarted() { return g_started; }

size_t count() {
    MutexLock lock;
    return lock.locked() ? g_count : 0;
}

esp_err_t addSceneIr(const char* label, const char* irName) {
    if (label == nullptr || label[0] == '\0' || irName == nullptr || irName[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    MutexLock lock;
    if (!lock.locked()) return ESP_ERR_TIMEOUT;
    if (g_count >= kMaxScenes) {
        LOG_W(TAG_SCENE, "scenes full (%u)", static_cast<unsigned>(kMaxScenes));
        return ESP_ERR_NO_MEM;
    }

    const size_t oldCount = g_count;
    const uint8_t oldNextId = g_nextId;
    SceneItem& s = g_scenes[g_count];
    memset(&s, 0, sizeof(s));
    // 跳过 BLE 预制保留段：g_nextId 永远 < 0xE0。
    if (g_nextId >= 0xE0) g_nextId = 1;
    s.id = g_nextId++;
    s.type = SignalType::IR;
    strncpy(s.label, label, sizeof(s.label) - 1);
    strncpy(s.irName, irName, sizeof(s.irName) - 1);
    g_count++;

    if (!saveLocked()) {
        memset(&s, 0, sizeof(s));
        g_count = oldCount;
        g_nextId = oldNextId;
        LOG_E(TAG_SCENE, "add IR scene save failed");
        return ESP_FAIL;
    }
    LOG_I(TAG_SCENE, "scene[%u] IR added: %s -> %s", s.id, label, irName);
    return ESP_OK;
}

esp_err_t addSceneRf433(const char* label, uint64_t code, uint8_t len, uint16_t T) {
    if (label == nullptr || label[0] == '\0' || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    MutexLock lock;
    if (!lock.locked()) return ESP_ERR_TIMEOUT;
    if (g_count >= kMaxScenes) {
        LOG_W(TAG_SCENE, "scenes full (%u)", static_cast<unsigned>(kMaxScenes));
        return ESP_ERR_NO_MEM;
    }

    const size_t oldCount = g_count;
    const uint8_t oldNextId = g_nextId;
    SceneItem& s = g_scenes[g_count];
    memset(&s, 0, sizeof(s));
    if (g_nextId >= 0xE0) g_nextId = 1;
    s.id = g_nextId++;
    s.type = SignalType::RF433;
    strncpy(s.label, label, sizeof(s.label) - 1);
    s.code433 = code;
    s.len433 = len;
    s.T433 = T;
    g_count++;

    if (!saveLocked()) {
        memset(&s, 0, sizeof(s));
        g_count = oldCount;
        g_nextId = oldNextId;
        LOG_E(TAG_SCENE, "add RF433 scene save failed");
        return ESP_FAIL;
    }
    const uint32_t codeHigh = static_cast<uint32_t>(code >> 32);
    const uint32_t codeLow = static_cast<uint32_t>(code);
    LOG_I(TAG_SCENE, "scene[%u] RF433 added: %s code=0x%08lX%08lX len=%u T=%u", s.id, label,
          static_cast<unsigned long>(codeHigh), static_cast<unsigned long>(codeLow), len, T);
    return ESP_OK;
}

esp_err_t removeById(uint8_t id) {
    if (isBlePresetId(id)) {
        // BLE 预制是 ROM，禁止删除。
        return ESP_ERR_INVALID_ARG;
    }
    MutexLock lock;
    if (!lock.locked()) return ESP_ERR_TIMEOUT;

    for (size_t i = 0; i < g_count; i++) {
        if (g_scenes[i].id != id) continue;

        SceneItem backup = g_scenes[i];
        for (size_t j = i; j + 1 < g_count; j++) {
            g_scenes[j] = g_scenes[j + 1];
        }
        g_count--;
        memset(&g_scenes[g_count], 0, sizeof(SceneItem));

        if (!saveLocked()) {
            for (size_t j = g_count; j > i; j--) {
                g_scenes[j] = g_scenes[j - 1];
            }
            g_scenes[i] = backup;
            g_count++;
            LOG_E(TAG_SCENE, "remove scene[%u] save failed", id);
            return ESP_FAIL;
        }
        LOG_I(TAG_SCENE, "scene[%u] removed", id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t clearAll() {
    MutexLock lock;
    if (!lock.locked()) return ESP_ERR_TIMEOUT;

    struct stat st = {};
    if (stat(kScenesPath, &st) == 0) {
        ScopedFlashGuard flashGuard("scenes clear", 5000);
        if (!flashGuard.ok()) {
            return ESP_ERR_TIMEOUT;
        }
        if (unlink(kScenesPath) != 0) {
            LOG_E(TAG_SCENE, "unlink scenes.json failed");
            return ESP_FAIL;
        }
    }
    g_count = 0;
    g_nextId = 1;
    memset(g_scenes, 0, sizeof(SceneItem) * kMaxScenes);
    LOG_I(TAG_SCENE, "all scenes cleared");
    return ESP_OK;
}

bool getById(uint8_t id, SceneItem* out) {
    if (out == nullptr) return false;
    if (isBlePresetId(id)) {
        return AppIdfBlePresetScenes::getById(id, *out);
    }
    MutexLock lock;
    if (!lock.locked()) return false;
    for (size_t i = 0; i < g_count; i++) {
        if (g_scenes[i].id == id) {
            *out = g_scenes[i];
            return true;
        }
    }
    return false;
}

bool getByIndex(size_t index, SceneItem* out) {
    if (out == nullptr) return false;
    MutexLock lock;
    if (!lock.locked()) return false;
    if (index >= g_count) return false;
    *out = g_scenes[index];
    return true;
}

esp_err_t executeById(uint8_t id) {
    SceneItem item = {};
    if (!getById(id, &item)) {
        LOG_W(TAG_SCENE, "scene[%u] not found", id);
        return ESP_ERR_NOT_FOUND;
    }

    LOG_I(TAG_SCENE, "executing scene[%u]: %s (type=%u)", id, item.label,
          static_cast<unsigned>(item.type));

    if (item.type == SignalType::BLE) {
        if (!AppIdfAppMode::isBle()) {
            LOG_W(TAG_SCENE, "scene[%u] is BLE preset but app mode is %s", id,
                  AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
            return ESP_ERR_INVALID_STATE;
        }
        return AppIdfBlePresetScenes::executePresetById(item.presetId);
    }

    if (item.type == SignalType::IR) {
        if (!AppIdfAppMode::isIr() || !AppIdfIr::isStarted()) {
            LOG_W(TAG_SCENE, "scene[%u] is IR but app mode is %s; switch to MODE=IR first", id,
                  AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
            return ESP_ERR_INVALID_STATE;
        }
        return AppIdfIr::sendLearned(item.irName, 1) ? ESP_OK : ESP_FAIL;
    }

    if (item.type == SignalType::RF433) {
        if (!AppIdfAppMode::isRf433() || !AppIdfRf433::isStarted()) {
            LOG_W(TAG_SCENE, "scene[%u] is RF433 but app mode is %s; switch to MODE=RF433 first", id,
                  AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
            return ESP_ERR_INVALID_STATE;
        }
        if (item.code433 == 0 || item.len433 == 0) {
            LOG_E(TAG_SCENE, "scene[%u] has empty RF433 payload", id);
            return ESP_ERR_INVALID_ARG;
        }
        // 走 async：把 sendCode 调度到 core 0 的 TX worker。同步直调会让
        // 调用者（KEY2 → LVGL task）持 600ms 内被 LCD DMA 中断切碎 OOK pulse。
        return AppIdfRf433::sendCodeAsync(item.code433, item.len433, item.T433);
    }

    return ESP_ERR_INVALID_ARG;
}

size_t listForCurrentMode(SceneItem* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;

    const auto mode = AppIdfAppMode::current();
    if (mode == AppIdfAppMode::Mode::BLE) {
        return AppIdfBlePresetScenes::listAll(out, cap);
    }

    const SignalType wantType =
        (mode == AppIdfAppMode::Mode::RF433) ? SignalType::RF433 : SignalType::IR;

    MutexLock lock;
    if (!lock.locked()) return 0;
    size_t n = 0;
    for (size_t i = 0; i < g_count && n < cap; i++) {
        if (g_scenes[i].type == wantType) {
            out[n++] = g_scenes[i];
        }
    }
    return n;
}

size_t labelsForServerMeta(char* outBuf, size_t outBufSize) {
    if (outBuf == nullptr || outBufSize < 3) return 0;

    const auto mode = AppIdfAppMode::current();
    if (mode == AppIdfAppMode::Mode::BLE) {
        // BLE 模式不让本地预制参与语音派发 → scene_labels 留空，语音继续走 aircon_ble_v1。
        outBuf[0] = '[';
        outBuf[1] = ']';
        outBuf[2] = '\0';
        return 2;
    }

    const SignalType wantType =
        (mode == AppIdfAppMode::Mode::RF433) ? SignalType::RF433 : SignalType::IR;

    MutexLock lock;
    if (!lock.locked()) {
        outBuf[0] = '[';
        outBuf[1] = ']';
        outBuf[2] = '\0';
        return 2;
    }

    size_t pos = 0;
    auto append = [&](const char* s) {
        const size_t n = strlen(s);
        if (pos + n + 1 >= outBufSize) return false;
        memcpy(outBuf + pos, s, n);
        pos += n;
        outBuf[pos] = '\0';
        return true;
    };

    if (!append("[")) {
        outBuf[0] = '[';
        outBuf[1] = ']';
        outBuf[2] = '\0';
        return 2;
    }

    bool first = true;
    for (size_t i = 0; i < g_count; i++) {
        if (g_scenes[i].type != wantType) continue;
        char entry[96];
        snprintf(entry, sizeof(entry), "%s{\"id\":%u,\"label\":\"%s\"}",
                 first ? "" : ",", static_cast<unsigned>(g_scenes[i].id), g_scenes[i].label);
        if (!append(entry)) {
            // 缓冲不够，退化成空数组。
            outBuf[0] = '[';
            outBuf[1] = ']';
            outBuf[2] = '\0';
            return 2;
        }
        first = false;
    }

    if (!append("]")) {
        outBuf[0] = '[';
        outBuf[1] = ']';
        outBuf[2] = '\0';
        return 2;
    }
    return pos;
}

}  // namespace AppIdfScene
