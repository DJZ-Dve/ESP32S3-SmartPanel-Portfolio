#include "App_IdfRecorder.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfAudio.h"
#include "App_IdfLearnFlow.h"
#include "App_IdfPowerSave.h"
#include "App_IdfServer.h"
#include "App_IdfTasks.h"
#include "App_IdfUi.h"
#include "App_IdfVadNet.h"
#include "App_IdfWakeWord.h"
#include "App_Log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace AppIdfRecorder {
namespace {

constexpr const char* TAG_RECORDER_IDF = "IDF_RECORDER";
// 5120 → 7168：IDF_Recorder 栈承载 captureLoop + uploadPcmAndReceive + executeControlJson
// + cJSON parse 整条链，新增 local_learn_v1 协议在末尾还要叠 playLocalCue（manifest cJSON
// parse + fopen + I2S 输出），20KB 栈被压垮过；28KB 留 8KB 余量。
constexpr uint32_t kRecorderTaskStackWords = 7168;
constexpr size_t kReadChunkBytes = 1024;
constexpr uint8_t kRecordingMicGain = 70;
constexpr uint32_t kReadTimeoutMs = 500;
constexpr uint32_t kIdleNotifyWaitMs = 1000;
constexpr uint32_t kAutoStopMs = 16000;
constexpr uint32_t kVadMaxRecordMs = 12000;
constexpr uint32_t kVadInitialSkipMs = 500;
constexpr uint32_t kVadInitialSilenceUploadMs = 3000;
constexpr uint32_t kVadSpeechEndSilenceMs = 1500;
constexpr uint32_t kNotifyStart = BIT0;
constexpr uint32_t kNotifyStopUpload = BIT1;
constexpr uint32_t kNotifyCancel = BIT2;
constexpr uint32_t kNotifyUploadLast = BIT3;
constexpr uint32_t kNotifyWakeInteraction = BIT4;

enum class CaptureMode {
    Manual,
    WakeVad,
};

SemaphoreHandle_t g_mutex = nullptr;
AppIdfTasks::StaticTaskMemory g_taskMemory;
uint8_t* g_recordBuffer = nullptr;
Snapshot g_snapshot;
uint32_t g_recordStartMs = 0;
bool g_resumeWakeAfterCapture = false;

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
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstSize, "%s", src);
}

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ensureRecordBuffer() {
    if (g_recordBuffer != nullptr) {
        return ESP_OK;
    }

    g_recordBuffer =
        static_cast<uint8_t*>(heap_caps_malloc(kMaxRecordBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_recordBuffer == nullptr) {
        LOG_E(TAG_RECORDER_IDF, "Failed to allocate %u-byte PSRAM record buffer", static_cast<unsigned>(kMaxRecordBytes));
        return ESP_ERR_NO_MEM;
    }

    LOG_I(TAG_RECORDER_IDF, "Record buffer allocated in PSRAM: %u bytes", static_cast<unsigned>(kMaxRecordBytes));
    return ESP_OK;
}

esp_err_t setStateError(esp_err_t err, const char* summary) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.lastError = err;
        copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), summary);
    }
    return err;
}

void setRecordingState(bool recording, bool startPending, bool uploading, const char* summary) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }
    g_snapshot.recording = recording;
    g_snapshot.startPending = startPending;
    g_snapshot.uploading = uploading;
    if (summary != nullptr) {
        copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), summary);
    }
    if (recording) {
        g_snapshot.lastUploadOk = false;
    } else {
        g_snapshot.lastRms = 0;
    }
}

void updateProgress(uint32_t recordedBytes, uint32_t droppedBytes) {
    MutexLock lock(pdMS_TO_TICKS(20));
    if (!lock.locked()) {
        return;
    }
    g_snapshot.recordedBytes = recordedBytes;
    g_snapshot.droppedBytes = droppedBytes;
    g_snapshot.durationMs = (recordedBytes * 1000U) / (AppIdfAudio::kSampleRateHz * sizeof(int16_t));
}

bool consumeRecorderCommand(uint32_t* bits) {
    if (bits == nullptr) {
        return false;
    }
    *bits = 0;
    return xTaskNotifyWait(0, UINT32_MAX, bits, 0) == pdTRUE;
}

esp_err_t prepareAudioForRecording(uint8_t* scratch, size_t scratchLen) {
    if (scratch == nullptr || scratchLen == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = AppIdfAudio::start();
    if (err != ESP_OK) {
        return err;
    }
    AppIdfAudio::setPaEnabled(false);
    err = AppIdfAudio::enableMicChannel();
    if (err != ESP_OK) {
        return err;
    }
    err = AppIdfAudio::setMicGain(kRecordingMicGain);
    if (err != ESP_OK) {
        AppIdfAudio::disableMicChannel();
        return err;
    }

    for (int i = 0; i < 3; ++i) {
        size_t bytesRead = 0;
        (void)AppIdfAudio::readPcm(scratch, scratchLen, &bytesRead, kReadTimeoutMs);
    }
    return ESP_OK;
}

esp_err_t uploadCurrentRecording() {
    Snapshot beforeUpload = snapshot();
    if (beforeUpload.recordedBytes == 0 || g_recordBuffer == nullptr) {
        return setStateError(ESP_ERR_INVALID_SIZE, "no_recording_data");
    }
    if ((beforeUpload.recordedBytes % sizeof(int16_t)) != 0) {
        return setStateError(ESP_ERR_INVALID_SIZE, "recording_not_16bit_aligned");
    }

    setRecordingState(false, false, true, "uploading");
    LOG_I(TAG_RECORDER_IDF,
          "[LAT] upload_call bytes=%u duration_ms=%u",
          static_cast<unsigned>(beforeUpload.recordedBytes),
          static_cast<unsigned>(beforeUpload.durationMs));
    AppIdfTasks::feedCurrentTaskWatchdog();

    bool cuePlayedByExecutor = false;
    const esp_err_t err =
        AppIdfServer::uploadPcmAndReceive(reinterpret_cast<const int16_t*>(g_recordBuffer),
                                          beforeUpload.recordedBytes / sizeof(int16_t),
                                          &cuePlayedByExecutor);

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.uploading = false;
            g_snapshot.lastUploadOk = err == ESP_OK;
            g_snapshot.lastError = err;
            if (err == ESP_OK) {
                ++g_snapshot.uploadCount;
                copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "upload_ok");
            } else {
                copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), esp_err_to_name(err));
            }
        }
    }

    // Only play network_failed if the transport itself failed AND the executor
    // hasn't already played a cue. This prevents the double-cue regression where
    // a BLE write failure (which played op_failed inside the executor) would
    // also trigger network_failed here, telling the user "网络出问题了" when
    // network was fine.
    if (err != ESP_OK && !cuePlayedByExecutor) {
        const esp_err_t cueErr = AppIdfAudio::playLocalCue("network_failed", 8000);
        if (cueErr != ESP_OK) {
            LOG_W(TAG_RECORDER_IDF, "network_failed cue skipped: %s", esp_err_to_name(cueErr));
        }
    }

    AppIdfTasks::feedCurrentTaskWatchdog();
    return err;
}

void finishRecording(bool uploadAfterStop, const char* summary) {
    AppIdfAudio::disableMicChannel();
    if (uploadAfterStop) {
        setRecordingState(false, false, true, summary);
    } else {
        setRecordingState(false, false, false, summary);
        return;
    }

    (void)AppIdfAudio::playGeneratedCue("record_stop", 2000);
    (void)uploadCurrentRecording();
}

void maybeResumeWakeWord() {
    if (!g_resumeWakeAfterCapture) {
        return;
    }
    g_resumeWakeAfterCapture = false;
    const esp_err_t err = AppIdfWakeWord::resume();
    if (err != ESP_OK) {
        LOG_W(TAG_RECORDER_IDF, "WakeWord resume after recording failed: %s", esp_err_to_name(err));
    }
}

void updateWakeUi(AppIdfUi::AiStatus status, bool trackRecorder, uint32_t autoExitDelayMs = 0) {
    if (!AppIdfUi::isStarted()) {
        return;
    }
    const esp_err_t err = AppIdfUi::showAiStatus(status, trackRecorder, autoExitDelayMs);
    if (err != ESP_OK) {
        LOG_D(TAG_RECORDER_IDF, "AI UI update skipped: %s", esp_err_to_name(err));
    }
}

void captureLoop(CaptureMode mode) {
    esp_err_t err = ensureRecordBuffer();
    if (err != ESP_OK) {
        setRecordingState(false, false, false, "record_buffer_alloc_failed");
        setStateError(err, "record_buffer_alloc_failed");
        maybeResumeWakeWord();
        return;
    }

    uint8_t* scratch = static_cast<uint8_t*>(heap_caps_malloc(kReadChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
        scratch = static_cast<uint8_t*>(heap_caps_malloc(kReadChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (scratch == nullptr) {
        setRecordingState(false, false, false, "scratch_alloc_failed");
        setStateError(ESP_ERR_NO_MEM, "scratch_alloc_failed");
        maybeResumeWakeWord();
        return;
    }

    AppIdfVadNet::Detector vadNet;
    int16_t* vadBuffer = nullptr;
    size_t vadBufferFill = 0;
    bool hasSpeech = false;
    bool isSilence = false;
    bool uploadAfterInitialSilence = false;
    uint32_t silenceStartMs = 0;
    bool vadReady = false;

    if (mode == CaptureMode::WakeVad) {
        err = vadNet.begin("model");
        if (err != ESP_OK) {
            heap_caps_free(scratch);
            setRecordingState(false, false, false, "vadnet_init_failed");
            setStateError(err, "vadnet_init_failed");
            maybeResumeWakeWord();
            return;
        }
        vadBuffer = static_cast<int16_t*>(
            heap_caps_malloc(vadNet.frameSamples() * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (vadBuffer == nullptr) {
            vadBuffer = static_cast<int16_t*>(
                heap_caps_malloc(vadNet.frameSamples() * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (vadBuffer == nullptr) {
            vadNet.end();
            heap_caps_free(scratch);
            setRecordingState(false, false, false, "vad_buffer_alloc_failed");
            setStateError(ESP_ERR_NO_MEM, "vad_buffer_alloc_failed");
            maybeResumeWakeWord();
            return;
        }
    }

    err = prepareAudioForRecording(scratch, kReadChunkBytes);
    if (err != ESP_OK) {
        if (vadBuffer != nullptr) {
            heap_caps_free(vadBuffer);
        }
        vadNet.end();
        heap_caps_free(scratch);
        setRecordingState(false, false, false, "audio_prepare_failed");
        setStateError(err, "audio_prepare_failed");
        maybeResumeWakeWord();
        return;
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.startPending = false;
            g_snapshot.recording = true;
            g_snapshot.uploading = false;
            g_snapshot.recordedBytes = 0;
            g_snapshot.droppedBytes = 0;
            g_snapshot.durationMs = 0;
            g_snapshot.lastError = ESP_OK;
            g_snapshot.lastUploadOk = false;
            ++g_snapshot.startCount;
            copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "recording");
        }
    }

    memset(g_recordBuffer, 0, kMaxRecordBytes);
    g_recordStartMs = nowMs();
    uint32_t recordedBytes = 0;
    uint32_t droppedBytes = 0;
    uint32_t zeroReadCount = 0;
    bool uploadAfterStop = false;
    const char* finishSummary = mode == CaptureMode::WakeVad ? "vad_recording_stopped" : "recording_stopped";
    const uint32_t autoStopMs = mode == CaptureMode::WakeVad ? kVadMaxRecordMs : kAutoStopMs;

    LOG_I(TAG_RECORDER_IDF, "[LAT] rec_start mode=%d max=%u bytes",
          static_cast<int>(mode),
          static_cast<unsigned>(kMaxRecordBytes));
    while (true) {
        uint32_t bits = 0;
        if (consumeRecorderCommand(&bits)) {
            if ((bits & kNotifyCancel) != 0) {
                finishSummary = "recording_canceled";
                uploadAfterStop = false;
                break;
            }
            if ((bits & kNotifyStopUpload) != 0) {
                finishSummary = "recording_stopped";
                uploadAfterStop = true;
                break;
            }
        }

        const uint32_t elapsedMs = nowMs() - g_recordStartMs;
        if (elapsedMs >= autoStopMs) {
            finishSummary = mode == CaptureMode::WakeVad ? "vad_recording_auto_stopped" : "recording_auto_stopped";
            uploadAfterStop = true;
            break;
        }

        size_t bytesRead = 0;
        err = AppIdfAudio::readPcm(scratch, kReadChunkBytes, &bytesRead, kReadTimeoutMs);
        if (err != ESP_OK) {
            if (err == ESP_ERR_TIMEOUT) {
                if (mode == CaptureMode::WakeVad && !hasSpeech && elapsedMs > kVadInitialSilenceUploadMs) {
                    uploadAfterInitialSilence = true;
                    finishSummary = "vad_initial_no_audio";
                    uploadAfterStop = true;
                    LOG_I(TAG_RECORDER_IDF,
                          "VADNet: no audio data for %u ms, ending and uploading",
                          static_cast<unsigned>(kVadInitialSilenceUploadMs));
                    break;
                }
                ++zeroReadCount;
                if ((zeroReadCount % 20U) == 0) {
                    LOG_W(TAG_RECORDER_IDF, "I2S read timeout while recording");
                }
                AppIdfTasks::feedCurrentTaskWatchdog();
                continue;
            }
            finishSummary = "recording_read_failed";
            uploadAfterStop = false;
            setStateError(err, finishSummary);
            break;
        }

        const size_t alignedBytes = bytesRead & ~static_cast<size_t>(1);
        if (alignedBytes == 0) {
            ++zeroReadCount;
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        zeroReadCount = 0;

        {
            const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(scratch);
            const size_t sampleCount = alignedBytes / sizeof(int16_t);
            int64_t sumSq = 0;
            for (size_t i = 0; i < sampleCount; ++i) {
                sumSq += static_cast<int64_t>(pcmSamples[i]) * pcmSamples[i];
            }
            const uint32_t rms =
                sampleCount > 0 ? static_cast<uint32_t>(sqrt(static_cast<double>(sumSq / static_cast<int64_t>(sampleCount)))) : 0;
            MutexLock rmsLock(pdMS_TO_TICKS(10));
            if (rmsLock.locked()) {
                g_snapshot.lastRms = rms;
            }
        }

        const uint32_t space = kMaxRecordBytes - recordedBytes;
        const uint32_t copyBytes = alignedBytes < space ? static_cast<uint32_t>(alignedBytes) : space;
        if (copyBytes > 0) {
            memcpy(g_recordBuffer + recordedBytes, scratch, copyBytes);
            recordedBytes += copyBytes;
        }
        if (copyBytes < alignedBytes) {
            droppedBytes += static_cast<uint32_t>(alignedBytes - copyBytes);
        }
        updateProgress(recordedBytes, droppedBytes);

        if (mode == CaptureMode::WakeVad && vadBuffer != nullptr && vadNet.isReady()) {
            int16_t* pcm = reinterpret_cast<int16_t*>(scratch);
            const size_t samples = alignedBytes / sizeof(int16_t);
            size_t offset = 0;
            while (offset < samples) {
                const size_t need = vadNet.frameSamples() - vadBufferFill;
                const size_t available = samples - offset;
                const size_t copySamples = available < need ? available : need;
                memcpy(vadBuffer + vadBufferFill, pcm + offset, copySamples * sizeof(int16_t));
                vadBufferFill += copySamples;
                offset += copySamples;

                if (vadBufferFill < vadNet.frameSamples()) {
                    continue;
                }
                vadBufferFill = 0;

                const uint32_t vadElapsedMs = nowMs() - g_recordStartMs;
                if (vadElapsedMs < kVadInitialSkipMs) {
                    continue;
                }
                if (!vadReady) {
                    vadReady = true;
                    LOG_I(TAG_RECORDER_IDF, "VADNet enabled for wake interaction");
                }

                if (vadNet.isSpeech(vadBuffer)) {
                    if (!hasSpeech) {
                        LOG_I(TAG_RECORDER_IDF, "VADNet: speech start detected");
                        hasSpeech = true;
                    }
                    isSilence = false;
                } else {
                    if (!hasSpeech && vadElapsedMs > kVadInitialSilenceUploadMs) {
                        uploadAfterInitialSilence = true;
                        finishSummary = "vad_initial_silence_upload";
                        uploadAfterStop = true;
                        LOG_I(TAG_RECORDER_IDF,
                              "VADNet: initial silence reached %u ms, ending and uploading",
                              static_cast<unsigned>(kVadInitialSilenceUploadMs));
                        goto capture_done;
                    }
                    if (hasSpeech && !isSilence) {
                        isSilence = true;
                        silenceStartMs = nowMs();
                        LOG_D(TAG_RECORDER_IDF, "VADNet: speech ended, starting silence timer");
                    }
                    if (isSilence && nowMs() - silenceStartMs > kVadSpeechEndSilenceMs) {
                        finishSummary = "vad_speech_end_silence";
                        uploadAfterStop = true;
                        LOG_I(TAG_RECORDER_IDF,
                              "[LAT] vad_end silence=%u ms",
                              static_cast<unsigned>(kVadSpeechEndSilenceMs));
                        goto capture_done;
                    }
                }
            }
        }

        if (recordedBytes >= kMaxRecordBytes) {
            finishSummary = "recording_buffer_full";
            uploadAfterStop = true;
            break;
        }

        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

capture_done:
    if (mode == CaptureMode::WakeVad && !hasSpeech && !uploadAfterInitialSilence && !uploadAfterStop) {
        finishSummary = "vad_no_speech";
        uploadAfterStop = false;
    }
    if (vadBuffer != nullptr) {
        heap_caps_free(vadBuffer);
    }
    vadNet.end();
    heap_caps_free(scratch);
    updateProgress(recordedBytes, droppedBytes);
    LOG_I(TAG_RECORDER_IDF,
          "[LAT] rec_finish bytes=%u dropped=%u duration_ms=%u upload=%d",
          static_cast<unsigned>(recordedBytes),
          static_cast<unsigned>(droppedBytes),
          static_cast<unsigned>((recordedBytes * 1000U) / (AppIdfAudio::kSampleRateHz * sizeof(int16_t))),
          uploadAfterStop ? 1 : 0);
    finishRecording(uploadAfterStop, finishSummary);
    maybeResumeWakeWord();
}

void wakeInteractionLoop() {
    updateWakeUi(AppIdfUi::AiStatus::Ack, true);
    (void)AppIdfAudio::playLocalCue("wake_ack", 8000);
    vTaskDelay(pdMS_TO_TICKS(100));
    updateWakeUi(AppIdfUi::AiStatus::Listening, true);

    g_resumeWakeAfterCapture = true;
    captureLoop(CaptureMode::WakeVad);

    // local_learn_v1 等协议已经把屏切到学习屏；不要再用 AIScreen Success/Error 抢屏并 1500ms 后回主屏。
    if (AppIdfLearnFlow::isActive()) {
        return;
    }
    const Snapshot after = snapshot();
    updateWakeUi(after.lastUploadOk ? AppIdfUi::AiStatus::Success : AppIdfUi::AiStatus::Error, false, 1500);
}

void recorderTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Recorder");
    while (true) {
        uint32_t bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(kIdleNotifyWaitMs)) != pdTRUE) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            continue;
        }

        if ((bits & kNotifyStart) != 0) {
            const uint32_t deferredStopBits = bits & (kNotifyStopUpload | kNotifyCancel);
            if (deferredStopBits != 0) {
                xTaskNotify(g_taskMemory.handle, deferredStopBits, eSetBits);
            }
            const bool wakeWasRunning = AppIdfWakeWord::isRunning();
            AppIdfWakeWord::pause();
            g_resumeWakeAfterCapture = wakeWasRunning;
            // 与 wake-word 路径（wakeInteractionLoop）对齐：先播 wake_ack 提示音让用户
            // 听到 "叮" 再开口。否则 I2S mic channel enable + 96ms warmup discard
            // 期间用户已经开始说话，前几个字（如 "开启空调" 的 "开启"）被吃掉。
            (void)AppIdfAudio::playLocalCue("wake_ack", 3000);
            // IDF_EXEC 会无条件把 AIScreen 切到 EXECUTING；只有真发生过 upload 时才翻 Success/Error，
            // 否则维持现状（例如纯按键测试或 capture 阶段就退出，没必要骚扰 UI）。
            const Snapshot before = snapshot();
            captureLoop(CaptureMode::Manual);
            const Snapshot after = snapshot();
            // local_learn_v1 等协议已切到学习屏，不要再用 AIScreen Success/Error 抢屏。
            if ((after.uploadCount > before.uploadCount || after.lastError != ESP_OK) &&
                !AppIdfLearnFlow::isActive()) {
                updateWakeUi(after.lastUploadOk ? AppIdfUi::AiStatus::Success : AppIdfUi::AiStatus::Error,
                             false, 1500);
            }
        }
        if ((bits & kNotifyWakeInteraction) != 0) {
            wakeInteractionLoop();
        }
        if ((bits & kNotifyUploadLast) != 0) {
            (void)uploadCurrentRecording();
            const Snapshot after = snapshot();
            updateWakeUi(after.lastUploadOk ? AppIdfUi::AiStatus::Success : AppIdfUi::AiStatus::Error,
                         false, 1500);
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

esp_err_t notifyRecorder(uint32_t bits, const char* summary) {
    if (g_taskMemory.handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (summary != nullptr) {
        setStateError(ESP_OK, summary);
    }
    return xTaskNotify(g_taskMemory.handle, bits, eSetBits) == pdTRUE ? ESP_OK : ESP_FAIL;
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

    err = ensureRecordBuffer();
    if (err != ESP_OK) {
        setStateError(err, "record_buffer_alloc_failed");
        return err;
    }
    err = AppIdfTasks::createPinnedToCoreInternal(recorderTask, "IDF_Recorder", kRecorderTaskStackWords, nullptr, 3, 1,
                                                  &g_taskMemory);
    if (err != ESP_OK) {
        setStateError(err, "task_create_failed");
        return err;
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.started = true;
        g_snapshot.maxRecordBytes = kMaxRecordBytes;
        g_snapshot.lastError = ESP_OK;
        copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "ready");
    }
    LOG_I(TAG_RECORDER_IDF, "IDF recorder ready");
    return ESP_OK;
}

bool isStarted() {
    return g_taskMemory.handle != nullptr;
}

bool isBusy() {
    const Snapshot snap = snapshot();
    return snap.startPending || snap.recording || snap.uploading;
}

bool isRecordingActive() {
    const Snapshot snap = snapshot();
    return snap.startPending || snap.recording;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

esp_err_t startRecording() {
    AppIdfPowerSave::notifyActivity();
    esp_err_t err = start();
    if (err != ESP_OK) {
        return err;
    }
    if (AppFlashGuard::isActive()) {
        return setStateError(ESP_ERR_INVALID_STATE, "flash_guard_active");
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        if (g_snapshot.startPending || g_snapshot.recording || g_snapshot.uploading) {
            return ESP_ERR_INVALID_STATE;
        }
        g_snapshot.startPending = true;
        g_snapshot.lastUploadOk = false;
        g_snapshot.lastError = ESP_OK;
        copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "start_requested");
    }

    return notifyRecorder(kNotifyStart, nullptr);
}

esp_err_t stopRecordingAndUpload() {
    const Snapshot snap = snapshot();
    if (!snap.startPending && !snap.recording) {
        return setStateError(ESP_ERR_INVALID_STATE, "not_recording");
    }
    return notifyRecorder(kNotifyStopUpload, "stop_upload_requested");
}

esp_err_t cancelRecording() {
    const Snapshot snap = snapshot();
    if (!snap.startPending && !snap.recording) {
        return setStateError(ESP_ERR_INVALID_STATE, "not_recording");
    }
    return notifyRecorder(kNotifyCancel, "cancel_requested");
}

esp_err_t uploadLastRecording() {
    const Snapshot snap = snapshot();
    if (snap.startPending || snap.recording || snap.uploading) {
        return setStateError(ESP_ERR_INVALID_STATE, "recorder_busy");
    }
    if (snap.recordedBytes == 0) {
        return setStateError(ESP_ERR_INVALID_SIZE, "no_recording_data");
    }
    return notifyRecorder(kNotifyUploadLast, "upload_requested");
}

esp_err_t startWakeInteraction() {
    AppIdfPowerSave::notifyActivity();
    esp_err_t err = start();
    if (err != ESP_OK) {
        return err;
    }
    if (AppFlashGuard::isActive()) {
        return setStateError(ESP_ERR_INVALID_STATE, "flash_guard_active");
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (!lock.locked()) {
            return ESP_ERR_TIMEOUT;
        }
        if (g_snapshot.startPending || g_snapshot.recording || g_snapshot.uploading) {
            return ESP_ERR_INVALID_STATE;
        }
        g_snapshot.startPending = true;
        g_snapshot.lastUploadOk = false;
        g_snapshot.lastError = ESP_OK;
        copyText(g_snapshot.lastSummary, sizeof(g_snapshot.lastSummary), "wake_requested");
    }

    g_resumeWakeAfterCapture = AppIdfWakeWord::hasResidentTasks();
    return notifyRecorder(kNotifyWakeInteraction, nullptr);
}

uint32_t taskStackHighWatermark() {
    return g_taskMemory.handle != nullptr ? uxTaskGetStackHighWaterMark(g_taskMemory.handle) : 0;
}

}  // namespace AppIdfRecorder
