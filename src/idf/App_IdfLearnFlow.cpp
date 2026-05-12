#include "App_IdfLearnFlow.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfIr.h"
#include "App_IdfRf433.h"
#include "App_IdfScene.h"
#include "App_Log.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace AppIdfLearnFlow {
namespace {

constexpr const char* TAG = "LEARN";
constexpr uint32_t kCaptureTimeoutMs = 30000;
constexpr uint32_t kLabelTimeoutMs = 45000;  // 含录音 + 上传 + AI 时间

State g_state = State::Idle;
CaptureKind g_kind = CaptureKind::None;
bool g_irFirstCaptured = false;
// 二次按键与第一次不一致后置位；下一次第一次按键到来或离开 Capturing 时清。
bool g_irMismatch = false;
char g_irName[AppIdfIr::kMaxNameLen] = {};
char g_hashHex[40] = {};
uint16_t g_irRawLen = 0;
uint64_t g_rfCode = 0;
uint8_t  g_rfBitLen = 0;
uint16_t g_rfT = 0;
uint64_t g_deadlineUs = 0;
uint32_t g_learnSeq = 0;
TaskHandle_t g_pollTaskHandle = nullptr;

uint64_t nowUs() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

void setDeadlineMs(uint32_t ms) {
    g_deadlineUs = nowUs() + static_cast<uint64_t>(ms) * 1000ULL;
}

void resetSnapshot() {
    g_state = State::Idle;
    g_kind = CaptureKind::None;
    g_irFirstCaptured = false;
    g_irMismatch = false;
    memset(g_irName, 0, sizeof(g_irName));
    memset(g_hashHex, 0, sizeof(g_hashHex));
    g_irRawLen = 0;
    g_rfCode = 0;
    g_rfBitLen = 0;
    g_rfT = 0;
    g_deadlineUs = 0;
}

void cancelUnderlyingLocked() {
    if (g_kind == CaptureKind::IR && AppIdfIr::isLearning()) {
        AppIdfIr::stopLearning();
    } else if (g_kind == CaptureKind::RF433 && AppIdfRf433::currentMode() == AppIdfRf433::Mode::LearnCloud) {
        AppIdfRf433::setMode(AppIdfRf433::Mode::Idle);
    }
}

void pollIrEvent() {
    QueueHandle_t q = AppIdfIr::learnEventQueue();
    if (q == nullptr) return;
    AppIdfIr::LearnEvent ev = {};
    while (xQueueReceive(q, &ev, 0) == pdTRUE) {
        if (g_state != State::Capturing || g_kind != CaptureKind::IR) continue;
        if (ev.stage == AppIdfIr::LearnStage::Confirmed && ev.learningSuccess) {
            g_irRawLen = ev.rawLen;
            strncpy(g_hashHex, ev.signatureHash, sizeof(g_hashHex) - 1);
            g_irFirstCaptured = false;
            g_irMismatch = false;
            g_state = State::AwaitingLabel;
            setDeadlineMs(kLabelTimeoutMs);
            LOG_I(TAG, "IR captured ir_name=%s rawLen=%u; awaiting voice label",
                  g_irName, static_cast<unsigned>(g_irRawLen));
            AppIdfAudio::playLocalCue("learn_say_name", 5000);
        } else if (ev.stage == AppIdfIr::LearnStage::FirstCaptured) {
            // 第一次按键已收到，等待第二次相同按键确认；引导用户再按一次。
            // 这是一次新的"第一次"，无论之前是否 mismatch 都清掉旧标志。
            g_irMismatch = false;
            if (!g_irFirstCaptured) {
                g_irFirstCaptured = true;
                LOG_I(TAG, "IR first capture; awaiting second press");
                AppIdfAudio::playLocalCue("learn_press_again", 5000);
            }
        } else if (ev.stage == AppIdfIr::LearnStage::Mismatch) {
            // 二次不一致，IR 模块自动回到等待第一次状态；同步清 firstStage 并设 mismatch。
            // mismatch 在下一次 FirstCaptured 事件到来时自动清。
            if (g_irFirstCaptured) {
                g_irFirstCaptured = false;
                g_irMismatch = true;
                LOG_W(TAG, "IR mismatch; restart from first press");
            }
        } else if (ev.stage == AppIdfIr::LearnStage::SaveFailed) {
            LOG_W(TAG, "IR save failed; resetting flow");
            cancelUnderlyingLocked();
            resetSnapshot();
            AppIdfAudio::playLocalCue("op_failed", 5000);
        }
    }
}

void pollRfEvent() {
    QueueHandle_t q = AppIdfRf433::learnEventQueue();
    if (q == nullptr) return;
    AppIdfRf433::LearnEvent ev = {};
    while (xQueueReceive(q, &ev, 0) == pdTRUE) {
        if (g_state != State::Capturing || g_kind != CaptureKind::RF433) continue;
        g_rfCode = ev.code;
        g_rfBitLen = ev.bitLen;
        g_rfT = ev.T;
        snprintf(g_hashHex, sizeof(g_hashHex), "%016" PRIX64, ev.code);
        g_state = State::AwaitingLabel;
        setDeadlineMs(kLabelTimeoutMs);
        LOG_I(TAG, "RF433 captured code=0x%016" PRIX64 " bits=%u T=%u; awaiting voice label",
              g_rfCode, static_cast<unsigned>(g_rfBitLen), static_cast<unsigned>(g_rfT));
        // RF433 不需要继续接收，关 LearnCloud 节省功耗。
        AppIdfRf433::setMode(AppIdfRf433::Mode::Idle);
        AppIdfAudio::playLocalCue("learn_say_name", 5000);
    }
}

[[noreturn]] void pollTask(void*) {
    for (;;) {
        if (g_state != State::Idle) {
            pollIrEvent();
            pollRfEvent();
            if (g_state != State::Idle && g_state != State::UploadingLabel &&
                g_deadlineUs != 0 && nowUs() > g_deadlineUs) {
                LOG_W(TAG, "learn flow timeout (state=%u); reset", static_cast<unsigned>(g_state));
                cancelUnderlyingLocked();
                resetSnapshot();
                AppIdfAudio::playLocalCue("op_failed", 5000);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

}  // namespace

esp_err_t start() {
    if (g_pollTaskHandle != nullptr) return ESP_OK;
    // BLE 模式不需要轮询 IR/RF433 学习队列；省下 4KB internal SRAM 给 NimBLE 连接用，
    // 否则 BLE 配对会因 largest_internal < 14KB 直接 abort（见 App_IdfBleAircon 的 SRAM 守门）。
    if (AppIdfAppMode::isBle()) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(pollTask, "IDF_LEARN", 4096, nullptr, 2, &g_pollTaskHandle, 1);
    if (ok != pdPASS) {
        g_pollTaskHandle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool isActive() { return g_state != State::Idle; }
State currentState() { return g_state; }
CaptureKind captureKind() { return g_kind; }
bool irFirstStageCaptured() { return g_irFirstCaptured; }
bool irMismatchPending() {
    return g_irMismatch && g_state == State::Capturing && g_kind == CaptureKind::IR;
}
const char* hashHex() { return g_hashHex; }
uint16_t lastIrRawLen() { return g_irRawLen; }

esp_err_t startCapture() {
    if (g_state != State::Idle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (AppIdfAppMode::isIr() && AppIdfIr::isStarted()) {
        ++g_learnSeq;
        snprintf(g_irName, sizeof(g_irName), "learn_%lu",
                 static_cast<unsigned long>(g_learnSeq));
        const esp_err_t err = AppIdfIr::startLearning(g_irName);
        if (err != ESP_OK) {
            resetSnapshot();
            return err;
        }
        g_kind = CaptureKind::IR;
    } else if (AppIdfAppMode::isRf433() && AppIdfRf433::isStarted()) {
        const esp_err_t err = AppIdfRf433::setMode(AppIdfRf433::Mode::LearnCloud);
        if (err != ESP_OK) {
            return err;
        }
        g_kind = CaptureKind::RF433;
    } else {
        // BLE 模式或对应模块未启动。
        return ESP_ERR_INVALID_STATE;
    }

    g_state = State::Capturing;
    g_irFirstCaptured = false;
    g_irRawLen = 0;
    g_rfCode = 0;
    g_rfBitLen = 0;
    g_rfT = 0;
    g_hashHex[0] = '\0';
    setDeadlineMs(kCaptureTimeoutMs);
    AppIdfAudio::playLocalCue("learn_press_key", 5000);
    LOG_I(TAG, "learn capture start: kind=%s", g_kind == CaptureKind::IR ? "ir" : "rf433");
    return ESP_OK;
}

esp_err_t cancel() {
    if (g_state == State::Idle) return ESP_OK;
    LOG_I(TAG, "learn flow cancelled (state=%u)", static_cast<unsigned>(g_state));
    cancelUnderlyingLocked();
    resetSnapshot();
    return ESP_OK;
}

esp_err_t onLabelArrived(const char* label) {
    if (label == nullptr || label[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (g_state != State::AwaitingLabel && g_state != State::UploadingLabel) {
        LOG_W(TAG, "label arrived in unexpected state %u", static_cast<unsigned>(g_state));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_FAIL;
    if (g_kind == CaptureKind::IR) {
        err = AppIdfScene::addSceneIr(label, g_irName);
    } else if (g_kind == CaptureKind::RF433) {
        err = AppIdfScene::addSceneRf433(label, g_rfCode, g_rfBitLen, g_rfT);
    }

    if (err == ESP_OK) {
        LOG_I(TAG, "scene saved: label=\"%s\" kind=%s", label,
              g_kind == CaptureKind::IR ? "ir" : "rf433");
        resetSnapshot();
        AppIdfAudio::playLocalCue("done", 5000);
    } else {
        LOG_E(TAG, "scene save failed: %s", esp_err_to_name(err));
        // 退回 AwaitingLabel 让用户重录或长按 KEY1 取消。
        g_state = State::AwaitingLabel;
        setDeadlineMs(kLabelTimeoutMs);
        AppIdfAudio::playLocalCue("op_failed", 5000);
    }
    return err;
}

esp_err_t onUploadFailed() {
    if (g_state == State::UploadingLabel || g_state == State::AwaitingLabel) {
        g_state = State::AwaitingLabel;
        setDeadlineMs(kLabelTimeoutMs);
    }
    return ESP_OK;
}

size_t describePendingForMeta(char* outBuf, size_t cap) {
    if (outBuf == nullptr || cap < 4) return 0;
    if (g_state != State::AwaitingLabel && g_state != State::UploadingLabel) {
        outBuf[0] = '\0';
        return 0;
    }
    int n = 0;
    if (g_kind == CaptureKind::IR) {
        n = snprintf(outBuf, cap,
                     "{\"kind\":\"ir\",\"hash\":\"%s\",\"raw_len\":%u}",
                     g_hashHex, static_cast<unsigned>(g_irRawLen));
    } else if (g_kind == CaptureKind::RF433) {
        n = snprintf(outBuf, cap,
                     "{\"kind\":\"rf433\",\"hash\":\"%s\",\"bits\":%u,\"T\":%u}",
                     g_hashHex, static_cast<unsigned>(g_rfBitLen),
                     static_cast<unsigned>(g_rfT));
    } else {
        outBuf[0] = '\0';
        return 0;
    }
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        outBuf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

uint32_t remainingMs() {
    if (g_state == State::Idle || g_deadlineUs == 0) return 0;
    const uint64_t now = nowUs();
    if (now >= g_deadlineUs) return 0;
    return static_cast<uint32_t>((g_deadlineUs - now) / 1000ULL);
}

}  // namespace AppIdfLearnFlow
