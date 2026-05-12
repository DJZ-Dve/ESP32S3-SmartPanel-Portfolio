# 固件架构现状

## ESP-IDF 固件

当前 IDF 入口是 `src/idf_bootstrap.cpp` 的 `app_main()`，负责 NVS、Flash guard、LittleFS、WiFi STA/AP Web/DNS 配网 portal、4G PPP、active transport、MQTT、OTA、服务器 TCP 协议、ES8311/I2S 音频底座、手动录音上传、WakeWord/VADNet 自动语音交互、LVGL UI、ADC 按键输入、传感器采样、BLE/IR/RF433 三选一外设（按 `App_IdfAppMode` 当前模式条件启动）、本地场景管理、业务控制执行器、健康日志任务和串口命令骨架。

IDF 基础层包括：

- `App_Log.h`：项目日志宏。
- `App_FlashGuard`：Flash 写入互斥保护，已可在纯 IDF 下编译；IDF 侧进入 Flash 写窗口前会暂停 WakeWord，退出后恢复。
- `App_IdfSystem`：app/chip/分区/heap/任务栈诊断辅助。
- `App_IdfFilesystem`：可写挂载 LittleFS 资源分区，挂载点 `/littlefs`，供 IDF 本地固定语音 cue 读取以及 IR 学习（`/littlefs/ir_codes.json`）和场景表（`/littlefs/scenes.json`）持久化。资源类静态文件（`audio_cues/*` 等）在运行期不主动改写。
- `App_IdfTasks`：内部 SRAM 静态任务创建和 task watchdog 辅助。当前 bootstrap 用它创建 `IDF_Health` 任务，定期输出 heap 和栈水位，并在 MQTT 连接时每 5 分钟触发一次 device_info 上报。
- `App_IdfConsole`：ESP-IDF 串口命令骨架。当前提供基础状态、内存、分区、文件系统、显示、按键、电池/温度/充电检测、WiFi STA/scan/配网 portal、4G PPP、active transport、MQTT/OTA 状态、服务器 TCP probe、手动/唤醒录音上传、WakeWord、音频 I2C/I2S/PA/本地 cue 验证、BLE 扫描/配对/控制帧/连接保持/释放连接、`AICMD=<json>` 业务控制协议验证、主题切换、日志级别和重启命令。输入优先走 IDF `stdin`，生成配置保留 USB Serial/JTAG secondary console 时也会读取 `/dev/secondary`。
- `App_IdfAdc`：ESP-IDF ADC oneshot 共用层，负责 GPIO 到 ADC unit/channel 映射、通道配置、校准和 raw/mV 读取，并用互斥保护按键和传感器任务的并发 ADC 访问。
- `App_IdfSensors`：ESP-IDF 传感器层，负责 GPIO1 电池、GPIO2 温度和 GPIO37 充电检测，每秒在 `IDF_Sensors` 内部 SRAM 静态栈任务中采样一次并缓存快照；同时跑 `PowerState` 低电量状态机（NORMAL → WARN_25 → COUNTDOWN_15 → EMERGENCY_8），分别触发 `low_battery` cue、30 秒倒计时弹窗、立即 deep sleep。阈值定义在 `include/Battery_Config.h`，详见 `docs/power-save.md` 「低电量自动关机」节。
- `App_IdfNetwork`：ESP-IDF WiFi/network 底座，负责 `esp_netif`、默认 event loop、WiFi STA/AP netif、原 `wifi_cfg` NVS 凭据读取/保存/清除、STA 连接、国内 DNS 覆盖、WiFi scan、`esp_http_server` 配网页、轻量 UDP captive DNS 和状态缓存。无已保存凭据时会启动开放热点 `ESP32_xxxx`，AP IP 为 `192.168.4.1`，HTTP 路由为 `/`、`/scan`、`/save`、`/status`；DNS 在 UDP 53 对 A 查询返回 `192.168.4.1`；串口可用 `WIFIPORTAL`/`WIFIPORTALSTOP` 手动开关。
- `App_IdfCellular`：ESP-IDF 4G PPP 底座，使用 UART2 驱动 LE270-EU，GPIO45 控制 4G 电源，缓存 IMEI/CSQ，AT 拨号后通过 lwIP PPPoS 建立 `PPP_4G`。
- `App_IdfTime`：ESP-IDF SNTP 时间同步层。`init()` 在 bootstrap 设置 `TZ=CST-8` 并配置 NTP 服务器（`ntp.aliyun.com` 等），不立即启动 SNTP；WiFi 拿 IP 或 4G PPP 拨号成功时各自调用 `onNetworkUp()` 启动或 restart SNTP。MainScreen 1Hz 状态 timer 通过 `isSynced()` 决定 `ui_ClockLabel` / `ui_DateLabel` 显示真实时间还是 `--:--` / `--/--` 占位。
- `App_IdfTransport`：ESP-IDF active transport 管理器，支持 `AUTO`、`WIFI_ONLY` 和 `CELLULAR_ONLY`，WiFi 优先、4G 兜底，并在录音、OTA 或 Flash guard 活跃时延后路由切换。
- `App_IdfMqtt`：ESP-IDF MQTT 迁移层，负责 IMEI 优先/MAC fallback 设备身份、原有 topic 生成、设备/广播指令订阅、online/device_info 上报、5 分钟心跳入口、OTA preflight/notify 转发，以及普通控制 JSON 到 `App_IdfCommandExecutor`。
- `App_IdfOta`：ESP-IDF OTA 下载层，负责正式 OTA metadata 校验、preflight ACK、1024B 内部 SRAM HTTP 流式下载、MD5 校验、OTA slot 写入、boot partition 切换、重启和 pending verify 后的 MQTT 健康确认/超时回滚；HTTP 下载走当前 active transport 的默认路由。
- `App_IdfServer`：ESP-IDF 服务器 TCP 协议层，负责 lwIP socket 连接业务服务器、发送设备身份包和 META 能力包、上传录音 PCM、按十六进制 JSON + `*` 结束符解析响应，并把响应 JSON 交给 `App_IdfCommandExecutor`。当前可用 `SERVERPROBE` 验证连接和身份包。
- `App_IdfAudio`：ESP-IDF ES8311/I2S 音频底座，负责 I2C 探测/寄存器配置、I2S0 RX/TX 16kHz 16-bit mono left、PA 控制、音量/麦克风增益、PCM 读写、`SINETEST`、`AUDIORESET`、固件短提示音 `boot/low_battery/record_stop` 和 LittleFS `wake_ack/settings_done/done` 本地固定语音同步播放。
- `App_IdfWakeWord`：ESP-IDF WakeNet 封装，从 `model` 分区加载 `wn9_nihaoxiaoan_tts2`，创建单麦 AFE，只启用 WakeNet，并用 `WW_Feed` / `WW_Fetch` 两个内部 SRAM 静态栈任务常驻检测。
- `App_IdfVadNet`：ESP-IDF VADNet 封装，从 `model` 分区加载 `vadnet1_medium`，Wake/VAD 录音交互期间临时创建检测器，结束后释放。
- `App_IdfRecorder`：ESP-IDF 录音上传层，负责 512KB PSRAM 录音 buffer、`IDF_Recorder` 任务、`AIRECSTART/AIRECSTOP/AIRECCANCEL/AIRECUPLOAD`、MainScreen KEY2 长按录音，以及停止后播放 `record_stop` 并经 `App_IdfServer` 上传 PCM；WakeWord 唤醒后会播放 `wake_ack`，再用 VADNet 自动断句并上传。
- `App_IdfBleAircon`：ESP-IDF NimBLE BLE 空调控制层，当前负责读取/保存 `ble_aircon` NVS 目标、BLE 广播扫描、配对页选择、连接并校验 `FFE0/FFE2`，成功后保留连接供控制帧写入复用；同时提供开关机、温度、模式、风速、显示、灯光和摆风 API。控制命令在 `IDF_BLE` 工作任务里执行，使用 16 字节以内空调控制帧，必要时会自动连接保存目标并重新发现 characteristic。仅在 `AppIdfAppMode::isBle()` 时启动。
- `App_IdfAppMode`：BLE/IR/RF433 运行期互斥状态机。NVS namespace=`appmode` / key=`mode` u8（0=BLE / 1=IR / 2=RF433），默认 0 BLE。`init()` 在 `app_main` 早期加载缓存当前模式；`bootstrap` 据此条件化启动对应外设。切换走 `switchAndRestart`（写 NVS → 短延迟 → `esp_restart`）软重启，避免 NimBLE deinit 在 ESP-IDF 5.x 上的"second start 偶发 hci init failed"坑。
- `App_IdfIr`：ESP-IDF RMT 红外学习/回放层，仅在 IR 模式下启动。`IDF_IR` 任务消费 RX done 事件，二次按键确认 + djb2 指纹去重，存储 `/littlefs/ir_codes.json`（cJSON）。发送前后用 `rmt_disable/rmt_enable(rxChannel)` 包裹 transmit，避免 IDF 5.x RMT TX 后 RX 进 not-enable 态。RX raw buffer + 首次捕获 buffer 各 2KB 在 PSRAM。
- `App_IdfRf433`：CMT2300A 433MHz bit-banging SPI 学习/回放层，仅在 RF433 模式下启动。`IDF_RF433` 任务消费 GPIO ISR 帧队列，4 模式 IDLE/LEARN_CLOUD/LISTEN_NORMAL/SNIFF_RAW；学习走候选+聚类投票（最多 8 候选，T 容差 12%、bitLen 差 ≤12，评分 = 成员*1000+bits*10+ratio-dispersion+sync*20）。发射 Direct TX 模式（GPIO3 输出 sync HIGH T+LOW 31T，bit=1 HIGH 3T+LOW T，bit=0 HIGH T+LOW 3T，重复 8 次），自动检测重复帧/零填充半帧并补发归一化版本。学习成功推送 `LearnEvent` 到 `learnEventQueue`。
- `App_IdfScene`：本地场景统一表，IR 或 RF433 模式下都启动（BLE 模式下不启动）。无独立 task，纯数据层 + cJSON 持久化到 `/littlefs/scenes.json`，最多 20 条；JSON 字段（id/type/desc/ir_name/code_high/code_low/len/T）与老 Arduino 仓库格式兼容。`executeById` 按 type 调 `AppIdfIr::sendLearned` 或 `AppIdfRf433::sendCode`，type 不匹配当前模式时返回 `ESP_ERR_INVALID_STATE` 并打印「is X but app mode is Y」提示。
- `App_IdfCommandExecutor`：ESP-IDF 业务控制协议执行器，用 cJSON 解析服务器响应中的 `control` 或产品 profile 直出的 `command`。当前 `aircon_ble_v1` steps 会直接调用 `App_IdfBleAircon`，`speaker_v1` 音量 steps 会调用 `App_IdfAudio`，服务器 JSON 带 `audio_cue` 时会在控制命令后播放 LittleFS 本地 cue；可通过串口 `AICMD=<json>` 验证。
- `App_IdfDisplay`：ESP-IDF ST7789P3 panel 链路，负责 SPI panel 初始化、VCOM、方向/offset、背光和 RGB565 bitmap 绘制。
- `App_IdfLvgl`：ESP-IDF LVGL UI 底座，负责 PSRAM draw buffer、内部 DMA bounce buffer、flush/tick 和 `IDF_LVGL` 任务，并调用现有 `ui_init()`。
- `App_IdfInput`：ESP-IDF ADC 按键输入，基于 `App_IdfAdc` 读取 GPIO3，负责 KEY1/KEY2/BOTH 阈值判断、去抖、长按判定和 `IDF_Input` 任务。
- `App_IdfUi`：ESP-IDF UI bridge，负责把 `App_IdfInput` 事件接到现有 LVGL UI，提供主屏焦点切换、BLE 配对页、QR/AIScreen 进入、KEY2 长按 AI 录音上传、音量/网络/主题/固件设置页、OTA 状态页和主题偏好 NVS，并把 `App_IdfSensors`、`App_IdfNetwork`、`App_IdfTransport`、`App_IdfCellular`、`App_IdfAudio`、`App_IdfBleAircon` 和 `App_IdfOta` 的缓存状态刷新到 MainScreen/OTA 页面；WakeWord 自动唤醒也会更新 AIScreen 状态。

## 任务与事件

当前项目不再保留 Arduino `Audio/Net/UI/Sys` 主任务和对应队列。ESP-IDF 侧任务由各模块按需创建，优先使用内部 SRAM 静态栈：

- 常驻任务：`IDF_Console`、`IDF_Health`、`IDF_LVGL`、`IDF_Input`、`IDF_Sensors`、`IDF_Transport`、`IDF_OTA`、`IDF_Recorder`、`WW_Feed`、`WW_Fetch`。
- 按需任务：`IDF_DNS` 在配网 portal 首次启动时创建；`IDF_BLE` 在 BLE 模式下首次 BLE 扫描、配对或控制命令时创建；`IDF_IR` 在 IR 模式下随 `AppIdfIr::start()` 创建；`IDF_RF433` 在 RF433 模式下随 `AppIdfRf433::start()` 创建；`IDF_4G_RX` 在 4G PPP 链路运行期间工作。BLE/IR/RF433 任务按当前 `AppIdfAppMode` 互斥存在，同一时刻最多只有其中一个任务活跃。
- 事件入口：串口命令走 `App_IdfConsole`，按键事件走 `App_IdfInput` 到 `App_IdfUi`，MQTT OTA/控制消息走 `App_IdfMqtt`，AI 录音上传和 Wake/VAD 交互走 `App_IdfRecorder`。
- 省电状态机：`App_IdfPowerSave`（不开 task，挂在 1Hz `esp_timer`）维护 L0/L1，按键和唤醒词钩子触发退出，60s 无活动且抑制条件全过则进入 L1（关背光 + WiFi PS_MIN_MODEM + LVGL 降频），细节见 `docs/power-save.md`。

## 按键响应

ADC 按键由 `App_IdfInput` 扫描，复用 `Pin_Config.h` 的 GPIO3 和毫伏阈值。当前 `App_IdfUi` 把 KEY1/KEY2 映射到 UI：KEY1 短按切换主屏焦点、设置项或 BLE 配对列表项，KEY1 长按返回主屏，KEY2 短按确认页面动作，KEY2 长按开始 AI 录音，松开后停止并上传。

设置页当前由 `App_IdfUi` 动态创建，包含「音量 / 网络 / 重置WiFi / 主题 / 模式 / 场景 / 固件」7 项。进入设置页后 `KEY1` 切换设置项，`KEY2` 打开对应三行弹窗或跳屏；音量弹窗支持减小 10%、完成、增大 10%，网络弹窗支持 Auto/WiFi/4G，主题弹窗支持亮色/暗色并保存到 NVS。**模式弹窗**支持蓝牙空调/红外/射频433 三选一，确认后调 `AppIdfAppMode::switchAndRestart`（写 NVS + 800ms 后 `esp_restart`）；**场景**项跳转到独立场景列表屏，列出当前 `/littlefs/scenes.json` 中所有条目（最多 20），`KEY1` 切焦点、`KEY2` 调 `AppIdfScene::executeById` 执行（互斥校验失败时只打印日志，UI 上无视觉反馈），`KEY1` 长按返回主屏。固件项当前只刷新版本摘要。

BLE 配对页当前由 `App_IdfUi` 动态创建。进入 MainScreen 的“蓝牙/设备”入口后，`App_IdfBleAircon` 在后台任务中启动 NimBLE 扫描，最多缓存 10 个可连接广播并按 `FFE0` 广告和 RSSI 排序；页面显示 6 行滚动窗口，`KEY1` 切换列表项，`KEY2` 连接选中设备并校验 `FFE0/FFE2`，成功后保存 `ble_aircon/target_mac` 和 `ble_aircon/addr_type`。

## 传感器状态

`App_IdfSensors` 复用 GPIO1/GPIO2/GPIO37 和既有电池 raw 校准/NTC 公式，当前负责状态栏、串口 `SENSORS/BAT/TEMP`、MQTT device_info、OTA preflight 电量 gate、低电量状态机（25% cue / 15% 30s 倒计时 / 8% 紧急 deep sleep），以及为状态机服务的串口调试覆写 `BAT-INJECT=<V>` / `BAT-CHARGING=<0|1|-1>`。

按键、焦点切换、设置确认、BLE 配对和 OTA 流程不再播放本地提示音。MainScreen 上 KEY2 长按 AI 录音时会进入 AI 界面、暂停 WakeWord 并直接开麦录音，不再播放录音开始提示音；松手停止录音后播放 `RecordStop`，排空约 100ms 静音尾部并确认 PA 关闭后再上传录音。WakeWord 唤醒路径会先播放 LittleFS `wake_ack`，播完后额外等待约 100ms，然后直接进入录音。

串口调试命令 `AIRECSTART` / `AIRECSTOP` 复用同一套手动 AI 录音开始/结束逻辑，用于不按实体 KEY2 时验证录音、`RecordStop` 提示音、服务器上传和响应 JSON 执行链路；`AIRECCANCEL` 停止不上传，`AIRECUPLOAD` 重新上传上一段 PCM。

## 系统提示音

本地提示音只保留 `Boot`、`LowBattery`、`RecordStop`。低电量 cue 只在状态机首次进入 `WARN_25`（电压 ≤ 3.30V，≈25%）时播放一次，电压回升过 3.36V 后才重新解锁。ESP-IDF 固件播放这三段固件短提示音：bootstrap 播放 `Boot`，`IDF_Sensors` 状态机进入 WARN_25 时触发 `LowBattery`，`App_IdfRecorder` 手动和唤醒录音结束触发 `RecordStop`。

## 网络模式

网络模式实现在 `include/App_IdfTransport.h`：

- `NET_MODE_AUTO`：默认，WiFi 优先；WiFi 不可用持续约 10 秒后按需拨 4G PPP 兜底，WiFi 恢复稳定约 15 秒后切回 WiFi。AUTO fallback 中 PPP 已就绪但 MQTT 约 75 秒仍未连上时，会停止本轮 fallback 并保持主屏可用，直到 WiFi 恢复后解除抑制。
- `NET_MODE_WIFI_ONLY`：只使用 WiFi，并断开 PPP。
- `NET_MODE_4G_ONLY`：强制 4G PPP-only 业务模式，WiFi 会关闭；如果 PPP 约 120 秒仍未就绪，或 PPP 就绪后 MQTT 约 75 秒仍未连上，会自动切回 `NET_MODE_AUTO`。4G/MQTT 连接弹窗期间长按 `KEY1` 也会主动取消当前尝试并返回主屏。

`App_IdfTransport` 维护一个 active transport：

- `NONE`：无业务网络。
- `WIFI`：业务默认路由为 WiFi。
- `PPP_4G`：业务默认路由为 4G PPP。

MQTT、AI 和 OTA 都只使用当前 active transport。切换 active transport 时先断开 MQTT，再设置默认路由并重连 MQTT。AI 会话、整体录音上传、OTA 下载或 Flash guard 活跃时不会立即切换路由，切换会推迟到空闲后执行。

串口命令 `NET=AUTO`、`NET=WIFI`、`NET=4G` 可切换模式。

## OTA 流程

OTA 由服务端开放批次调度触发。正式通知必须携带 `request_id`，且必须先通过 `ota_preflight`；没有 `request_id` 的旧 OTA 通知会被拒绝。调度通知 `force=true` 时自动设置 `g_OtaInfo.requested`，随后由 `performOTA()` 通过 HTTP 下载。

下载前会二次检查版本、充电/电量、录音、播放和 OTA 分区大小。下载使用 1024B 内部 SRAM 缓冲，流式写入 OTA 分区并增量计算 MD5。升级后通过 pending verify 机制确认健康状态，超时可回滚。服务端最终以新版本重新上线的 `fw_version` 作为 OTA job 的 `verified` 条件。

`App_IdfOta` 通过 active transport 上的 MQTT 接收同一套 `ota_preflight` 和正式 OTA JSON。正式 OTA 必须匹配最近一次通过的 `request_id`；下载路径使用 `esp_http_client` + `esp_ota_ops`，1024B 内部 SRAM buffer，校验 `Content-Length`、最终写入字节数和 32 位 MD5，成功后切换 boot partition 并重启。新固件如果处于 `ESP_OTA_IMG_PENDING_VERIFY`，会在 MQTT 成功连接后调用 `esp_ota_mark_app_valid_cancel_rollback()`，120 秒内未确认且存在回滚镜像时会回滚重启。录音/上传中会拒绝 preflight；WakeWord 常驻麦克风不再被视为 recording，真正写 Flash 时由 `App_FlashGuard` 暂停 WakeWord。

## 修改注意事项

- 不要在主任务循环里加入长阻塞逻辑。
- UI 操作需要通过 `App_IdfLvgl::runLocked()` 或同一 GUI mutex。
- I2S 被音频播放、录音、WakeWord 共享，改动时要考虑互斥和采样率恢复。
- 串口命令处理在 `App_IdfConsole` 中执行，长耗时命令要持续喂 watchdog 并避免占用网络或 UI 临界区。
- 改 BLE/IR/RF433 任一模块时注意：`AppIdfAppMode::init()` 已经在 `app_main` 早期调用，三个模块由 `bootstrap` 按当前模式条件化启动。模式切换走 **软重启**（NVS + `esp_restart`），不要尝试热卸载 NimBLE 或在不重启的情况下切换外设——NimBLE deinit 在 ESP-IDF 5.x 上不稳，会触发"hci init failed"已知坑。
- 如果改 sdkconfig 的 `ESP_CONSOLE_*` 选项（例如从 USB Serial/JTAG 改回 UART0）：必须同步改 `App_IdfConsole.cpp::configureMainConsoleInput`，加对应驱动安装 + `vfs_use_driver` 调用，否则 `stdin` 没驱动 → 串口只能收不能发。
