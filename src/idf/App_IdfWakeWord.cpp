#include "App_IdfWakeWord.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfAudio.h"
#include "App_IdfPowerSave.h"
#include "App_IdfRecorder.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

namespace AppIdfWakeWord {
namespace {

constexpr const char* TAG_WAKE_IDF = "IDF_WAKE";
constexpr uint32_t kFeedStackWords = 3072;
constexpr uint32_t kFetchStackWords = 4096;
constexpr uint32_t kStartFreeMarginBytes = 1024;
constexpr uint32_t kStartLargestMarginBytes = 128;
constexpr BaseType_t kWakeCore = 1;
constexpr uint32_t kWakeCooldownMs = 2000;
constexpr uint8_t kWakeMicGain = 70;

SemaphoreHandle_t g_mutex = nullptr;
Snapshot g_snapshot;
srmodel_list_t* g_models = nullptr;
const esp_afe_sr_iface_t* g_afe = nullptr;
esp_afe_sr_data_t* g_afeData = nullptr;
bool g_running = false;
bool g_paused = false;
bool g_needFlush = false;
bool g_stopping = false;

StackType_t* g_feedStack = nullptr;
StaticTask_t* g_feedTcb = nullptr;
TaskHandle_t g_feedTask = nullptr;
StackType_t* g_fetchStack = nullptr;
StaticTask_t* g_fetchTcb = nullptr;
TaskHandle_t g_fetchTask = nullptr;

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

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    snprintf(dst, dstSize, "%s", src != nullptr ? src : "");
}

esp_err_t setLastError(esp_err_t err) {
    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.lastError = err;
    }
    return err;
}

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

char* findWakeModel(srmodel_list_t* models) {
    if (models == nullptr) {
        return nullptr;
    }

    char* wakeName = esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr);
    if (wakeName != nullptr) {
        return wakeName;
    }

    for (int i = 0; i < models->num; ++i) {
        const char* name = models->model_name[i];
        if (name != nullptr && (strstr(name, "wn9") != nullptr || strstr(name, "nihaoxiaoan") != nullptr)) {
            return models->model_name[i];
        }
    }
    return nullptr;
}

void logModelList(srmodel_list_t* models) {
    if (models == nullptr) {
        return;
    }
    LOG_I(TAG_WAKE_IDF, "ESP-SR model count=%d", models->num);
    for (int i = 0; i < models->num; ++i) {
        LOG_I(TAG_WAKE_IDF,
              "model[%d]=%s info=%s",
              i,
              models->model_name[i] != nullptr ? models->model_name[i] : "-",
              models->model_info[i] != nullptr ? models->model_info[i] : "-");
    }
}

esp_err_t createAfe(char* wakeName) {
    afe_config_t* config = afe_config_init("M", g_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == nullptr) {
        LOG_E(TAG_WAKE_IDF, "AFE config allocation failed");
        return ESP_ERR_NO_MEM;
    }

    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->vad_init = false;
    config->wakenet_init = true;
    config->wakenet_model_name = wakeName;
    config->wakenet_model_name_2 = nullptr;
    config->wakenet_mode = DET_MODE_95;
    config->agc_init = false;
    config->pcm_config.mic_num = 1;
    config->pcm_config.ref_num = 0;
    config->pcm_config.total_ch_num = 1;
    config->pcm_config.sample_rate = AppIdfAudio::kSampleRateHz;
    config->afe_perferred_core = kWakeCore;
    config->afe_perferred_priority = 5;
    config->afe_ringbuf_size = 25;
    config->afe_linear_gain = 1.0f;
    config->fixed_first_channel = true;
    config->fixed_output_channel = true;
    config->output_playback_channel = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    const esp_afe_sr_iface_t* afe = esp_afe_handle_from_config(config);
    if (afe == nullptr) {
        afe_config_free(config);
        LOG_E(TAG_WAKE_IDF, "No AFE handle for current config");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_afe_sr_data_t* afeData = afe->create_from_config(config);
    if (afeData == nullptr) {
        LOG_W(TAG_WAKE_IDF, "AFE PSRAM allocation failed, retrying balanced internal/PSRAM mode");
        config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
        afeData = afe->create_from_config(config);
    }
    afe_config_free(config);

    if (afeData == nullptr) {
        LOG_E(TAG_WAKE_IDF, "AFE create failed");
        return ESP_ERR_NO_MEM;
    }

    g_afe = afe;
    g_afeData = afeData;
    if (g_afe->print_pipeline != nullptr) {
        g_afe->print_pipeline(g_afeData);
    }

    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.feedChunkSamples = g_afe->get_feed_chunksize(g_afeData);
        g_snapshot.fetchChunkSamples = g_afe->get_fetch_chunksize(g_afeData);
        g_snapshot.feedChannels =
            g_afe->get_feed_channel_num != nullptr ? g_afe->get_feed_channel_num(g_afeData) : g_afe->get_channel_num(g_afeData);
        g_snapshot.fetchChannels = g_afe->get_fetch_channel_num != nullptr ? g_afe->get_fetch_channel_num(g_afeData) : 1;
    }

    return ESP_OK;
}

esp_err_t allocateTaskMemory(uint32_t stackWords, StackType_t** stackOut, StaticTask_t** tcbOut, const char* name) {
    if (stackOut == nullptr || tcbOut == nullptr || name == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *stackOut =
        static_cast<StackType_t*>(heap_caps_malloc(stackWords * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    *tcbOut = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (*stackOut == nullptr || *tcbOut == nullptr) {
        LOG_E(TAG_WAKE_IDF, "Failed to allocate internal task memory for %s", name);
        if (*stackOut != nullptr) {
            heap_caps_free(*stackOut);
        }
        if (*tcbOut != nullptr) {
            heap_caps_free(*tcbOut);
        }
        *stackOut = nullptr;
        *tcbOut = nullptr;
        return ESP_ERR_NO_MEM;
    }
    memset(*stackOut, 0, stackWords * sizeof(StackType_t));
    memset(*tcbOut, 0, sizeof(StaticTask_t));
    return ESP_OK;
}

void releaseTaskMemory(TaskHandle_t task, StackType_t*& stack, StaticTask_t*& tcb) {
    if (task != nullptr) {
        return;
    }
    if (stack != nullptr) {
        heap_caps_free(stack);
        stack = nullptr;
    }
    if (tcb != nullptr) {
        heap_caps_free(tcb);
        tcb = nullptr;
    }
}

esp_err_t enableWakeMic() {
    esp_err_t err = AppIdfAudio::start();
    if (err != ESP_OK) {
        return err;
    }
    AppIdfAudio::setPaEnabled(false);
    err = AppIdfAudio::enableMicChannel();
    if (err != ESP_OK) {
        return err;
    }
    err = AppIdfAudio::setMicGain(kWakeMicGain);
    if (err == ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(50));
        if (lock.locked()) {
            g_snapshot.micEnabled = true;
        }
        LOG_I(TAG_WAKE_IDF, "ES8311 ADC enabled for WakeNet");
    }
    return err;
}

void disableWakeMic() {
    (void)AppIdfAudio::disableMicChannel();
    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.micEnabled = false;
    }
    LOG_I(TAG_WAKE_IDF, "ES8311 ADC disabled for WakeNet");
}

void feedTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("WW_Feed");
    LOG_I(TAG_WAKE_IDF, "Wake feed task started");
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t err = enableWakeMic();
    if (err != ESP_OK) {
        LOG_E(TAG_WAKE_IDF, "Wake mic enable failed: %s", esp_err_to_name(err));
        g_running = false;
    }

    const int feedSamples = g_afe != nullptr && g_afeData != nullptr ? g_afe->get_feed_chunksize(g_afeData) : 0;
    const int feedChannels = (g_afe != nullptr && g_afeData != nullptr && g_afe->get_feed_channel_num != nullptr)
                                 ? g_afe->get_feed_channel_num(g_afeData)
                                 : 1;
    const size_t bytesNeeded = static_cast<size_t>(feedSamples) * static_cast<size_t>(feedChannels) * sizeof(int16_t);
    int16_t* audioBuffer = nullptr;
    if (bytesNeeded > 0) {
        audioBuffer = static_cast<int16_t*>(heap_caps_malloc(bytesNeeded, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (audioBuffer == nullptr) {
            audioBuffer = static_cast<int16_t*>(heap_caps_malloc(bytesNeeded, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }
    if (audioBuffer == nullptr || bytesNeeded == 0) {
        LOG_E(TAG_WAKE_IDF, "Failed to allocate WakeNet feed buffer (%u bytes)", static_cast<unsigned>(bytesNeeded));
        g_running = false;
    }

    uint32_t lastLogMs = nowMs();
    uint32_t feedCountInWindow = 0;
    while (g_running) {
        while (g_paused && g_running) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!g_running) {
            break;
        }

        if (g_needFlush) {
            for (int i = 0; i < 5; ++i) {
                size_t dropped = 0;
                (void)AppIdfAudio::readPcm(audioBuffer, bytesNeeded, &dropped, 250);
            }
            g_needFlush = false;
            LOG_D(TAG_WAKE_IDF, "I2S stale frames flushed for WakeNet");
        }

        size_t bytesRead = 0;
        err = AppIdfAudio::readPcm(audioBuffer, bytesNeeded, &bytesRead, 1000);
        if (err != ESP_OK || bytesRead < bytesNeeded) {
            if (err != ESP_ERR_TIMEOUT) {
                LOG_W(TAG_WAKE_IDF, "WakeNet I2S read failed: %s bytes=%u/%u",
                      esp_err_to_name(err),
                      static_cast<unsigned>(bytesRead),
                      static_cast<unsigned>(bytesNeeded));
            }
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        g_afe->feed(g_afeData, audioBuffer);
        ++feedCountInWindow;
        {
            MutexLock lock(pdMS_TO_TICKS(10));
            if (lock.locked()) {
                ++g_snapshot.feedCount;
            }
        }

        const uint32_t currentMs = nowMs();
        if (currentMs - lastLogMs > 5000) {
            int64_t sumSq = 0;
            const size_t sampleCount = bytesRead / sizeof(int16_t);
            for (size_t i = 0; i < sampleCount; ++i) {
                sumSq += static_cast<int64_t>(audioBuffer[i]) * audioBuffer[i];
            }
            const uint32_t rms = sampleCount > 0 ? static_cast<uint32_t>(sqrt(static_cast<double>(sumSq / sampleCount))) : 0;
            MutexLock lock(pdMS_TO_TICKS(10));
            if (lock.locked()) {
                g_snapshot.lastRms = rms;
            }
            LOG_D(TAG_WAKE_IDF, "[Feed] RMS=%u, feed=%u/5s", static_cast<unsigned>(rms), static_cast<unsigned>(feedCountInWindow));
            feedCountInWindow = 0;
            lastLogMs = currentMs;
        }

        AppIdfTasks::feedCurrentTaskWatchdog();
        taskYIELD();
    }

    if (audioBuffer != nullptr) {
        heap_caps_free(audioBuffer);
    }
    disableWakeMic();
    LOG_I(TAG_WAKE_IDF, "Wake feed task exited");
    g_feedTask = nullptr;
    AppIdfTasks::unregisterCurrentTaskWatchdog();
    vTaskDelete(nullptr);
}

void fetchTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("WW_Fetch");
    LOG_I(TAG_WAKE_IDF, "Wake fetch task started");
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint32_t lastLogMs = nowMs();
    uint32_t fetchCountInWindow = 0;

    while (g_running) {
        while (g_paused && g_running) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!g_running) {
            break;
        }

        afe_fetch_result_t* result = nullptr;
        if (g_afe->fetch_with_delay != nullptr) {
            result = g_afe->fetch_with_delay(g_afeData, pdMS_TO_TICKS(500));
        } else {
            result = g_afe->fetch(g_afeData);
        }
        ++fetchCountInWindow;
        {
            MutexLock lock(pdMS_TO_TICKS(10));
            if (lock.locked()) {
                ++g_snapshot.fetchCount;
            }
        }

        const uint32_t currentMs = nowMs();
        if (result != nullptr && result->wakeup_state == WAKENET_DETECTED &&
            currentMs - g_snapshot.lastWakeMs > kWakeCooldownMs) {
            LOG_I(TAG_WAKE_IDF,
                  "[LAT] wake_detected index=%d model_index=%d volume=%.1f",
                  result->wake_word_index,
                  result->wakenet_model_index,
                  result->data_volume);
            AppIdfPowerSave::notifyActivity();
            (void)AppIdfPowerSave::exitL1();
            pause();
            {
                MutexLock lock(pdMS_TO_TICKS(20));
                if (lock.locked()) {
                    ++g_snapshot.wakeCount;
                    g_snapshot.lastWakeMs = currentMs;
                }
            }
            const esp_err_t startErr = AppIdfRecorder::startWakeInteraction();
            if (startErr != ESP_OK) {
                LOG_W(TAG_WAKE_IDF, "Wake interaction start failed: %s", esp_err_to_name(startErr));
                (void)resume();
            }
        }

        if (currentMs - lastLogMs > 5000) {
            LOG_D(TAG_WAKE_IDF, "[Fetch] fetch=%u/5s", static_cast<unsigned>(fetchCountInWindow));
            fetchCountInWindow = 0;
            lastLogMs = currentMs;
        }

        AppIdfTasks::feedCurrentTaskWatchdog();
    }

    LOG_I(TAG_WAKE_IDF, "Wake fetch task exited");
    g_fetchTask = nullptr;
    AppIdfTasks::unregisterCurrentTaskWatchdog();
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t init() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }
    if (g_afeData != nullptr) {
        return ESP_OK;
    }

    LOG_I(TAG_WAKE_IDF, "Initializing ESP-SR WakeWord");
    g_models = get_static_srmodels();
    if (g_models == nullptr) {
        g_models = esp_srmodel_init("model");
    }
    if (g_models == nullptr) {
        LOG_E(TAG_WAKE_IDF, "ESP-SR model load failed from model partition");
        return setLastError(ESP_ERR_NOT_FOUND);
    }
    logModelList(g_models);

    char* wakeName = findWakeModel(g_models);
    if (wakeName == nullptr) {
        LOG_E(TAG_WAKE_IDF, "WakeNet model not found");
        return setLastError(ESP_ERR_NOT_FOUND);
    }

    err = createAfe(wakeName);
    if (err != ESP_OK) {
        return setLastError(err);
    }

    char* wakeWords = esp_srmodel_get_wake_words(g_models, wakeName);
    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.initialized = true;
        g_snapshot.modelCount = g_models->num;
        copyText(g_snapshot.wakeModelName, sizeof(g_snapshot.wakeModelName), wakeName);
        copyText(g_snapshot.wakeWords, sizeof(g_snapshot.wakeWords), wakeWords != nullptr ? wakeWords : "");
        g_snapshot.lastError = ESP_OK;
    }

    LOG_I(TAG_WAKE_IDF, "WakeWord initialized: model=%s words=%s", wakeName, wakeWords != nullptr ? wakeWords : "-");
    return ESP_OK;
}

esp_err_t start() {
    if (AppFlashGuard::isActive() && !AppFlashGuard::isRestoringWakeWord()) {
        return setLastError(ESP_ERR_INVALID_STATE);
    }

    esp_err_t err = init();
    if (err != ESP_OK) {
        return err;
    }
    if (g_feedTask != nullptr || g_fetchTask != nullptr) {
        return ESP_OK;
    }

    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t requiredFree =
        (kFeedStackWords + kFetchStackWords) * sizeof(StackType_t) + (2U * sizeof(StaticTask_t)) + kStartFreeMarginBytes;
    const uint32_t requiredLargest = (kFetchStackWords * sizeof(StackType_t)) + kStartLargestMarginBytes;
    if (internalFree < requiredFree || internalLargest < requiredLargest) {
        LOG_E(TAG_WAKE_IDF,
              "Internal SRAM too low for WakeWord: free=%u largest=%u need_free=%u need_largest=%u",
              static_cast<unsigned>(internalFree),
              static_cast<unsigned>(internalLargest),
              static_cast<unsigned>(requiredFree),
              static_cast<unsigned>(requiredLargest));
        return setLastError(ESP_ERR_NO_MEM);
    }

    err = allocateTaskMemory(kFeedStackWords, &g_feedStack, &g_feedTcb, "WW_Feed");
    if (err == ESP_OK) {
        err = allocateTaskMemory(kFetchStackWords, &g_fetchStack, &g_fetchTcb, "WW_Fetch");
    }
    if (err != ESP_OK) {
        releaseTaskMemory(nullptr, g_feedStack, g_feedTcb);
        releaseTaskMemory(nullptr, g_fetchStack, g_fetchTcb);
        return setLastError(err);
    }

    g_running = true;
    g_paused = false;
    g_needFlush = true;
    g_stopping = false;

    g_feedTask =
        xTaskCreateStaticPinnedToCore(feedTask, "WW_Feed", kFeedStackWords, nullptr, 7, g_feedStack, g_feedTcb, kWakeCore);
    g_fetchTask = xTaskCreateStaticPinnedToCore(fetchTask,
                                               "WW_Fetch",
                                               kFetchStackWords,
                                               nullptr,
                                               5,
                                               g_fetchStack,
                                               g_fetchTcb,
                                               kWakeCore);
    if (g_feedTask == nullptr || g_fetchTask == nullptr) {
        LOG_E(TAG_WAKE_IDF, "WakeWord task creation failed feed=%p fetch=%p", g_feedTask, g_fetchTask);
        g_running = false;
        for (int i = 0; i < 40 && (g_feedTask != nullptr || g_fetchTask != nullptr); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        releaseTaskMemory(g_feedTask, g_feedStack, g_feedTcb);
        releaseTaskMemory(g_fetchTask, g_fetchStack, g_fetchTcb);
        return setLastError(ESP_FAIL);
    }

    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.running = true;
        g_snapshot.paused = false;
        g_snapshot.feedTaskResident = true;
        g_snapshot.fetchTaskResident = true;
        g_snapshot.lastError = ESP_OK;
    }
    LOG_I(TAG_WAKE_IDF, "WakeWord tasks started");
    return ESP_OK;
}

void stop() {
    if (!g_running && g_feedTask == nullptr && g_fetchTask == nullptr) {
        return;
    }
    g_stopping = true;
    g_running = false;
    g_paused = false;
    disableWakeMic();

    for (int i = 0; i < 50 && (g_feedTask != nullptr || g_fetchTask != nullptr); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (g_feedTask != nullptr || g_fetchTask != nullptr) {
        LOG_W(TAG_WAKE_IDF, "WakeWord tasks did not exit before timeout");
    }

    releaseTaskMemory(g_feedTask, g_feedStack, g_feedTcb);
    releaseTaskMemory(g_fetchTask, g_fetchStack, g_fetchTcb);
    g_stopping = false;

    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.running = false;
        g_snapshot.paused = false;
        g_snapshot.feedTaskResident = g_feedTask != nullptr;
        g_snapshot.fetchTaskResident = g_fetchTask != nullptr;
    }
    LOG_I(TAG_WAKE_IDF, "WakeWord stopped");
}

void pause() {
    if (!g_running) {
        return;
    }
    g_paused = true;
    g_needFlush = true;
    MutexLock lock(pdMS_TO_TICKS(20));
    if (lock.locked()) {
        g_snapshot.paused = true;
        g_snapshot.running = false;
    }
    LOG_D(TAG_WAKE_IDF, "WakeWord paused");
}

esp_err_t resume() {
    if (AppFlashGuard::isActive() && !AppFlashGuard::isRestoringWakeWord()) {
        return setLastError(ESP_ERR_INVALID_STATE);
    }
    if (g_stopping) {
        return setLastError(ESP_ERR_INVALID_STATE);
    }
    if (g_afeData == nullptr) {
        return start();
    }
    if (g_feedTask == nullptr || g_fetchTask == nullptr) {
        return start();
    }
    if (!g_paused) {
        return ESP_OK;
    }

    g_needFlush = true;
    esp_err_t err = enableWakeMic();
    if (err != ESP_OK) {
        return setLastError(err);
    }
    vTaskDelay(pdMS_TO_TICKS(80));

    if (g_afe != nullptr && g_afeData != nullptr) {
        if (g_afe->reset_buffer != nullptr) {
            const int resetResult = g_afe->reset_buffer(g_afeData);
            LOG_I(TAG_WAKE_IDF, "AFE buffer reset result=%d", resetResult);
        }
        if (g_afe->reset_wakenet_threshold != nullptr) {
            const int thresholdResult = g_afe->reset_wakenet_threshold(g_afeData, 1);
            LOG_I(TAG_WAKE_IDF, "WakeNet threshold reset result=%d", thresholdResult);
        }
        if (g_afe->enable_wakenet != nullptr) {
            (void)g_afe->enable_wakenet(g_afeData);
        }
    }

    g_paused = false;
    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        g_snapshot.paused = false;
        g_snapshot.running = true;
        g_snapshot.lastError = ESP_OK;
    }
    LOG_D(TAG_WAKE_IDF, "WakeWord resumed");
    return ESP_OK;
}

bool isRunning() {
    return g_running && !g_paused;
}

bool hasResidentTasks() {
    return g_feedTask != nullptr || g_fetchTask != nullptr;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    Snapshot snap = g_snapshot;
    snap.running = g_running && !g_paused;
    snap.paused = g_paused;
    snap.feedTaskResident = g_feedTask != nullptr;
    snap.fetchTaskResident = g_fetchTask != nullptr;
    return snap;
}

uint32_t feedTaskStackHighWatermark() {
    return g_feedTask != nullptr ? uxTaskGetStackHighWaterMark(g_feedTask) : 0;
}

uint32_t fetchTaskStackHighWatermark() {
    return g_fetchTask != nullptr ? uxTaskGetStackHighWaterMark(g_fetchTask) : 0;
}

}  // namespace AppIdfWakeWord
