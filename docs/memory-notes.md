# 内存现状与注意事项

## 总体判断

硬件有 8MB PSRAM，但内部 SRAM 只有约 500KB 且已经很紧。当前代码多处专门为了回收内部 SRAM 做了收紧和保护，修改固件时必须优先考虑内部 SRAM，而不是只看总 heap。

## 内部 SRAM 高风险点

- 主任务栈通过 `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)` 静态分配。
- `App_IdfTasks` 用于把新任务的静态栈和 TCB 明确分配在内部 SRAM；除非任务确认不触碰 Flash/OTA/NVS/文件系统关键路径，否则不要默认把任务栈放进外部 PSRAM。
- ESP-IDF LVGL 侧 `IDF_LVGL` 使用内部 SRAM 静态任务栈；两个 240x40 draw buffer 放外置 PSRAM，SPI flush 只保留一块 240x16 RGB565 内部 DMA bounce buffer。
- ESP-IDF ADC 按键侧 `IDF_Input` 使用 4096B 内部 SRAM 静态任务栈；当前只做 oneshot ADC 扫描和轻量 UI 事件分发，不要在这个任务里加入音频、网络、BLE 或文件系统长阻塞逻辑。实际硬件启动时 2048B 会触发 `IDF_Input` stack overflow，因此不要再降回 2048B。
- ESP-IDF 串口控制台 `IDF_Console` 使用 6144B 内部 SRAM 静态任务栈；控制台有 512B 命令行 buffer 且会执行 DIAG/FS/WW/AUDIO 等诊断打印，实际硬件启动时 2048B 会触发 `IDF_Console` stack overflow。
- ESP-IDF 健康监控 `IDF_Health` 使用 4096B 内部 SRAM 静态任务栈；它会周期性打印 heap 和多任务高水位，实机 2048B 第二轮周期日志会触发 stack overflow。
- ESP-IDF 传感器侧 `IDF_Sensors` 使用 4096B 内部 SRAM 静态任务栈；当前每秒采样 GPIO1 电池、GPIO2 温度和 GPIO37 充电检测，并把结果缓存给 UI/串口读取。运行中低电量下降沿会在这个任务里同步播放短提示音，播放前会避开 Flash guard 和录音上传。ADC 访问通过 `App_IdfAdc` 互斥保护，避免和 `IDF_Input` 并发配置同一个 ADC unit/channel。实机 DIAG 显示 2048B 只剩几十字节高水位，不能再降回 2048B。
- ESP-IDF WiFi 侧当前 `App_IdfNetwork` 在配网 portal 首次启动时会创建 `IDF_DNS` 内部 SRAM 静态栈任务；启用 `esp_wifi`、`esp_netif`、lwIP 和配网 portal 后还会引入 IDF WiFi/lwIP 系统任务、内部缓冲和 `esp_http_server` 任务，这是内部 SRAM 和 app flash 增量的主要来源。AP portal 启动前会检查内部 free/largest 阈值。
- ESP-IDF active transport 侧 `IDF_Transport` 使用 8192B 内部 SRAM 静态任务栈；这个任务会直接执行 4G 上电、AT 探测、拨号和 PPP 等待。实机 `NET=4G` 首次拨号时 3072B 会触发 `IDF_Transport` stack overflow，不能降回 3072B。
- ESP-IDF MQTT 侧使用 `esp-mqtt` 自带任务，当前配置 8192B task stack、768B in/out buffer 和 1536B outbox 上限；OTA preflight 会在 MQTT 回调路径解析 JSON、生成 ACK 并发布 MQTT 消息，实机 4096B task stack 在服务端批次 preflight 时出现过栈相关崩溃风险，不能再降回 4096B。设备信息心跳复用 `IDF_Health`，不额外创建项目自有 MQTT 任务。
- ESP-IDF OTA 侧新增 `IDF_OTA` 内部 SRAM 静态任务栈 4096 words，HTTP/OTA 写入 buffer 固定 1024B 内部 SRAM，下载时由 `AppFlashGuard` 保护 Flash 写入窗口并暂停 WakeWord；不要在 OTA 下载循环里加入大栈对象或长时间不喂 watchdog 的逻辑。
- ESP-IDF 服务器 TCP 侧 `App_IdfServer` 当前不创建自有任务；`SERVERPROBE` 在 console 任务里执行，录音上传在 `IDF_Recorder` 任务里执行。响应 hex/json buffer 最大 8192/4096B，优先 PSRAM，失败才回落内部 SRAM；socket 写入和响应等待循环会喂当前任务 watchdog。
- ESP-IDF 音频侧当前 `App_IdfAudio` 不创建独立播放任务；ES8311/I2S、PA、同步 LittleFS cue、固件短提示音、`SINETEST` 和 `AUDIORESET` 都在调用方任务中小块执行。`boot` cue 在 bootstrap 中播放，`low_battery` cue 在 `IDF_Sensors` 中播放，`record_stop` cue 在 `IDF_Recorder` 中播放。LittleFS cue 读取使用 512B 局部 I/O buffer，manifest 解析最多读取 4096B 且优先 PSRAM。由于运行时已不再接收服务器云端 TTS PCM，ESP-IDF 默认固件不分配 1MB 异步播放 ring buffer。
- ESP-IDF 录音侧 `App_IdfRecorder` 使用 `IDF_Recorder` 内部 SRAM 静态任务栈 5120 words（20KB），512KB 录音线性 buffer 固定分配在外置 PSRAM，录音 I/O scratch buffer 1024B 优先 PSRAM、失败才回落内部 SRAM。录音最长约 16 秒，达到 512KB 后自动停止并上传。语音 → 服务器响应 → BLE 空调控制的整条链（`uploadPcmAndReceive` → `cJSON_Parse` → `AppIdfCommandExecutor::executeControlJson` → `AppIdfBleAircon::sendFrame`）都在 `IDF_Recorder` 栈上同步执行（GATT write 转到 `IDF_BLE` worker，不在录音栈上跑），其中 cJSON 递归解析 + executor 调用栈是峰值；2026-05-05 实机 4096 words 时 BLE 控制场景触发 `IDF_Recorder` stack overflow，因此不要再降回 4096 words 以下。
- `IDF_Recorder` 空闲等待录音通知时不能用无限阻塞；任务注册了 watchdog，空闲态也要周期性超时唤醒并喂狗，否则实机约 60 秒会触发 `IDF_Recorder` task WDT。
- ESP-IDF BLE 侧当前启用 NimBLE central/observer。`App_IdfBleAircon` 进入配对扫描、配对连接或控制帧写入时才创建 `IDF_BLE` 工作任务，使用 3072 words 内部 SRAM 静态栈；NimBLE host task 栈按 `sdkconfig.defaults` 保持 4096。BLE 扫描、连接和控制帧写入前会记录 internal free、largest block 和 PSRAM 余量，低于告警阈值时只告警并继续尝试。
- 2026-05-04 实机 BLE 验证中，NimBLE 初始化并连接 `48:87:2d:c2:61:60` 后 internal free 约 25KB、largest block 约 17KB，低于 BLE 告警阈值但配对、`FFE0/FFE2` discovery 和 `BLECTEMP 24` 写入仍成功；`IDF_BLE` 任务高水位约 668B。后续不要把 `IDF_BLE` 栈降到 3072 words 以下。
- ESP-IDF WakeWord 使用 `WW_Feed` 3072 words 和 `WW_Fetch` 4096 words 内部 SRAM 静态栈；AFE 优先走 PSRAM。
- OTA 写入缓冲是 1024B 内部 SRAM，因为写 Flash 时 cache/PSRAM 安全性要谨慎。
- BLE 初始化和连接需要内部 SRAM 连续块，内部最大可用块比总量更重要。
- BLE 分支复用一个 `IDF_BLE` 工作任务接收扫描、配对和控制命令，避免反复分配任务栈。
- IR、433 和本地场景模块已恢复（详见 `docs/ir-433-scene-status.md`），运行期通过 `App_IdfAppMode` 三选一启动，不会与 BLE 同时占用内部 SRAM。`IDF_IR` 任务 4096 words（16KB）内部 SRAM 静态栈，运行时高水位约 2KB；`IDF_RF433` 同样 4096 words，运行时高水位约 2KB。`App_IdfScene` 没有独立任务，全局表 `g_scenes[20]` 占 BSS 约 2.2KB，cJSON 临时树走 PSRAM。互斥即省内存：BLE 关停后释放的 controller HCI/DMA + IDF_BLE 栈刚好够 IR/RF433 启动用。
- 旧 4G AT/TCP 和 Arduino PPP 路径已从仓库移除，当前 4G 默认构建走 `App_IdfCellular` + `App_IdfTransport`。
- WiFi 配网门户启动前会检查内部 SRAM 余量；ESP-IDF portal 的 HTTP server stack 当前按 8192B 配置，只在 portal 活跃时启动；`/scan` 的扫描结果和 JSON 响应放在堆上，避免手机 captive portal 自动请求时压垮 httpd 栈。`IDF_DNS` 使用 4096B 内部 SRAM 静态栈，首次启动 portal 后常驻，portal 关闭时关闭 UDP socket 并休眠。

## 当前任务栈

当前没有 Arduino `Audio/Net/UI/Sys` 主任务。ESP-IDF 固件主要任务栈和水位看 `App_IdfTasks`、`App_IdfSystem` 的 DIAG 输出，重点关注 `IDF_Console`、`IDF_Health`、`IDF_LVGL`、`IDF_Input`、`IDF_Sensors`、`IDF_OTA`、`IDF_DNS`、`IDF_Recorder`、`IDF_4G_RX`、`IDF_Transport`、`WW_Feed`、`WW_Fetch` 和按需创建的 `IDF_BLE`。

使用串口 `DIAG` 或 `FREE` 查看内部 SRAM、最大连续块、PSRAM 和任务高水位。

## 主要 PSRAM 分配

- LVGL 双全屏 buffer：两个 240x240 `lv_color_t` buffer，优先 PSRAM。
- ESP-IDF LVGL UI：两个 240x240 全帧 `lv_color_t` draw buffer，固定分配在外置 PSRAM（每帧 1 次 flush，真正双缓冲，渲染与 SPI 并行）；LVGL heap 也通过 `lv_conf.h` 的非 Arduino 分支优先走 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`。
- ESP-IDF NimBLE host 已切到 `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`，host 的 ~15-20KB 分配走外部 PSRAM；BT controller 的 HCI/DMA 队列必须留在内部 SRAM(DMA 限制，不可走 PSRAM)，所以这条切换主要是把内部 SRAM 让给 controller。实测前后 `before NimBLE init` 内部 free 从 ~48KB 抬到 ~67KB、largest 从 ~24KB 抬到 ~31KB，原本会触发 `BLE_INIT: hci inits failed` 的低内存场景被消除。NimBLE host 调用走 PSRAM 略慢但 BLE 控制频率不高，无感知。
- ESP-IDF 音频底座当前只使用 I2S DMA 小缓冲、512B LittleFS cue I/O buffer、最多 4096B manifest 解析缓冲、固件短提示音 flash 常量和 512KB PSRAM 录音线性 buffer；未分配 1MB 播放 ring buffer 或 600KB 录音 ring buffer。
- ESP-IDF 录音上传层固定分配 512KB PSRAM 录音 buffer；如果 PSRAM 分配失败，`App_IdfRecorder::start()` 会失败，不会把 512KB buffer 回落到内部 SRAM。
- 录音 buffer：512KB PSRAM 线性 buffer。
- WakeWord AFE：优先 `AFE_MEMORY_ALLOC_MORE_PSRAM`。
- VADNet：录音交互期间临时创建，模型从 `model` 分区读取；帧缓冲按 VADNet chunksize 分配，优先 PSRAM，结束录音后释放。
- 服务器 scratch buffer：按需分配，默认 4096B，优先 PSRAM。

## 现有保护阈值

- WiFi 配网门户启动：内部 free 至少约 20KB，最大连续块至少约 8KB。
- WiFi 重试探测：内部 free 至少约 12KB，最大连续块至少约 6KB。
- BLE 配对扫描 / 配对连接 / 控制帧写入：进入路径前 `logBleMemory` 守门，内部 free 少于 **12KB** 或最大连续块少于 **6KB** 直接 abort 返回 `ESP_ERR_NO_MEM`，scan 和 control 共用同一对常量（`kMinBleInternalFree` / `kMinBleInternalLargest`，`App_IdfBleAircon.cpp`）。历史阈值变迁：32K/20K → 22K/14K → 14K/7K → 12K/6K（2026-05-09）。
- NimBLE 协议栈一旦 `initStack`（`nimble_port_init` + BT controller HCI/DMA pool）即在内部 SRAM 常驻 ~600B；`cancelPairing` / `ble_gap_terminate` / 断连只归还 GAP connection 槽位，不会让 internal_free 回退。当前没有实现 `nimble_port_deinit` / `esp_bt_controller_disable` 的拆栈路径，因此从 boot 第一次进入 BLE 扫描后，internal_free 会一次性压到稳态水位（实测约 13759B）并保持，所以阈值只能贴着这个水位往下设。
- BLE 命令默认保持当前 NimBLE client 长连接；如果连续块不足或 WakeWord/RMS 受影响，可用 `BLEKEEP=0` 切换为命令后自动断开，或用 `BLEDROP` 手动释放连接。注意 disconnect 同样不会释放协议栈常驻部分。
- WakeWord 启动：会检查内部 free 和最大连续块，余量不足则拒绝启动。

## 修改规则

- 不要新增大的局部数组，尤其是在 UI 回调、串口命令、BLE/WakeWord 路径里。
- 避免大 `String` 拼接和过大的 `JsonDocument`；需要大 buffer 时优先 PSRAM，并处理分配失败。
- 新增任务前先估算内部 SRAM 和栈高水位。
- 需要写 Flash、OTA、NVS、文件系统时，注意 `AppFlashGuard` 和 WakeWord 暂停/停止逻辑。
- 做完固件改动至少运行 `idf.py build`，上板后优先看 `DIAG`。
