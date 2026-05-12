#include "App_IdfRf433.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "App_IdfTasks.h"
#include "App_Log.h"
#include "Pin_Config.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace AppIdfRf433 {
namespace {

// ============== 寄存器地址（CMT2300A 用户区） ==============
constexpr uint8_t CMT2300A_CUS_MODE_CTL = 0x60;
constexpr uint8_t CMT2300A_CUS_FIFO_CTL = 0x69;
constexpr uint8_t CMT2300A_CUS_INT_CLR1 = 0x6A;
constexpr uint8_t CMT2300A_CUS_INT_CLR2 = 0x6B;
constexpr uint8_t CMT2300A_CUS_FIFO_CLR = 0x6C;
constexpr uint8_t CMT2300A_CUS_IO_SEL = 0x65;
constexpr uint8_t CMT2300A_CUS_INT_EN = 0x68;

// ============== 寄存器初始化表（addr,value 交替）==============
// CMT2300A 标准运行时参数表（433.92MHz / OOK / 2.4kbps / Direct mode / AGC Off）。
// TS3260（硅传 P2P 替代芯片）也兼容这张表，但要在写它之前先写 kTs3260InitReg
// + 0x61=0x10 + 软复位完成模拟前端校准，否则灵敏度差约 13dB（见 MRF2300A
// 替代 CMT2300A 使用说明）。
const uint8_t kCmt2300aSData[] = {
    0x00, 0x00, 0x01, 0x66, 0x02, 0xEC, 0x03, 0x1C, 0x04, 0xF0, 0x05, 0x80,
    0x06, 0x14, 0x07, 0x08, 0x08, 0x91, 0x09, 0x02, 0x0A, 0x02, 0x0B, 0xD0,
    0x0C, 0xAE, 0x0D, 0xE0, 0x0E, 0x35, 0x0F, 0x00, 0x10, 0x00, 0x11, 0xF4,
    0x12, 0x10, 0x13, 0xE2, 0x14, 0x42, 0x15, 0x20, 0x16, 0x20, 0x17, 0x81,
    0x18, 0x42, 0x19, 0x71, 0x1A, 0xCE, 0x1B, 0x1C, 0x1C, 0x42, 0x1D, 0x5B,
    0x1E, 0x1C, 0x1F, 0x1C,
    0x20, 0x32, 0x21, 0x18, 0x22, 0x80, 0x23, 0xDD, 0x24, 0x00, 0x25, 0x00,
    0x26, 0x00, 0x27, 0x00, 0x28, 0x00, 0x29, 0x00, 0x2A, 0x00, 0x2B, 0x29,
    0x2C, 0xC0, 0x2D, 0x51, 0x2E, 0x2A, 0x2F, 0x4A, 0x30, 0x05, 0x31, 0x00,
    0x32, 0x50, 0x33, 0x2D, 0x34, 0x00, 0x35, 0x44, 0x36, 0x05, 0x37, 0x05,
    0x38, 0x10, 0x39, 0x08, 0x3A, 0x00, 0x3B, 0xAA, 0x3C, 0x02, 0x3D, 0x00,
    0x3E, 0x00, 0x3F, 0x00, 0x40, 0x00, 0x41, 0x00, 0x42, 0x00, 0x43, 0xD4,
    0x44, 0x2D, 0x45, 0x00, 0x46, 0x1F, 0x47, 0x00, 0x48, 0x00, 0x49, 0x00,
    0x4A, 0x00, 0x4B, 0x00, 0x4C, 0x00, 0x4D, 0x00, 0x4E, 0x00, 0x4F, 0x60,
    0x50, 0xFF, 0x51, 0x00, 0x52, 0x00, 0x53, 0x1F, 0x54, 0x10,
    0x55, 0x55, 0x56, 0x9A, 0x57, 0x0C, 0x58, 0x00, 0x59, 0x0F, 0x5A, 0xB0,
    0x5B, 0x00, 0x5C, 0x37, 0x5D, 0x0A, 0x5E, 0x3F, 0x5F, 0x7F,
};

// TS3260（硅传，CMT2300A P2P 替代）专用的前置初始化表，写到地址 0x00..0x31。
// 仅作为模拟前端校准参数，写完后软复位，随后会被 kCmt2300aSData 覆盖大部分值；
// 但软复位让模拟前端基于这份配置完成一次内部校准，是 13dB 灵敏度差距的关键。
// 末尾还要单独写一条 (0x61, 0x10)，那是 CMT2300A 表里不存在的 TS3260 扩展寄存器。
const uint8_t kTs3260InitReg[0x32] = {
    0x00, 0x66, 0xEC, 0x1D, 0xF0, 0x80, 0x14, 0x08,
    0x91, 0x02, 0x02, 0xD0,
    0xAE, 0xE0, 0x35, 0x00, 0x00, 0xF4,
    0x10, 0xE2, 0x42, 0x20, 0x00, 0x81,
    0x42, 0x71, 0xCE, 0x1C, 0x42, 0x5B, 0x1C, 0x1C,
    0x32, 0x18, 0x10, 0x99, 0xE1, 0x9B, 0x19, 0x0A,
    0x9F, 0x39, 0x29, 0x29, 0xC0, 0x51, 0x2A, 0x53,
    0x00, 0x00,
};

// ============== 协议/学习参数 ==============
constexpr uint16_t kMaxFramePulses = 256;
constexpr uint8_t kFrameQueueSize = 8;
constexpr uint16_t kFrameMinPulses = 24;
constexpr uint32_t kFrameBoundaryMinUs = 4000;
constexpr uint32_t kFrameBoundaryMaxUs = 60000;

constexpr uint32_t kLearnWaitTimeoutMs = 15000;
constexpr uint32_t kLearnCaptureMaxMs = 1200;
constexpr uint32_t kLearnCaptureSilenceMs = 220;
constexpr uint8_t kMinValidPairRatio = 85;
constexpr uint8_t kMaxDispersionPct = 25;
constexpr uint16_t kMinTUs = 150;
constexpr uint16_t kMaxTUs = 700;
constexpr uint16_t kTxDefaultTUs = 300;
constexpr uint8_t kTxRepeatCount = 8;
constexpr uint8_t kClusterTTolerancePct = 12;
constexpr uint8_t kMaxClusterLenDiff = 12;
constexpr uint8_t kHighQualityPairRatio = 94;
constexpr uint8_t kHighQualityDispersion = 25;
constexpr uint32_t kNormalListenCooldownMs = 800;
constexpr uint8_t kTriggerPairRatio = 70;
constexpr uint8_t kTriggerMaxDispersion = 45;
constexpr uint8_t kLearnMaxCandidates = 8;

constexpr uint32_t kRf433TaskStackWords = 4096;

// ============== 内部数据结构 ==============
struct DecodedSignal {
    bool valid = false;
    bool hasSync = false;
    uint64_t code = 0;
    uint8_t bitLen = 0;
    uint16_t T = 0;
    uint16_t pulseCount = 0;
    uint8_t validPairRatio = 0;
    uint8_t dispersionPct = 0;
};

enum class LearnState : uint8_t {
    WaitTrigger,
    Capturing,
    Analyzing,
};

struct RawFrame {
    uint16_t pulseCount = 0;
    uint32_t completedAtUs = 0;
    uint16_t pulses[kMaxFramePulses] = {0};
};

struct LearnClusterInfo {
    uint8_t members[kFrameQueueSize] = {0};
    uint8_t memberCount = 0;
    uint8_t representative = 0;
    uint16_t medianT = 0;
    uint8_t avgValidRatio = 0;
    uint8_t avgDispersion = 0;
    bool hasSync = false;
    int32_t score = 0;
};

// ============== 全局状态 ==============
AppIdfTasks::StaticTaskMemory g_taskMemory;
AppIdfTasks::StaticTaskMemory g_txTaskMemory;

// TX 异步队列：UI/场景把发射请求投这里，worker task (core=0) 串行处理。
// 避开 LVGL DMA 中断在 core=1 切碎 OOK pulse 的问题。
struct SendRequest {
    uint64_t code;
    uint8_t bitLen;
    uint16_t T;
};
QueueHandle_t g_sendQueue = nullptr;
constexpr uint32_t kRf433TxTaskStackWords = 3072;
SemaphoreHandle_t g_modeMutex = nullptr;
QueueHandle_t g_learnQueue = nullptr;
bool g_started = false;
bool g_isrServiceInstalled = false;
bool g_isrHandlerAdded = false;
bool g_isSniffing = false;
Mode g_currentMode = Mode::Idle;
uint32_t g_lastFrameOverflowSnapshot = 0;
uint32_t g_lastTriggerTimeMs = 0;

// 学习状态机
LearnState g_learnState = LearnState::WaitTrigger;
uint32_t g_learnWaitDeadlineMs = 0;
uint32_t g_captureStartMs = 0;
uint32_t g_captureLastValidMs = 0;
uint32_t g_lastLearnDiagMs = 0;
uint16_t g_captureRawFrameCount = 0;
uint8_t g_captureValidCount = 0;
DecodedSignal g_learnCandidates[kLearnMaxCandidates];

// ISR 共享状态
volatile uint16_t g_currentPulses[kMaxFramePulses];
volatile uint16_t g_currentPulseCount = 0;
volatile bool g_currentFrameOverflowed = false;
volatile uint32_t g_lastEdgeTimeUs = 0;

RawFrame g_frameQueue[kFrameQueueSize];
RawFrame g_loopFrameScratch;
LearnClusterInfo g_learnClusters[kFrameQueueSize];
volatile uint8_t g_frameHead = 0;
volatile uint8_t g_frameTail = 0;
volatile uint32_t g_frameOverflowCount = 0;

portMUX_TYPE g_rfMux = portMUX_INITIALIZER_UNLOCKED;

// ============== 工具函数 ==============
inline uint8_t nextQueueIndex(uint8_t index) {
    return static_cast<uint8_t>((index + 1U) % kFrameQueueSize);
}

inline uint16_t absDiffU16(uint16_t a, uint16_t b) {
    return (a > b) ? static_cast<uint16_t>(a - b) : static_cast<uint16_t>(b - a);
}

inline uint64_t lowBitsMask(uint8_t bits) {
    if (bits >= 64) {
        return UINT64_MAX;
    }
    return (1ULL << bits) - 1ULL;
}

inline uint32_t microsLow32() {
    return static_cast<uint32_t>(esp_timer_get_time());
}

inline uint32_t millisLow32() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000LL);
}

void clearReceiveBuffersLocked() {
    g_currentPulseCount = 0;
    g_currentFrameOverflowed = false;
    g_frameHead = 0;
    g_frameTail = 0;
    g_frameOverflowCount = 0;
}

void resetReceiveBuffers(uint32_t nowUs) {
    portENTER_CRITICAL(&g_rfMux);
    clearReceiveBuffersLocked();
    g_lastEdgeTimeUs = nowUs;
    portEXIT_CRITICAL(&g_rfMux);
}

void IRAM_ATTR finalizeCurrentFrameLocked(uint32_t completedAtUs) {
    if (g_currentFrameOverflowed || g_currentPulseCount < kFrameMinPulses) {
        g_currentPulseCount = 0;
        g_currentFrameOverflowed = false;
        return;
    }

    uint8_t tail = g_frameTail;
    uint8_t nextTail = nextQueueIndex(tail);
    if (nextTail == g_frameHead) {
        g_frameHead = nextQueueIndex(g_frameHead);
        g_frameOverflowCount = g_frameOverflowCount + 1;
    }

    RawFrame& slot = g_frameQueue[tail];
    slot.pulseCount = g_currentPulseCount;
    slot.completedAtUs = completedAtUs;
    for (uint16_t i = 0; i < g_currentPulseCount; ++i) {
        slot.pulses[i] = g_currentPulses[i];
    }
    g_frameTail = nextTail;

    g_currentPulseCount = 0;
    g_currentFrameOverflowed = false;
}

void IRAM_ATTR rfGpioIsr(void* arg) {
    (void)arg;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());

    portENTER_CRITICAL_ISR(&g_rfMux);
    const uint32_t duration = now - g_lastEdgeTimeUs;
    g_lastEdgeTimeUs = now;

    if (duration > kFrameBoundaryMinUs && duration < kFrameBoundaryMaxUs) {
        finalizeCurrentFrameLocked(now);
    } else if (duration >= kFrameBoundaryMaxUs) {
        g_currentPulseCount = 0;
        g_currentFrameOverflowed = false;
    } else if (!g_currentFrameOverflowed) {
        if (g_currentPulseCount < kMaxFramePulses) {
            const uint16_t idx = g_currentPulseCount;
            g_currentPulses[idx] = static_cast<uint16_t>(duration);
            g_currentPulseCount = static_cast<uint16_t>(idx + 1);
        } else {
            g_currentFrameOverflowed = true;
        }
    }

    portEXIT_CRITICAL_ISR(&g_rfMux);
}

bool popCompletedFrame(RawFrame& out, uint32_t& overflowSnapshot) {
    bool hasFrame = false;

    portENTER_CRITICAL(&g_rfMux);
    overflowSnapshot = g_frameOverflowCount;
    if (g_frameHead != g_frameTail) {
        out = g_frameQueue[g_frameHead];
        g_frameHead = nextQueueIndex(g_frameHead);
        hasFrame = true;
    }
    portEXIT_CRITICAL(&g_rfMux);

    return hasFrame;
}

esp_err_t ensureIsrServiceInstalled() {
    if (g_isrServiceInstalled) {
        return ESP_OK;
    }
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    g_isrServiceInstalled = true;
    return ESP_OK;
}

void startReceiveCapture() {
    resetReceiveBuffers(microsLow32());
    if (!g_isrHandlerAdded) {
        gpio_set_intr_type(static_cast<gpio_num_t>(PIN_RF_GPIO3), GPIO_INTR_ANYEDGE);
        const esp_err_t err = gpio_isr_handler_add(static_cast<gpio_num_t>(PIN_RF_GPIO3), rfGpioIsr, nullptr);
        if (err != ESP_OK) {
            LOG_W(TAG_RF433, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
            return;
        }
        g_isrHandlerAdded = true;
    }
    gpio_intr_enable(static_cast<gpio_num_t>(PIN_RF_GPIO3));
}

void stopReceiveCapture() {
    if (g_isrHandlerAdded) {
        gpio_intr_disable(static_cast<gpio_num_t>(PIN_RF_GPIO3));
        gpio_isr_handler_remove(static_cast<gpio_num_t>(PIN_RF_GPIO3));
        g_isrHandlerAdded = false;
    }
}

// ============== bit-banging SPI ==============
inline void rfDelayUs(uint32_t n) { esp_rom_delay_us(n); }

void cmtSpiSend(uint8_t data) {
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_SDIO), GPIO_MODE_OUTPUT);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 0);
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SDIO), (data & 0x80) ? 1 : 0);
        rfDelayUs(2);
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 1);
        rfDelayUs(2);
        data <<= 1;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 0);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SDIO), 1);
}

uint8_t cmtSpiRecv() {
    uint8_t data = 0;
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_SDIO), GPIO_MODE_INPUT);
    gpio_set_pull_mode(static_cast<gpio_num_t>(PIN_RF_SDIO), GPIO_PULLUP_ONLY);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 0);
        rfDelayUs(2);
        data <<= 1;
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 1);
        if (gpio_get_level(static_cast<gpio_num_t>(PIN_RF_SDIO))) {
            data |= 0x01;
        }
        rfDelayUs(2);
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 0);
    return data;
}

void writeReg(uint8_t addr, uint8_t data) {
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_CSB), 0);
    rfDelayUs(4);
    cmtSpiSend(addr & 0x7F);
    cmtSpiSend(data);
    rfDelayUs(2);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_CSB), 1);
}

uint8_t readReg(uint8_t addr) {
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_CSB), 0);
    rfDelayUs(4);
    cmtSpiSend(addr | 0x80);
    const uint8_t val = cmtSpiRecv();
    rfDelayUs(2);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_CSB), 1);
    return val;
}

// ============== 高层接收/发送 ==============
// 同时兼容 CMT2300A 和 TS3260 / MRF2300A（CMT2300A 的 P2P 国产替代芯片）。
// 阶段 1 写 TS3260 专用前置参数 + 0x61=0x10，软复位让模拟前端基于这份配置自校准；
// 阶段 2 写 CMT2300A 标准表配置 OOK / 2.4kbps / Direct 等运行参数；
// SLEEP→STBY 锁定寄存器后再切到 GPIO3 DOUT。CMT2300A 上跑这个序列也安全，
// 阶段 1 的值会被阶段 2 覆盖；0x61 在 CMT2300A 是 reserved，写它通常无副作用。
// 详见 server-side files 同事提供的 "MRF2300A 替代 CMT2300A 使用说明"。
void rxInit() {
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_CSB), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_FCSB), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_SCLK), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_SDIO), GPIO_MODE_OUTPUT);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_GPIO3), GPIO_MODE_INPUT);

    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_CSB), 1);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_FCSB), 1);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_SCLK), 0);

    vTaskDelay(pdMS_TO_TICKS(32));

    // 阶段 1: TS3260 / MRF2300A 前置初始化（CMT2300A 上是无害写入，被阶段 2 覆盖）
    for (size_t i = 0; i < sizeof(kTs3260InitReg); ++i) {
        writeReg(static_cast<uint8_t>(i), kTs3260InitReg[i]);
    }
    writeReg(0x61, 0x10);

    // 软复位 → STBY，让模拟前端基于阶段 1 的值完成一次内部自校准
    writeReg(0x7F, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(32));
    writeReg(CMT2300A_CUS_MODE_CTL, 0x02);
    vTaskDelay(pdMS_TO_TICKS(1));

    // 阶段 2: CMT2300A 标准运行时参数表
    for (size_t i = 0; i < sizeof(kCmt2300aSData); i += 2) {
        writeReg(kCmt2300aSData[i], kCmt2300aSData[i + 1]);
    }

    // SLEEP → STBY 锁定参数（PDF: GO_SLEEP() / GO_STBY()）
    writeReg(CMT2300A_CUS_MODE_CTL, 0x01);
    vTaskDelay(pdMS_TO_TICKS(1));
    writeReg(CMT2300A_CUS_MODE_CTL, 0x02);

    writeReg(CMT2300A_CUS_IO_SEL, 0x10);
    writeReg(CMT2300A_CUS_INT_EN, 0x00);

    LOG_I(TAG_RF433, "CMT2300A/TS3260 init done (Direct mode, 2-phase init)");
}

void rxGoReceive() {
    writeReg(CMT2300A_CUS_MODE_CTL, 0x02);
    writeReg(CMT2300A_CUS_FIFO_CLR, 0x02);
    writeReg(CMT2300A_CUS_INT_CLR1, 0xFF);
    writeReg(CMT2300A_CUS_INT_CLR2, 0xFF);
    writeReg(CMT2300A_CUS_FIFO_CTL, 0x00);
    writeReg(CMT2300A_CUS_MODE_CTL, 0x08);
}

void txGoTransmit() {
    if (g_isSniffing) {
        stopReceiveCapture();
        g_isSniffing = false;
    }

    writeReg(CMT2300A_CUS_MODE_CTL, 0x02);
    vTaskDelay(pdMS_TO_TICKS(1));

    writeReg(CMT2300A_CUS_INT_CLR1, 0xFF);
    writeReg(CMT2300A_CUS_INT_CLR2, 0xFF);
    writeReg(CMT2300A_CUS_FIFO_CLR, 0x01);
    // Direct TX 模式：IO_SEL=0x10 选 GPIO3，FIFO_CTL=0xC0 = TX_DIN_EN(bit7) +
    // TX_DIN_SEL=GPIO3(bit6)，两个 bit 同时置位才把 PA 调制源接到 GPIO3 引脚电平。
    // 中间几次实验把 FIFO_CTL 改成 0x00（误以为是关 FIFO）反而关掉了这个 enable。
    writeReg(CMT2300A_CUS_IO_SEL, 0x10);
    writeReg(CMT2300A_CUS_FIFO_CTL, 0xC0);
    writeReg(CMT2300A_CUS_INT_EN, 0x00);

    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_GPIO3), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);

    // GO_TX = 0x40 (state ID 6=TX，扫描实测确认；CMT2300A state ID 顺序：
    // 1=SLEEP 2=STBY 3=TFS 4=RFS 5=RX 6=TX)。
    writeReg(CMT2300A_CUS_MODE_CTL, 0x40);
    vTaskDelay(pdMS_TO_TICKS(1));

    const uint8_t modeSta = readReg(0x61);
    LOG_I(TAG_RF433, "TX state: STA=0x%02X (lo4=%u, want 6=TX)",
          modeSta, static_cast<unsigned>(modeSta & 0x0F));

    LOG_I(TAG_RF433, "TX Direct mode ready");
}

// ============== 解码 ==============
bool decodeSignalDetailed(const uint16_t* pulses, uint16_t pulseCount, DecodedSignal* out) {
    if (out == nullptr) return false;
    *out = DecodedSignal{};
    out->pulseCount = pulseCount;

    if (pulses == nullptr || pulseCount < kFrameMinPulses) {
        return false;
    }

    int syncIndex = -1;
    for (uint16_t i = 0; i < pulseCount; ++i) {
        if (pulses[i] > 3000 && pulses[i] < 15000) {
            syncIndex = i;
            break;
        }
    }

    uint16_t startIndex = 0;
    if (syncIndex >= 0 && static_cast<uint16_t>(syncIndex + 2) < pulseCount) {
        out->hasSync = true;
        startIndex = static_cast<uint16_t>(syncIndex + 1);
    }

    uint64_t code = 0;
    uint16_t bits = 0;
    uint32_t shortPulseSum = 0;
    uint16_t shortPulseCount = 0;
    uint16_t shortPulses[64] = {0};
    uint16_t evaluatedPairs = 0;
    uint16_t validPairs = 0;

    for (uint16_t p = startIndex; (p + 1) < pulseCount && evaluatedPairs < 64; p += 2) {
        const uint16_t highTime = pulses[p];
        const uint16_t lowTime = pulses[p + 1];
        const uint32_t cycleTime = static_cast<uint32_t>(highTime) + lowTime;
        const bool cycleValid = cycleTime > 400 && cycleTime < 3000;
        const bool looksZero =
            cycleValid && (static_cast<uint32_t>(lowTime) * 10U > static_cast<uint32_t>(highTime) * 15U);
        const bool looksOne =
            cycleValid && (static_cast<uint32_t>(highTime) * 10U > static_cast<uint32_t>(lowTime) * 15U);

        evaluatedPairs++;
        if (!looksZero && !looksOne) {
            break;
        }

        validPairs++;
        const uint16_t shortPulse = looksZero ? highTime : lowTime;
        shortPulseSum += shortPulse;
        shortPulses[shortPulseCount++] = shortPulse;

        code = (code << 1) | (looksOne ? 1ULL : 0ULL);
        bits++;
    }

    if (evaluatedPairs == 0 || shortPulseCount == 0) {
        return false;
    }

    const uint16_t avgT = static_cast<uint16_t>(shortPulseSum / shortPulseCount);
    const uint8_t validPairRatio = static_cast<uint8_t>((validPairs * 100U) / evaluatedPairs);
    uint32_t totalDeviation = 0;
    for (uint16_t i = 0; i < shortPulseCount; ++i) {
        totalDeviation += absDiffU16(shortPulses[i], avgT);
    }
    const uint16_t meanDeviation = static_cast<uint16_t>(totalDeviation / shortPulseCount);
    const uint8_t dispersionPct = (avgT == 0)
                                      ? 100
                                      : static_cast<uint8_t>((static_cast<uint32_t>(meanDeviation) * 100U) / avgT);

    out->code = code;
    out->bitLen = static_cast<uint8_t>(bits);
    out->T = avgT;
    out->validPairRatio = validPairRatio;
    out->dispersionPct = dispersionPct;
    out->valid = (bits >= 24 && avgT >= kMinTUs && avgT <= kMaxTUs && validPairRatio >= kMinValidPairRatio &&
                  dispersionPct <= kMaxDispersionPct);
    return out->valid;
}

bool isBasicLearnCandidate(const DecodedSignal& signal) {
    return signal.bitLen >= 24 && signal.T >= kMinTUs && signal.T <= kMaxTUs;
}

bool isTriggerableLearnCandidate(const DecodedSignal& signal) {
    const bool meetsLenRule = signal.hasSync ? (signal.bitLen >= 24) : (signal.bitLen >= 32);
    return isBasicLearnCandidate(signal) && meetsLenRule && signal.validPairRatio >= kTriggerPairRatio &&
           signal.dispersionPct <= kTriggerMaxDispersion;
}

bool areCandidatesCompatible(const DecodedSignal& a, const DecodedSignal& b) {
    if (!isBasicLearnCandidate(a) || !isBasicLearnCandidate(b)) {
        return false;
    }
    const uint16_t tBase = (a.T > b.T) ? a.T : b.T;
    if (absDiffU16(a.T, b.T) * 100U > tBase * kClusterTTolerancePct) {
        return false;
    }
    const DecodedSignal& longer = (a.bitLen >= b.bitLen) ? a : b;
    const DecodedSignal& shorter = (a.bitLen >= b.bitLen) ? b : a;
    if ((longer.bitLen - shorter.bitLen) > kMaxClusterLenDiff) {
        return false;
    }
    const uint64_t mask = lowBitsMask(shorter.bitLen);
    return (longer.code & mask) == (shorter.code & mask);
}

uint8_t chooseRepresentativeIndex(const uint8_t* indices, uint8_t count) {
    uint8_t best = indices[0];
    for (uint8_t i = 1; i < count; ++i) {
        const uint8_t candidate = indices[i];
        const DecodedSignal& bestSignal = g_learnCandidates[best];
        const DecodedSignal& signal = g_learnCandidates[candidate];

        bool replace = false;
        if (signal.bitLen > bestSignal.bitLen) {
            replace = true;
        } else if (signal.bitLen == bestSignal.bitLen && signal.hasSync && !bestSignal.hasSync) {
            replace = true;
        } else if (signal.bitLen == bestSignal.bitLen && signal.hasSync == bestSignal.hasSync &&
                   signal.validPairRatio > bestSignal.validPairRatio) {
            replace = true;
        } else if (signal.bitLen == bestSignal.bitLen && signal.hasSync == bestSignal.hasSync &&
                   signal.validPairRatio == bestSignal.validPairRatio &&
                   signal.dispersionPct < bestSignal.dispersionPct) {
            replace = true;
        }
        if (replace) {
            best = candidate;
        }
    }
    return best;
}

uint16_t medianOfValues(const uint16_t* values, uint8_t count) {
    if (count == 0) return 0;
    uint16_t sorted[kFrameQueueSize];
    for (uint8_t i = 0; i < count; ++i) sorted[i] = values[i];
    for (uint8_t i = 1; i < count; ++i) {
        uint16_t value = sorted[i];
        int8_t j = static_cast<int8_t>(i) - 1;
        while (j >= 0 && sorted[j] > value) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = value;
    }
    if ((count & 0x01U) != 0U) {
        return sorted[count / 2];
    }
    return static_cast<uint16_t>((sorted[count / 2 - 1] + sorted[count / 2]) / 2U);
}

uint16_t computeMedianT(const uint8_t* indices, uint8_t count) {
    uint16_t values[kFrameQueueSize] = {0};
    for (uint8_t i = 0; i < count; ++i) {
        values[i] = g_learnCandidates[indices[i]].T;
    }
    return medianOfValues(values, count);
}

uint16_t sanitizeTransmitT(uint16_t learnedT) {
    if (learnedT < kMinTUs || learnedT > kMaxTUs) {
        return kTxDefaultTUs;
    }
    return learnedT;
}

bool collapseRepeatedFrame(uint64_t code, uint8_t bitLen, uint64_t* collapsedCode, uint8_t* collapsedLen) {
    if (collapsedCode == nullptr || collapsedLen == nullptr || bitLen < 48) {
        return false;
    }
    for (uint8_t unitLen = 24; unitLen <= (bitLen / 2U); ++unitLen) {
        if ((bitLen % unitLen) != 0U) continue;
        const uint64_t mask = lowBitsMask(unitLen);
        const uint64_t unit = code & mask;
        bool allChunksSame = true;
        for (uint8_t offset = unitLen; offset < bitLen; offset = static_cast<uint8_t>(offset + unitLen)) {
            if (((code >> offset) & mask) != unit) {
                allChunksSame = false;
                break;
            }
        }
        if (allChunksSame) {
            *collapsedCode = unit;
            *collapsedLen = unitLen;
            return true;
        }
    }
    return false;
}

bool collapseZeroPaddedHalf(uint64_t code, uint8_t bitLen, uint64_t* collapsedCode, uint8_t* collapsedLen) {
    if (collapsedCode == nullptr || collapsedLen == nullptr || bitLen < 48 || (bitLen & 0x01U) != 0U) {
        return false;
    }
    const uint8_t halfLen = static_cast<uint8_t>(bitLen / 2U);
    if (halfLen < 24 || halfLen >= 64) {
        return false;
    }
    const uint64_t halfMask = lowBitsMask(halfLen);
    const uint64_t lowHalf = code & halfMask;
    const uint64_t highHalf = (code >> halfLen) & halfMask;
    if (highHalf != 0 && lowHalf == 0) {
        *collapsedCode = highHalf;
        *collapsedLen = halfLen;
        return true;
    }
    if (lowHalf != 0 && highHalf == 0) {
        *collapsedCode = lowHalf;
        *collapsedLen = halfLen;
        return true;
    }
    return false;
}

// ============== 学习状态机 ==============
void resetLearnState(uint32_t nowMs, bool logWait) {
    g_learnState = LearnState::WaitTrigger;
    g_learnWaitDeadlineMs = nowMs + kLearnWaitTimeoutMs;
    g_captureStartMs = 0;
    g_captureLastValidMs = 0;
    g_lastLearnDiagMs = 0;
    g_captureRawFrameCount = 0;
    g_captureValidCount = 0;
    for (uint8_t i = 0; i < kLearnMaxCandidates; ++i) {
        g_learnCandidates[i] = DecodedSignal{};
    }
    if (logWait) {
        LOG_I(TAG_RF433, "learn: waiting trigger (%lus window, hold the remote button)",
              static_cast<unsigned long>(kLearnWaitTimeoutMs / 1000U));
    }
}

void storeLearnCandidate(const DecodedSignal& candidate) {
    if (g_captureValidCount < kLearnMaxCandidates) {
        g_learnCandidates[g_captureValidCount++] = candidate;
    }
}

void startLearnCapture(const DecodedSignal& candidate, uint32_t nowMs) {
    resetLearnState(nowMs, false);
    g_learnState = LearnState::Capturing;
    g_captureStartMs = nowMs;
    g_captureLastValidMs = nowMs;
    g_captureRawFrameCount = 1;
    storeLearnCandidate(candidate);

    const uint32_t codeHigh = static_cast<uint32_t>(candidate.code >> 32);
    const uint32_t codeLow = static_cast<uint32_t>(candidate.code);
    LOG_I(TAG_RF433, "learn trigger: %d-bits, T=%dus, ratio=%u%%, sync=%s, 0x%08lX%08lX", candidate.bitLen,
          candidate.T, candidate.validPairRatio, candidate.hasSync ? "Y" : "N",
          static_cast<unsigned long>(codeHigh), static_cast<unsigned long>(codeLow));
}

void finishLearnCapture(uint32_t nowMs) {
    if (g_learnState != LearnState::Capturing) return;
    g_learnState = LearnState::Analyzing;
    LOG_I(TAG_RF433, "learn capture done: rawFrames=%u valid=%u took=%lums", g_captureRawFrameCount,
          g_captureValidCount, static_cast<unsigned long>(nowMs - g_captureStartMs));
}

void publishLearnEvent(const DecodedSignal& candidate, uint16_t stableT, uint8_t clusterCount, uint8_t totalValid) {
    const uint32_t codeHigh = static_cast<uint32_t>(candidate.code >> 32);
    const uint32_t codeLow = static_cast<uint32_t>(candidate.code);
    LOG_I(TAG_RF433, "learn winner: %u/%u frames, %d-bits, T=%dus, 0x%08lX%08lX", clusterCount, totalValid,
          candidate.bitLen, stableT, static_cast<unsigned long>(codeHigh), static_cast<unsigned long>(codeLow));

    if (g_learnQueue != nullptr) {
        LearnEvent evt = {};
        evt.code = candidate.code;
        evt.bitLen = candidate.bitLen;
        evt.T = stableT;
        evt.memberCount = clusterCount;
        evt.totalCandidates = totalValid;
        if (xQueueSend(g_learnQueue, &evt, 0) != pdTRUE) {
            LOG_W(TAG_RF433, "learn event queue full, dropping event");
        }
    }
}

void analyzeLearnCapture() {
    if (g_captureValidCount == 0) {
        LOG_W(TAG_RF433, "analyze: no candidates, keep waiting");
        resetReceiveBuffers(microsLow32());
        resetLearnState(millisLow32(), true);
        return;
    }

    for (uint8_t i = 0; i < kLearnMaxCandidates; ++i) {
        g_learnClusters[i] = LearnClusterInfo{};
    }
    uint8_t clusterCount = 0;

    for (uint8_t i = 0; i < g_captureValidCount; ++i) {
        bool placed = false;
        for (uint8_t c = 0; c < clusterCount && !placed; ++c) {
            bool compatible = true;
            for (uint8_t m = 0; m < g_learnClusters[c].memberCount; ++m) {
                if (!areCandidatesCompatible(g_learnCandidates[i],
                                             g_learnCandidates[g_learnClusters[c].members[m]])) {
                    compatible = false;
                    break;
                }
            }
            if (compatible) {
                g_learnClusters[c].members[g_learnClusters[c].memberCount++] = i;
                placed = true;
            }
        }
        if (!placed && clusterCount < kLearnMaxCandidates) {
            g_learnClusters[clusterCount].members[0] = i;
            g_learnClusters[clusterCount].memberCount = 1;
            clusterCount++;
        }
    }

    int8_t bestCluster = -1;
    uint8_t secondClusterCount = 0;
    for (uint8_t c = 0; c < clusterCount; ++c) {
        LearnClusterInfo& cluster = g_learnClusters[c];
        cluster.representative = chooseRepresentativeIndex(cluster.members, cluster.memberCount);
        cluster.medianT = computeMedianT(cluster.members, cluster.memberCount);

        uint16_t sumRatio = 0;
        uint16_t sumDispersion = 0;
        for (uint8_t m = 0; m < cluster.memberCount; ++m) {
            const DecodedSignal& signal = g_learnCandidates[cluster.members[m]];
            sumRatio += signal.validPairRatio;
            sumDispersion += signal.dispersionPct;
            cluster.hasSync = cluster.hasSync || signal.hasSync;
        }
        cluster.avgValidRatio = static_cast<uint8_t>(sumRatio / cluster.memberCount);
        cluster.avgDispersion = static_cast<uint8_t>(sumDispersion / cluster.memberCount);

        const DecodedSignal& repr = g_learnCandidates[cluster.representative];
        cluster.score = static_cast<int32_t>(cluster.memberCount) * 1000 +
                        static_cast<int32_t>(repr.bitLen) * 10 +
                        static_cast<int32_t>(cluster.avgValidRatio) -
                        static_cast<int32_t>(cluster.avgDispersion) +
                        (cluster.hasSync ? 20 : 0);

        if (bestCluster < 0 || cluster.score > g_learnClusters[bestCluster].score) {
            if (bestCluster >= 0) {
                secondClusterCount = g_learnClusters[bestCluster].memberCount;
            }
            bestCluster = static_cast<int8_t>(c);
        } else if (cluster.memberCount > secondClusterCount) {
            secondClusterCount = cluster.memberCount;
        }
    }

    if (bestCluster < 0) {
        LOG_W(TAG_RF433, "analyze: no cluster formed, keep waiting");
        resetReceiveBuffers(microsLow32());
        resetLearnState(millisLow32(), true);
        return;
    }

    const LearnClusterInfo& winner = g_learnClusters[bestCluster];
    const bool highQuality =
        winner.avgValidRatio >= kHighQualityPairRatio && winner.avgDispersion <= kHighQualityDispersion;
    const bool enoughFrames = (winner.memberCount >= 3) || (winner.memberCount >= 2 && highQuality);
    const bool enoughShare = winner.memberCount * 100U >= g_captureValidCount * 60U;
    const bool strongLead =
        (secondClusterCount == 0) || (winner.memberCount >= static_cast<uint8_t>(secondClusterCount * 2U));

    if (!enoughFrames || !enoughShare || !strongLead) {
        const DecodedSignal& repr = g_learnCandidates[winner.representative];
        const uint32_t codeHigh = static_cast<uint32_t>(repr.code >> 32);
        const uint32_t codeLow = static_cast<uint32_t>(repr.code);
        LOG_W(TAG_RF433, "analyze: weak win %u/%u %d-bits T=%dus 0x%08lX%08lX, keep waiting", winner.memberCount,
              g_captureValidCount, repr.bitLen, winner.medianT, static_cast<unsigned long>(codeHigh),
              static_cast<unsigned long>(codeLow));
        resetReceiveBuffers(microsLow32());
        resetLearnState(millisLow32(), true);
        return;
    }

    DecodedSignal result = g_learnCandidates[winner.representative];
    result.T = winner.medianT;
    uint64_t collapsedCode = 0;
    uint8_t collapsedLen = 0;
    if (collapseRepeatedFrame(result.code, result.bitLen, &collapsedCode, &collapsedLen)) {
        LOG_I(TAG_RF433, "learn normalized: %d -> %d bits", result.bitLen, collapsedLen);
        result.code = collapsedCode;
        result.bitLen = collapsedLen;
    } else if (collapseZeroPaddedHalf(result.code, result.bitLen, &collapsedCode, &collapsedLen)) {
        LOG_I(TAG_RF433, "learn de-padded: %d -> %d bits", result.bitLen, collapsedLen);
        result.code = collapsedCode;
        result.bitLen = collapsedLen;
    }
    publishLearnEvent(result, winner.medianT, winner.memberCount, g_captureValidCount);
    setMode(Mode::Idle);
}

void processRawFrame(const uint16_t* pulses, uint16_t pulseCount) {
    char line[1024] = {};
    int written = snprintf(line, sizeof(line), "RAW sniffed (%u pulses):", pulseCount);
    for (uint16_t i = 0; i < pulseCount && written < static_cast<int>(sizeof(line)) - 8; ++i) {
        written += snprintf(line + written, sizeof(line) - written, " %u", pulses[i]);
    }
    LOG_I(TAG_RF433, "%s", line);
}

void processListenFrame(const uint16_t* pulses, uint16_t pulseCount) {
    DecodedSignal candidate;
    if (!decodeSignalDetailed(pulses, pulseCount, &candidate)) {
        return;
    }
    if (millisLow32() - g_lastTriggerTimeMs > kNormalListenCooldownMs) {
        g_lastTriggerTimeMs = millisLow32();
        // 留给上层的去抖入口（透明网关，不主动上报）
    }
}

void processLearnFrame(const uint16_t* pulses, uint16_t pulseCount, uint32_t completedAtUs) {
    const uint32_t nowMs = completedAtUs / 1000U;
    DecodedSignal candidate;
    decodeSignalDetailed(pulses, pulseCount, &candidate);
    const bool triggerable = isTriggerableLearnCandidate(candidate);
    const bool basicCandidate = isBasicLearnCandidate(candidate);

    if (g_learnState == LearnState::WaitTrigger) {
        if (triggerable) {
            startLearnCapture(candidate, nowMs);
        } else if (basicCandidate && (g_lastLearnDiagMs == 0 || (nowMs - g_lastLearnDiagMs) >= 1500U)) {
            const uint32_t codeHigh = static_cast<uint32_t>(candidate.code >> 32);
            const uint32_t codeLow = static_cast<uint32_t>(candidate.code);
            LOG_I(TAG_RF433, "loose candidate: %d-bits T=%dus ratio=%u%% disp=%u%% sync=%s 0x%08lX%08lX",
                  candidate.bitLen, candidate.T, candidate.validPairRatio, candidate.dispersionPct,
                  candidate.hasSync ? "Y" : "N", static_cast<unsigned long>(codeHigh),
                  static_cast<unsigned long>(codeLow));
            g_lastLearnDiagMs = nowMs;
        }
        return;
    }

    if (g_learnState != LearnState::Capturing) return;

    g_captureRawFrameCount++;
    if (basicCandidate) {
        g_captureLastValidMs = nowMs;
        storeLearnCandidate(candidate);
        if (g_captureValidCount >= kLearnMaxCandidates) {
            finishLearnCapture(nowMs);
        }
    }
}

void handleLearnTimers(uint32_t nowMs) {
    if (g_learnState == LearnState::WaitTrigger) {
        if (g_learnWaitDeadlineMs != 0 && nowMs >= g_learnWaitDeadlineMs) {
            LOG_W(TAG_RF433, "learn trigger window expired, restarting");
            resetLearnState(nowMs, true);
        }
        return;
    }
    if (g_learnState == LearnState::Capturing) {
        if ((g_captureLastValidMs != 0 && (nowMs - g_captureLastValidMs) >= kLearnCaptureSilenceMs) ||
            (g_captureStartMs != 0 && (nowMs - g_captureStartMs) >= kLearnCaptureMaxMs)) {
            finishLearnCapture(nowMs);
        }
    }
    if (g_learnState == LearnState::Analyzing) {
        analyzeLearnCapture();
    }
}

// ============== 任务循环 ==============
void rf433Task(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_RF433");

    while (true) {
        AppIdfTasks::feedCurrentTaskWatchdog();
        if (!g_isSniffing) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        uint32_t overflowSnapshot = 0;
        bool handled = false;
        while (popCompletedFrame(g_loopFrameScratch, overflowSnapshot)) {
            handled = true;
            if (overflowSnapshot != g_lastFrameOverflowSnapshot) {
                LOG_W(TAG_RF433, "frame queue overflow: dropped %lu frames",
                      static_cast<unsigned long>(overflowSnapshot - g_lastFrameOverflowSnapshot));
                g_lastFrameOverflowSnapshot = overflowSnapshot;
            }
            switch (g_currentMode) {
                case Mode::SniffRaw:
                    processRawFrame(g_loopFrameScratch.pulses, g_loopFrameScratch.pulseCount);
                    break;
                case Mode::LearnCloud:
                    processLearnFrame(g_loopFrameScratch.pulses, g_loopFrameScratch.pulseCount,
                                      g_loopFrameScratch.completedAtUs);
                    break;
                case Mode::ListenNormal:
                    processListenFrame(g_loopFrameScratch.pulses, g_loopFrameScratch.pulseCount);
                    break;
                default:
                    break;
            }
            if (!g_isSniffing) break;
        }
        if (overflowSnapshot != g_lastFrameOverflowSnapshot) {
            LOG_W(TAG_RF433, "frame queue overflow: dropped %lu frames",
                  static_cast<unsigned long>(overflowSnapshot - g_lastFrameOverflowSnapshot));
            g_lastFrameOverflowSnapshot = overflowSnapshot;
        }
        if (g_currentMode == Mode::LearnCloud) {
            handleLearnTimers(millisLow32());
        }
        if (!handled) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

}  // namespace

// ============== 公开 API ==============
esp_err_t setMode(Mode mode) {
    if (g_modeMutex != nullptr && xSemaphoreTake(g_modeMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    g_currentMode = mode;
    g_lastFrameOverflowSnapshot = 0;

    if (g_isSniffing) {
        stopReceiveCapture();
        g_isSniffing = false;
    }
    resetReceiveBuffers(microsLow32());

    if (mode == Mode::Idle) {
        resetLearnState(millisLow32(), false);
        LOG_I(TAG_RF433, "mode -> idle (rx off)");
        if (g_modeMutex != nullptr) xSemaphoreGive(g_modeMutex);
        return ESP_OK;
    }

    startReceiveCapture();
    g_isSniffing = true;
    resetLearnState(millisLow32(), false);

    switch (mode) {
        case Mode::LearnCloud:
            LOG_I(TAG_RF433, "mode -> learn_cloud (hold the remote button)");
            resetLearnState(millisLow32(), true);
            break;
        case Mode::ListenNormal:
            LOG_I(TAG_RF433, "mode -> listen_normal");
            break;
        case Mode::SniffRaw:
            LOG_I(TAG_RF433, "mode -> sniff_raw");
            break;
        default:
            break;
    }

    if (g_modeMutex != nullptr) xSemaphoreGive(g_modeMutex);
    return ESP_OK;
}

Mode currentMode() { return g_currentMode; }

const char* modeNameAscii(Mode mode) {
    switch (mode) {
        case Mode::Idle:
            return "idle";
        case Mode::LearnCloud:
            return "learn_cloud";
        case Mode::ListenNormal:
            return "listen_normal";
        case Mode::SniffRaw:
            return "sniff_raw";
    }
    return "unknown";
}

esp_err_t sendCode(uint64_t code, uint8_t bitLen, uint16_t T) {
    if (bitLen == 0) {
        LOG_E(TAG_RF433, "sendCode: bitLen=0");
        return ESP_ERR_INVALID_ARG;
    }
    if (g_modeMutex != nullptr && xSemaphoreTake(g_modeMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const bool shouldResumeSniffing = (g_currentMode != Mode::Idle);
    txGoTransmit();
    vTaskDelay(pdMS_TO_TICKS(5));

    const uint16_t baseTUs = sanitizeTransmitT(T);
    uint64_t collapsedCode = 0;
    uint8_t collapsedLen = 0;
    const bool hasCollapsed = collapseRepeatedFrame(code, bitLen, &collapsedCode, &collapsedLen);
    uint64_t zeroPaddedCode = 0;
    uint8_t zeroPaddedLen = 0;
    const bool hasZeroHalf = collapseZeroPaddedHalf(code, bitLen, &zeroPaddedCode, &zeroPaddedLen);

    auto sendBurst = [](uint64_t burstCode, uint8_t burstBitLen, uint16_t burstTUs, uint8_t repeatCount) {
        for (uint8_t r = 0; r < repeatCount; ++r) {
            gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 1);
            esp_rom_delay_us(burstTUs);
            gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
            esp_rom_delay_us(static_cast<uint32_t>(burstTUs) * 31U);

            for (int i = burstBitLen - 1; i >= 0; --i) {
                const bool bitVal = ((burstCode >> i) & 0x01ULL) != 0ULL;
                if (bitVal) {
                    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 1);
                    esp_rom_delay_us(static_cast<uint32_t>(burstTUs) * 3U);
                    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
                    esp_rom_delay_us(burstTUs);
                } else {
                    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 1);
                    esp_rom_delay_us(burstTUs);
                    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
                    esp_rom_delay_us(static_cast<uint32_t>(burstTUs) * 3U);
                }
            }
        }
    };

    const uint32_t codeHigh = static_cast<uint32_t>(code >> 32);
    const uint32_t codeLow = static_cast<uint32_t>(code);
    LOG_I(TAG_RF433, "tx: %d-bits T=%dus 0x%08lX%08lX", bitLen, baseTUs,
          static_cast<unsigned long>(codeHigh), static_cast<unsigned long>(codeLow));
    sendBurst(code, bitLen, baseTUs, kTxRepeatCount);

    if (hasCollapsed) {
        LOG_I(TAG_RF433, "tx normalized burst: %d -> %d bits", bitLen, collapsedLen);
        sendBurst(collapsedCode, collapsedLen, baseTUs, kTxRepeatCount);
    } else if (hasZeroHalf) {
        LOG_I(TAG_RF433, "tx de-padded burst: %d -> %d bits", bitLen, zeroPaddedLen);
        sendBurst(zeroPaddedCode, zeroPaddedLen, baseTUs, kTxRepeatCount);
    }

    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
    vTaskDelay(pdMS_TO_TICKS(2));

    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_GPIO3), GPIO_MODE_INPUT);
    writeReg(CMT2300A_CUS_IO_SEL, 0x10);
    writeReg(CMT2300A_CUS_INT_EN, 0x00);
    writeReg(CMT2300A_CUS_FIFO_CTL, 0x00);
    rxGoReceive();

    if (shouldResumeSniffing) {
        startReceiveCapture();
        g_isSniffing = true;
    }

    const uint8_t totalRepeats = (hasCollapsed || hasZeroHalf)
                                     ? static_cast<uint8_t>(kTxRepeatCount * 2U)
                                     : kTxRepeatCount;
    LOG_I(TAG_RF433, "tx done (%d-bits x%d, RX restored)", bitLen, totalRepeats);

    if (g_modeMutex != nullptr) xSemaphoreGive(g_modeMutex);
    return ESP_OK;
}

// TX worker：绑 core 0，比 LVGL (priority=3) 更高优先级。
// LVGL 的 LCD flush DMA 中断只在 core 1 触发，所以这里跑 sendCode 时
// GPIO bit-banging 不会被切碎，OOK pulse 时序干净。
void rf433TxWorkerTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_RF433_TX");
    SendRequest req = {};
    while (true) {
        AppIdfTasks::feedCurrentTaskWatchdog();
        if (xQueueReceive(g_sendQueue, &req, pdMS_TO_TICKS(100)) == pdTRUE) {
            sendCode(req.code, req.bitLen, req.T);
        }
    }
}

esp_err_t sendCodeAsync(uint64_t code, uint8_t bitLen, uint16_t T) {
    if (bitLen == 0) return ESP_ERR_INVALID_ARG;
    if (g_sendQueue == nullptr) return ESP_ERR_INVALID_STATE;
    SendRequest req = {code, bitLen, T};
    // 队列满直接丢，避免 UI 调用方阻塞。一般用户连按 KEY2 会很快堆满 4 个槽。
    if (xQueueSend(g_sendQueue, &req, 0) != pdTRUE) {
        LOG_W(TAG_RF433, "sendCodeAsync: queue full, dropped 0x%llX", static_cast<unsigned long long>(code));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sendTest() {
    if (g_modeMutex != nullptr && xSemaphoreTake(g_modeMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    LOG_I(TAG_RF433, "tx test: 5x 5ms square wave");
    const bool shouldResumeSniffing = (g_currentMode != Mode::Idle);
    txGoTransmit();
    vTaskDelay(pdMS_TO_TICKS(5));

    for (int i = 0; i < 5; i++) {
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 1);
        esp_rom_delay_us(5000);
        gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
        esp_rom_delay_us(5000);
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_RF_GPIO3), 0);
    vTaskDelay(pdMS_TO_TICKS(2));

    gpio_set_direction(static_cast<gpio_num_t>(PIN_RF_GPIO3), GPIO_MODE_INPUT);
    writeReg(CMT2300A_CUS_IO_SEL, 0x10);
    writeReg(CMT2300A_CUS_INT_EN, 0x00);
    writeReg(CMT2300A_CUS_FIFO_CTL, 0x00);
    rxGoReceive();

    if (shouldResumeSniffing) {
        startReceiveCapture();
        g_isSniffing = true;
    }
    LOG_I(TAG_RF433, "tx test done, RX restored");

    if (g_modeMutex != nullptr) xSemaphoreGive(g_modeMutex);
    return ESP_OK;
}

esp_err_t start() {
    if (g_started) return ESP_OK;

    if (g_modeMutex == nullptr) {
        g_modeMutex = xSemaphoreCreateMutex();
    }
    if (g_learnQueue == nullptr) {
        g_learnQueue = xQueueCreate(4, sizeof(LearnEvent));
    }
    if (g_sendQueue == nullptr) {
        g_sendQueue = xQueueCreate(4, sizeof(SendRequest));
    }
    if (g_modeMutex == nullptr || g_learnQueue == nullptr || g_sendQueue == nullptr) {
        LOG_E(TAG_RF433, "mutex/queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t isrErr = ensureIsrServiceInstalled();
    if (isrErr != ESP_OK) {
        LOG_E(TAG_RF433, "gpio_install_isr_service failed: %s", esp_err_to_name(isrErr));
        return isrErr;
    }

    LOG_I(TAG_RF433, "starting RF433 (CMT2300A) FCSB=%d CSB=%d SDIO=%d SCLK=%d GPIO3=%d", PIN_RF_FCSB, PIN_RF_CSB,
          PIN_RF_SDIO, PIN_RF_SCLK, PIN_RF_GPIO3);

    rxInit();
    rxGoReceive();

    const esp_err_t taskErr = AppIdfTasks::createPinnedToCoreInternal(rf433Task, "IDF_RF433", kRf433TaskStackWords,
                                                                     nullptr, 2, 1, &g_taskMemory);
    if (taskErr != ESP_OK) {
        LOG_E(TAG_RF433, "task creation failed: %s", esp_err_to_name(taskErr));
        return taskErr;
    }

    // TX worker 绑 core 0 (LVGL 在 core 1)，priority 5 高于 LVGL 的 3，
    // 让 sendCode 的 GPIO bit-banging 远离 LCD flush DMA 中断、不会被切碎。
    const esp_err_t txTaskErr = AppIdfTasks::createPinnedToCoreInternal(
        rf433TxWorkerTask, "IDF_RF433_TX", kRf433TxTaskStackWords, nullptr, 5, 0, &g_txTaskMemory);
    if (txTaskErr != ESP_OK) {
        LOG_E(TAG_RF433, "tx worker creation failed: %s", esp_err_to_name(txTaskErr));
        return txTaskErr;
    }

    g_started = true;
    g_currentMode = Mode::Idle;
    LOG_I(TAG_RF433, "RF433 module ready (mode=idle)");
    return ESP_OK;
}

bool isStarted() { return g_started; }

QueueHandle_t learnEventQueue() { return g_learnQueue; }

uint32_t taskStackHighWatermark() {
    if (g_taskMemory.handle == nullptr) return 0;
    return uxTaskGetStackHighWaterMark(g_taskMemory.handle);
}

esp_err_t debugReadRegs(const uint8_t* addrs, uint8_t* values, uint8_t count) {
    if (!g_started) return ESP_ERR_INVALID_STATE;
    if (addrs == nullptr || values == nullptr || count == 0) return ESP_ERR_INVALID_ARG;
    if (g_modeMutex != nullptr && xSemaphoreTake(g_modeMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (uint8_t i = 0; i < count; ++i) {
        values[i] = readReg(addrs[i]);
    }
    if (g_modeMutex != nullptr) xSemaphoreGive(g_modeMutex);
    return ESP_OK;
}

}  // namespace AppIdfRf433
