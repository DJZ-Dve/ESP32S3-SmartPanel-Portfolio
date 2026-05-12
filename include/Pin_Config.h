/**
 * @file Pin_Config.h
 * @brief ESP32-S3 硬件引脚定义 (量产板 V2)
 * @details 
 * Board: ESP32-S3FN8 (内置8MB Flash + 外挂8MB PSRAM)
 * Display: ST7789P3 (240x240)
 * Audio: ES8311 + NS4150B
 * Wireless: LE270-EU (4G), BLE air conditioner control
 * Framework: Arduino IDE / LVGL 8.3.11
 * 
 * @warning GPIO8 引脚冲突已解决 (LCD_CLK 已改至 GPIO14)
 *          当前 BLE 固件不启用 IR / 433 / 本地场景学习
 */

#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <stdint.h>
#endif

/* =================================================================
 * 1. 屏幕接口 (Display - ST7789P3)
 * 型号: HD154010C10
 * 分辨率: 240*240
 * 尺寸: 1.54英寸
 * 通信: 4-wire SPI
 * ================================================================= */
#define PIN_TFT_SCLK        14  // LCD_CLK
#define PIN_TFT_MOSI        7   // LCD_DAT (SPI_MOSI)
#define PIN_TFT_MISO        -1  // 未使用
#define PIN_TFT_CS          10  // LCD_CS
#define PIN_TFT_DC          11  // LCD_DC/DA
#define PIN_TFT_RST         17  // LCD_RESET
#define PIN_TFT_BL          18  // LCD_背光驱动


/* =================================================================
 * 2. 音频接口 (Audio - ES8311 & NS4150B)
 * 通信: I2S + I2C
 * ================================================================= */
// I2C 控制接口 (ES8311)
#define PIN_I2C_SDA         47  // I2S_SDA
#define PIN_I2C_SCL         48  // I2S_SCL

// I2S 数据接口
#define PIN_I2S_MCLK        42  // I2S_MCK -> ES8311_MCLK
#define PIN_I2S_BCLK        41  // I2S_BCK -> ES8311_BCLK
#define PIN_I2S_LRCK        39  // I2S_WS -> ES8311_WS (Word Select/LR Clock)
#define PIN_I2S_DOUT        38  // I2S_DO -> ES8311_DOUT (ESP输出 -> 喇叭)
#define PIN_I2S_DIN         40  // I2S_DI -> ES8311_DIN (Mic输入 -> ESP)

// 功放使能 (NS4150B)
#define PIN_PA_EN           21  // PA_EN_NS4150B (高电平有效)


/* =================================================================
 * 3. 4G 模块 (LTE Cat.1 - LE270-EU)
 * 通信: UART2
 * 引脚与旧版保持一致
 * ================================================================= */
#define PIN_4G_PWR      45  // 主电源控制 (高电平开启)
#define PIN_4G_PWRKEY   46  // 开机键 (4V 域，硬件下拉默认低；启动需 ESP32 拉低 2.5s 复位脉冲)
#define PIN_4G_RX       12  // 连接模块的 TXD
#define PIN_4G_TX       13  // 连接模块的 RXD
// PIN_4G_NET 已取消 (原 GPIO14 已改分配给 LCD_CLK/PIN_TFT_SCLK)

/* =================================================================
 * 4. 433MHz 射频模块预留引脚 (CMT2300A)
 * 当前 BLE 固件不启用该模块，下面定义仅作为硬件资料保留。
 * 
 * GPIO8 原与 LCD_CLK 冲突，LCD_CLK 已迁移至 GPIO14，冲突已消除。
 * 433 模块与屏幕使用完全独立的引脚和通信方式（bit-banging vs 硬件SPI）。
 * ================================================================= */
#define PIN_RF_FCSB         4   // CMT2300T_FCSB
#define PIN_RF_CSB          5   // CMT2300T_CSB (片选)
#define PIN_RF_SDIO         6   // CMT2300T_SDIO (双向数据)
#define PIN_RF_SCLK         8   // CMT2300T_SCLK (时钟) — GPIO8 冲突已消除
#define PIN_RF_GPIO3        9   // CMT2300T_GPIO3 (中断/状态)


/* =================================================================
 * 5. 输入与传感器 (Input & Sensors)
 * ================================================================= */
// ADC 按键 (3个按键通过分压电阻连入同一个 ADC)
// 按下不同按键时 ADC 读取到不同的电压值
// 电路: +3.3V → Rkey → KEYn(加热键) → 节点A → R14(1K) → GPIO3
//        节点A → R16(1K) → GND,  C44(104) 滤波
// S1: R15=2K  → 理论 1100mV (切换/下一个, 长按=返回)
// S2: R17=3.9K → 理论 673mV (确定/进入, 长按=AI录音)
#define PIN_ADC_KEY         3   // ADC 按键输入

// ADC 按键电压阈值 (单位: mV) — 2键硬件
#define ADC_KEYS_CONFIGURED
#define ADC_KEY2_MIN        550   // S2 (确定) 最小电压 (mV)  理论: 673mV
#define ADC_KEY2_MAX        800   // S2 (确定) 最大电压 (mV)
#define ADC_KEY1_MIN        950   // S1 (切换) 最小电压 (mV) 理论: 1100mV
#define ADC_KEY1_MAX        1200  // S1 (切换) 最大电压 (mV)  [收紧：理论1100，留100mV余量]
#define ADC_KEY_BOTH_MIN    1350  // S1+S2 同时按下 (理论约 1421mV) [间距150mV，抗ADC噪声]
#define ADC_KEY_BOTH_MAX    1550
#define ADC_KEY_NONE_MIN    50    // 无按键按下时的电压 (接近 0V, 被 R16 下拉)

// 电池电量检测
#define PIN_ADC_BAT         1   // AD_BAT 电池电量 ADC 检测
#define PIN_CHARGE_DETECT   37  // 充电检测输入 (高电平=检测到充电)

// 温度检测
#define PIN_ADC_TEMP        2   // GPIO2 -> ADC_TEMP (NTC热敏电阻分压)

// 红外线 (IR) 预留引脚：当前 BLE 固件不启用
#define PIN_IR_TX           16  // IR_TX
#define PIN_IR_RX           15  // IR_RX


/* =================================================================
 * 6. 系统与调试 (System & Debug)
 * ================================================================= */
// PC 通讯 / 调试串口 (UART0)
#define PIN_DBG_TX          43  // UART_TX -> 连接电脑/USB转串口
#define PIN_DBG_RX          44  // UART_RX

// USB (原生 USB 接口)
#define PIN_USB_DN          19  // USB_D-
#define PIN_USB_DP          20  // USB_D+


/* =================================================================
 * 7. 串口波特率配置 (UART Baud Rate)
 * ================================================================= */
#define BAUD_DEBUG     921600  // 调试串口波特率 (USB-Serial)
#define BAUD_4G        921600  // 4G模块串口波特率 (LE270-EU)

#endif // PIN_CONFIG_H
