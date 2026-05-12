# ESP-IDF 迁移完成记录

固件已经从 Arduino 3.0 迁移到纯 ESP-IDF。当前仓库只保留官方 ESP-IDF 构建和运行路径，旧 Arduino 入口、模块源码、头文件、专属本地库和 PlatformIO 工程入口已经删除；版本回退依赖 Git 历史。

目标硬件按 ESP32-S3 F8N8 处理：8MB Flash，外置 8MB Quad SPI PSRAM。固件修改时必须继续区分内部 SRAM 和外置 PSRAM；主任务栈、Flash 写入缓冲、BLE 关键路径等仍优先按内部 SRAM 约束设计。

## 阶段 1：IDF 启动基线

当前仓库已移除 PlatformIO 工程入口，官方 ESP-IDF 构建通过以下文件组织：

- `CMakeLists.txt`
- `src/CMakeLists.txt`
- `sdkconfig.defaults`
- `src/idf_bootstrap.cpp`

阶段 1 已完成，用于验证：

- ESP-IDF 工程能否构建。
- 8MB Flash、外置 8MB QSPI PSRAM、分区表配置是否正确。
- `model` 分区和 `spiffs` 文件系统分区是否仍在原 offset。
- 启动后内部 SRAM、最大连续块和 PSRAM 余量。

## 阶段 2：默认 ESP-IDF 固件

阶段 2 软件侧已完成，当前已把以下基础设施和主要业务链路纳入 ESP-IDF 默认构建：

- `App_Log.h` 提供项目日志宏；`g_LogLevel` 定义在 `src/idf/App_IdfLog.cpp`。
- `App_FlashGuard` 已去掉 Arduino 头文件依赖，并在 IDF bootstrap 中做互斥锁自检；IDF 侧 Flash 写入窗口会暂停 WakeWord，结束后恢复。
- `App_IdfSystem` 负责 NVS 初始化、app/chip 信息、OTA/model/filesystem 分区日志、内部 SRAM/PSRAM 余量日志和任务栈高水位辅助函数。
- `App_IdfFilesystem` 通过 `joltwallet/littlefs` IDF 组件只读挂载资源分区。分区标签仍沿用 `spiffs`，实际镜像格式继续使用 LittleFS，挂载点为 `/littlefs`。
- `App_IdfTasks` 负责 ESP-IDF 任务创建和 task watchdog 基础封装。新增业务任务应优先使用内部 SRAM 静态栈，避免 Flash/PSRAM cache 关闭期间踩到外部 PSRAM 栈。
- `App_IdfConsole` 负责纯 ESP-IDF 串口命令骨架，当前支持 `HELP`、`STATUS`、`DIAG/FREE`、`FS`、`DISPLAY`、`KEY/ADCKEY`、`SENSORS/BAT/TEMP`、`BLE/BLESCAN/BLEPAIR=n/BLEACON/BLEACOFF/BLECTEMP/BLEMODE/BLEFAN/BLEDISP/BLELIGHT/BLESWING/BLEKEEP/BLEDROP`、`AICMD=<json>`、`AUDIO/AUDIOSTART/SINETEST/AUDIOTEST/AUDIORESET/AUDIOCUE=<name>/AUDIOGEN=<name>/I2CSCAN/VOLUME=/MICGAIN=/MIC=`、`AIREC/AIRECSTART/AIRECSTOP/AIRECCANCEL/AIRECUPLOAD/AIWAKE`、`WW/WWSTART/WWPAUSE/WWRESUME/WWSTOP`、`WIFI/WIFISCAN/WIFICONNECT/WIFICLEAR/WIFIPORTAL/WIFIPORTALSTOP/WIFIDROP`、`NET/NET=AUTO/NET=WIFI/NET=4G/NETCANCEL`、`4G/4GON/4GDIAL/4GHANGUP/4GOFF/4GCSQ`、`MQTT/MQTTPUBSTATUS/MQTTINFO`、`OTA`、`SERVER/SERVERPROBE`、`THEME=LIGHT|DARK`、`PART`、`VER`、`CHIP`、`TASKS`、`LOG=` 和 `RESTART`。它默认读取 `stdin`，并在生成配置仍启用 USB Serial/JTAG secondary console 时额外读取 `/dev/secondary`，避免本机旧 `sdkconfig.esp32-s3-fn8-8mb` 让 USB 命令输入失效。
- `App_IdfAdc` 负责纯 ESP-IDF ADC oneshot 共用层。GPIO1 电池、GPIO2 温度和 GPIO3 按键都在 ADC 上；内部用互斥保护 oneshot driver 和通道校准，避免按键任务与传感器任务并发读 ADC 时互相踩状态。
- `App_IdfSensors` 负责纯 ESP-IDF 电池、温度和充电检测。它复用既有电池 raw 校准表、EMA/显示迟滞、连续 0 读数容错和 NTC 公式，读取 GPIO1/GPIO2 ADC 与 GPIO37 充电检测，并在内部 SRAM 静态栈 `IDF_Sensors` 任务里每秒采样一次；运行中电量从 `>=20%` 降到 `<20%` 时播放 `low_battery` 固件短提示音。
- `App_IdfNetwork` 是纯 ESP-IDF WiFi/network 底座。当前初始化 `esp_netif`、默认 event loop、WiFi STA/AP netif、`esp_http_server` 和轻量 captive DNS 任务，使用原 NVS namespace `wifi_cfg` 的 `ssid/password`，有凭据时启动 STA 连接，没有凭据时启动 `ESP32_xxxx` 开放配网热点。支持国内 DNS 覆盖、RSSI/IP/断开原因/portal 状态缓存、阻塞式 `WIFISCAN`、`WIFICONNECT=<ssid>,<password>`、`WIFICLEAR`、`WIFIPORTAL` 和 `WIFIPORTALSTOP`。HTTP portal 使用 `192.168.4.1:80`，路由为 `/`、`/scan`、`/save`、`/status`；DNS portal 在 UDP 53 对任意 A 查询返回 `192.168.4.1`。
- `App_IdfCellular` 是纯 ESP-IDF 4G PPP 底座，使用 UART2 与 LE270-EU 交互，GPIO45 控制 4G 电源，拨号前缓存 IMEI/CSQ，AT 拨号后通过 lwIP PPPoS 建立 `PPP_4G` 默认路由，并把 IMEI 交给 `App_IdfMqtt` 作为设备身份。
- `App_IdfTransport` 是纯 ESP-IDF active transport 管理器，支持 `AUTO`、`WIFI_ONLY` 和 `CELLULAR_ONLY`。AUTO 下 WiFi 优先，WiFi 丢失约 10 秒后拨 4G PPP，WiFi 稳定恢复约 15 秒后切回 WiFi；录音、OTA 和 Flash guard 活跃时推迟路由切换。
- `App_IdfMqtt` 是纯 ESP-IDF MQTT 层，使用 ESP-IDF `esp-mqtt`，以 4G IMEI 优先、WiFi MAC fallback 的设备身份生成原有 topic，订阅设备指令、广播指令和 OTA 通知，连接后上报 online/device_info，并每 5 分钟由 `IDF_Health` 触发一次 device_info 心跳。普通控制 JSON 会进入 `App_IdfCommandExecutor`，`ota_preflight` 和 OTA 通知交给 `App_IdfOta`。
- `App_IdfOta` 是纯 ESP-IDF OTA 下载层，负责 OTA preflight ACK、正式 OTA metadata 校验、1024B 内部 SRAM HTTP 流式下载、MD5 校验、写入 OTA slot、切换 boot partition、重启，以及新固件启动后的 pending verify 确认/超时回滚。OTA HTTP 下载走当前 active transport 的默认路由；WakeWord 常驻麦克风不再被当作 recording，真正写 Flash 时由 `App_FlashGuard` 暂停 WakeWord。
- `App_IdfServer` 是纯 ESP-IDF 服务器 TCP 协议层，使用 lwIP socket 连接业务服务器，发送原有设备身份包和 META 能力包，并提供 `uploadPcmAndReceive()` 给 `App_IdfRecorder` 上传 16kHz/16-bit/mono PCM。响应仍按十六进制 JSON + `*` 结束符解析，解析后的 JSON 交给 `App_IdfCommandExecutor` 执行；串口 `SERVERPROBE` 仍只验证 TCP 连接和身份/META 写入，不上传录音。
- `App_IdfDisplay` 负责纯 ESP-IDF ST7789P3 panel 链路。当前使用 `esp_lcd` + SPI2 初始化 ST7789P3、写入 VCOM `0xBB=0x1D`、设置 240x240 rotation=2 等价方向，并提供 RGB565 bitmap 绘制接口。
- `App_IdfLvgl` 负责纯 ESP-IDF LVGL shell UI。它通过本地 `components/lvgl` 包装组件复用仓库内置 LVGL 8.3.11，两个 `240 * 40` draw buffer 放外置 PSRAM，flush 时使用 `240 * 16` 内部 DMA bounce buffer，并在 `IDF_LVGL` 内部 SRAM 静态栈任务中调用 `lv_timer_handler()`。
- `App_IdfInput` 负责纯 ESP-IDF ADC 按键输入。它使用 `esp_adc` oneshot driver 读取 GPIO3，保留现有 `Pin_Config.h` 中的 KEY1/KEY2/BOTH 毫伏阈值、25ms 去抖和 800ms 长按判定，并创建内部 SRAM 静态栈 `IDF_Input` 任务。
- `App_IdfBleAircon` 是纯 ESP-IDF NimBLE 空调控制层。当前启用 NimBLE central/observer，读取和写入 NVS namespace `ble_aircon`，支持 BLE 广播扫描、配对页选择、连接并校验 `FFE0/FFE2` 后保存目标地址，成功后保留连接供后续控制帧复用；同时已经接入开关机、温度、模式、风速、显示、灯光和摆风控制帧写入。控制命令复用 `IDF_BLE` 工作任务，如果当前连接无效会自动连接保存目标并重新发现 `FFE0/FFE2`。
- `App_IdfAudio` 是纯 ESP-IDF ES8311/I2S 音频底座。当前使用 `esp_driver_i2c` 探测并配置 ES8311，使用 `esp_driver_i2s` 在 I2S0 上启动 16kHz、16-bit、mono left、RX/TX 全双工，PA 由 GPIO21 控制。它提供音量、麦克风增益/开关、PCM 读写、`SINETEST`、`AUDIOTEST`、`AUDIORESET`、`AUDIOGEN=boot|low_battery|record_stop` 固件短提示音播放，以及 LittleFS `wake_ack/settings_done/done` 本地语音同步播放；服务器语音响应已经收敛为 JSON + `audio_cue`，因此默认固件不引入 1MB 异步播放 ring buffer，而是用同步小块写 I2S 避免占用额外 PSRAM 和播放任务。
- `App_IdfVadNet` 是纯 ESP-IDF VADNet 封装，从 `model` 分区复用 ESP-SR 2.4.3 模型列表，创建 `vadnet1_medium` 检测器，录音交互结束后释放。
- `App_IdfWakeWord` 是纯 ESP-IDF WakeNet 封装，通过本地 `esp_sr_local` 组件链接 ESP-SR 2.4.3 头文件和预编译库，从 `model` 分区加载 `wn9_nihaoxiaoan_tts2`，创建单麦 AFE，只启用 WakeNet，不启用 AEC/SE/旧 VAD。它创建内部 SRAM 静态栈 `WW_Feed` 和 `WW_Fetch`，唤醒后暂停检测并请求 `App_IdfRecorder::startWakeInteraction()`。
- `App_IdfRecorder` 是纯 ESP-IDF 录音/上传层。它启动常驻 `IDF_Recorder` 内部 SRAM 静态栈任务，512KB 录音 buffer 固定分配在外置 PSRAM；`AIRECSTART` 开麦录音，`AIRECSTOP` 停止后播放 `record_stop` 并上传。WakeWord 唤醒路径会先播放 LittleFS `wake_ack`，再用 VADNet 自动断句，结束后播放 `record_stop`、上传服务器响应 JSON 并恢复 WakeWord。OTA preflight 会把录音或上传中的状态视为 `recording`。
- `App_IdfCommandExecutor` 负责纯 ESP-IDF 业务控制协议执行器。它用 ESP-IDF 自带 cJSON 解析服务器最终下发的 `control` 或产品 profile 直出的 `command`，当前支持 `aircon_ble_v1` steps 并调用 `App_IdfBleAircon`，支持 `speaker_v1` 音量设置/步进并调用 `App_IdfAudio`，同时会在服务器 JSON 带 `audio_cue` 时优先执行控制再播放 LittleFS 本地 cue。
- `App_IdfUi` 是轻量 UI bridge。它在 LVGL GUI mutex 内把 IDF 按键事件接到现有 MainScreen / QRScreen / AIScreen：KEY1 短按切换主屏底部焦点，KEY1 长按返回主屏，KEY2 短按确认当前页面动作，KEY2 长按触发 `App_IdfRecorder` 手动录音，松开后停止并上传，上传结束后显示成功或错误；WakeWord 自动唤醒也会切入 AIScreen 的唤醒/聆听/结果状态。当前还会用 LVGL timer 读取 `App_IdfSensors`、`App_IdfNetwork`、`App_IdfTransport`、`App_IdfCellular`、`App_IdfAudio` 和 `App_IdfOta` 缓存，刷新 MainScreen 电池格、充电图标、温度胶囊、WiFi/4G active 状态、音量、BLE 连接状态和 OTA 专用进度/验证/失败状态；设置入口已接入 IDF 动态设置页，当前包含音量、网络、主题和固件项，音量/网络/主题用三行弹窗执行调整。
- MainScreen 的“蓝牙/设备”入口已切到动态 BLE 配对列表，不再进入 QR 占位页。配对页规则：进入后后台扫描最多 10 个可连接广播，6 行窗口只显示名称，KEY1 切换，KEY2 连接并校验，KEY1 长按退出。

当前 IDF 默认构建启动 LVGL UI、ADC 按键输入、传感器采样、WiFi STA/AP Web/DNS portal、4G PPP、active transport、MQTT、OTA 下载层和 OTA 进度 UI、服务器 TCP 协议层、ES8311/I2S 音频底座、手动录音上传层、WakeWord/VADNet 自动语音交互、BLE 配对/控制、业务控制协议执行器和轻量 UI bridge。它用于验证 IDF 启动、分区、LittleFS、显示 panel、LVGL flush/tick、按键扫描、电池/温度/充电检测、WiFi STA/portal、4G PPP、MQTT topic/telemetry/OTA preflight、OTA HTTP 下载、pending verify、OTA 屏幕进度、服务器 TCP 身份/META/录音上传、音频 I2C/I2S/PA/本地语音、WakeNet/VADNet、BLE 扫描/配对/控制写入、`aircon_ble_v1` / `speaker_v1` JSON 执行和静态任务栈。启动时会尝试挂载 LittleFS 并检查 `/audio_cues/manifest.json` 是否存在；挂载失败只记录日志，不格式化资源分区。当前会创建内部 SRAM 静态栈的 `IDF_Health`、`IDF_Console`、`IDF_LVGL`、`IDF_Input`、`IDF_Sensors`、`IDF_OTA`、`IDF_Recorder`、`IDF_4G_RX`、`IDF_Transport`、`WW_Feed` 和 `WW_Fetch`，以及首次 BLE 扫描、配对或控制命令时创建 `IDF_BLE` 工作任务；配网 portal 首次启动时创建 `IDF_DNS` 内部 SRAM 静态栈任务，portal 活跃时还会由 `esp_http_server` 创建 HTTP server 任务。

## 上板验证清单

1. 上板验证 ESP-SR 模型加载、热态重复唤醒、`wake_ack` 播放后恢复、VADNet 自动断句和 `DIAG` 栈水位。
2. 上板验证 IDF 4G PPP、AUTO/WIFI/4G active transport、MQTT/AI/OTA 通过默认路由切换的实际行为。

每次上板验证都要记录：

- `firmware.bin` 大小是否小于 `0x300000` app slot。
- 内部 SRAM free 和最大连续块。
- 关键任务栈高水位。
- 上板串口验证命令和现象。
