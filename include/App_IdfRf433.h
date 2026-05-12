#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace AppIdfRf433 {

enum class Mode : uint8_t {
    Idle = 0,
    LearnCloud = 1,
    ListenNormal = 2,
    SniffRaw = 3,
};

// 学习成功时塞进 learnEventQueue，由上层（场景模块/服务器上报）取走。
struct LearnEvent {
    uint64_t code;
    uint8_t bitLen;
    uint16_t T;             // 短脉冲基本宽度（微秒）
    uint8_t memberCount;    // 胜出簇成员数
    uint8_t totalCandidates;
};

esp_err_t start();
bool isStarted();

Mode currentMode();
const char* modeNameAscii(Mode mode);
esp_err_t setMode(Mode mode);

// 同步发射：当前 task 自己跑完 600ms bit-banging。仅用于 console / 调试入口，
// **绝不要在 LVGL task 上下文里直接调**，否则 LCD flush DMA 中断会切碎 OOK pulse。
esp_err_t sendCode(uint64_t code, uint8_t bitLen, uint16_t T);

// 异步发射：把请求投到 RF TX worker task（绑 core 0，避开 LVGL DMA 中断），
// 立刻返回。UI/场景路径必须走这个，避免发射期间 GPIO 时序被 LVGL 打断。
esp_err_t sendCodeAsync(uint64_t code, uint8_t bitLen, uint16_t T);

// 发一段固定测试波形（5 个 5ms 高低脉冲），用来确认 TX 路径硬件通。
esp_err_t sendTest();

// 调试用：读 CMT2300A 寄存器组，验证 SPI 通信与芯片状态是否符合预期。
// 走 g_modeMutex，避免和 RF 任务的 SPI 操作并发。
esp_err_t debugReadRegs(const uint8_t* addrs, uint8_t* values, uint8_t count);

QueueHandle_t learnEventQueue();
uint32_t taskStackHighWatermark();

}  // namespace AppIdfRf433
