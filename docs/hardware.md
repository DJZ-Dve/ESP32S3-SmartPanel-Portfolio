# 硬件现状

## 当前现状

目标硬件是 ESP32-S3FN8，内置 8MB Flash，外挂 8MB PSRAM。内部 SRAM 很小且已经紧张，固件修改时不要随意新增大静态数组、大栈对象或大 JSON 缓冲。

## 主要外设

- 屏幕：ST7789P3，240x240，1.54 英寸，4-wire SPI。
- 音频：ES8311 编解码器 + NS4150B 功放，I2S + I2C。
- 蜂窝网络：LE270-EU Cat.1 4G 模块，UART2。
- BLE：当前构建用于控制 BLE 空调。
- 433MHz：`Pin_Config.h` 仅保留 CMT2300A 预留引脚资料，当前 BLE 固件不启用 433 功能代码。
- 红外：`Pin_Config.h` 仅保留 IR TX/RX 预留引脚资料，当前 BLE 固件不启用 IR 功能代码。

## 关键引脚

- LCD：SCLK GPIO14，MOSI GPIO7，CS GPIO10，DC GPIO11，RST GPIO17，BL GPIO18。
- ES8311 I2C：SDA GPIO47，SCL GPIO48。
- ES8311 I2S：MCLK GPIO42，BCLK GPIO41，LRCK GPIO39，DOUT GPIO38，DIN GPIO40。
- 功放使能：GPIO21。
- 4G：PWR GPIO45，PWRKEY GPIO46，RX GPIO12，TX GPIO13，波特率 921600。
- 433：FCSB GPIO4，CSB GPIO5，SDIO GPIO6，SCLK GPIO8，GPIO3 GPIO9。
- ADC 按键：GPIO3。`App_IdfInput` 通过 `App_IdfAdc` / `esp_adc` oneshot driver 扫描，并复用 `Pin_Config.h` 中 KEY1/KEY2/BOTH 的毫伏阈值。
- 电池检测：GPIO1。`App_IdfSensors` 通过 `App_IdfAdc` 采样，并复用既有 raw 校准表、EMA 和显示迟滞。
- 充电检测：GPIO37。`App_IdfSensors` 配置为 pulldown 输入并做 7 次稳定采样。
- 温度检测：GPIO2。`App_IdfSensors` 通过 `App_IdfAdc` 采样，并复用既有 NTC 公式。
- IR：TX GPIO16，RX GPIO15。
- 原生 USB：DN GPIO19，DP GPIO20。
- 调试串口：TX GPIO43，RX GPIO44。

## 重要约束

- GPIO8 已不再作为 LCD_CLK，LCD_CLK 已迁移到 GPIO14；GPIO8 当前留给 433 SCLK 定义。
- `Pin_Config.h` 和 `src/idf/App_IdfDisplay.cpp` 的 LCD 引脚必须保持一致。
- ESP32-S3 内部 SRAM 紧张，涉及 BLE、WakeWord、WiFi 配网、OTA、TLS/网络时都要先考虑内存余量。
- 不要把 PSRAM 当成所有场景都安全的栈内存；当前主任务栈刻意放在内部 SRAM。

## 关键文件

- `include/Pin_Config.h`
- `src/idf/App_IdfDisplay.cpp`
- `src/idf/App_IdfAudio.cpp`
- `src/idf/App_IdfCellular.cpp`
- `src/idf/App_IdfBleAircon.cpp`
- `src/idf/App_IdfSensors.cpp`
