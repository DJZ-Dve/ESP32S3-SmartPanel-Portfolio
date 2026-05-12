# 工具与调试现状

## 构建与烧录

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
```

官方 ESP-IDF CMake 会在 `board_models/srmodels.bin` 存在时把模型分区加入 `idf.py flash`：

```text
0x610000 = board_models/srmodels.bin
```

`idf.py build` 还会从 `data/` 生成 `build/spiffs.bin`，并把 LittleFS 资源镜像加入 `idf.py flash`。烧录前要确认模型文件存在；如果只想单独烧录资源或模型，可用 `idf.py -p PORT spiffs-flash` 或 `idf.py -p PORT model-flash`。

## 串口

默认波特率：921600。

优先使用仓库脚本：

```powershell
python tools/serial_monitor.py --list-ports
python tools/serial_monitor.py --port COMx
python tools/serial_monitor.py --port COMx --send DIAG --send-delay 1 --duration 5
```

`--send` 可以重复使用；多条命令会从 `--send-delay` 开始按 0.5 秒间隔依次发送，例如：

```powershell
python tools/serial_monitor.py --port COMx --send LOG=2 --send NET=4G --send-delay 12
```

需要在同一个串口会话里按指定时间点发命令时，用 `--send-at 秒:命令`，例如：

```powershell
python tools/serial_monitor.py --port COMx --send-at 12:LOG=2 --send-at 25:BLEACON --send-at 55:BLEACOFF
```

需要粗略测量串口事件间隔时，加 `--timestamp` 给每行日志加本机接收时间戳。

注意同一时间只能有一个程序占用串口。`idf.py monitor`、串口工具和 Python 脚本不要同时开。

## 常用串口命令

当前启用基础命令、WiFi/4G/active transport/MQTT/OTA、服务器 TCP probe、手动/唤醒录音上传、WakeWord、音频、BLE 配对、BLE 空调控制帧命令、业务控制 JSON 执行命令、应用模式互斥切换、IR 学习/回放、RF433 学习/回放、本地场景管理：`HELP`、`STATUS`、`DIAG/FREE`、`FS/LITTLEFS`、`DISPLAY/LCD`、`KEY/ADCKEY`、`SENSORS/BAT/TEMP`、`WIFI/WIFISCAN/WIFICONNECT/WIFICLEAR/WIFIPORTAL/WIFIPORTALSTOP/WIFIDROP`、`NET/NET=AUTO/NET=WIFI/NET=4G/NETCANCEL`、`4G/4GON/4GDIAL/4GHANGUP/4GOFF/4GCSQ`、`MQTT/MQTTPUBSTATUS/MQTTINFO`、`OTA/OTAPREFLIGHT=<json>/OTANOTIFY=<json>`、`SERVER/SERVERPROBE`、`AIREC/AIRECSTART/AIRECSTOP/AIRECCANCEL/AIRECUPLOAD/AIWAKE`、`WW/WWSTART/WWPAUSE/WWRESUME/WWSTOP`、`AUDIO/AUDIOSTART/SINETEST/AUDIOTEST/AUDIORESET/AUDIOCUE/AUDIOGEN/VOLUME/MICGAIN/MIC/I2CSCAN`、`BLE/BLESCAN/BLEPAIR=n/BLEACON/BLEACOFF/BLECTEMP/BLEMODE/BLEFAN/BLEDISP/BLELIGHT/BLESWING/BLEKEEP/BLEDROP`、`MODE=BLE|IR|RF433`、`IR/IRLIST/IRLEARN=<name>/IRSEND=<name>[,<count>]/IRCLEAR`、`RF/RF=IDLE|LEARN|LISTEN|SNIFF/RFSEND=<hex>,<bits>[,<T>]/RFTEST/RFDUMP`、`SCENE/SCENELIST/SCENERUN=<id>/SCENEDEL=<id>/SCENECLEAR/SCENEADDIR=<desc>,<ir_name>/SCENEADD433=<desc>,<hex>,<bits>,<T>`、`AICMD=<json>`、`UIKEY=KEY1|KEY2|KEY1_LONG`、`THEME=LIGHT|DARK`、`PART`、`VER/VERSION`、`CHIP`、`TASKS`、`LOG=OFF|0|1|2|3` 和 `RESTART`。IDF console 优先读取 `stdin`；当前 sdkconfig 是 USB Serial/JTAG 主控制台，console 任务在 `consoleTask` 启动时调用 `usb_serial_jtag_driver_install` + `usb_serial_jtag_vfs_use_driver` 让 stdin 走 USB JTAG VFS 驱动（否则 read 永远拿不到字节）。

- `HELP` / `H`：查看帮助。
- `STATUS` / `?` / `S`：IDF 固件状态，包含日志级别、Flash guard、LittleFS、显示/LVGL、UI bridge、传感器样本数、BLE bridge/目标/连接状态、按键事件计数和内存摘要。
- `DIAG` / `FREE`：内部 SRAM、最大连续块、PSRAM、Console/LVGL/Input/Sensors/OTA/DNS/Recorder/4G RX/Transport/WakeWord/BLE 任务栈高水位。
- `FS` / `LITTLEFS`：LittleFS 资源分区挂载、容量和 manifest 状态。
- `DISPLAY` / `LCD` / `SCREEN`：LVGL 已启动时请求刷新当前 UI；如果 LVGL 未启动则重绘 ST7789P3 启动色带 fallback，并显示背光/初始化/LVGL 状态。
- `KEY` / `KEYS` / `ADCKEY`：显示 IDF ADC 按键当前 raw/mV、活跃按键和最近一次 KEY1/KEY2/BOTH 动作。
- `SENSORS` / `BAT` / `TEMP`：显示 IDF 传感器缓存，包括 GPIO1 电池 raw/mV/电压/百分比、GPIO37 充电状态和 GPIO2 温度 raw/mV/摄氏度。
- `BLE` / `BLESTATUS`：显示 ESP-IDF BLE bridge、NimBLE stack、目标 MAC、连接状态、配对扫描结果和最近错误。
- `BLESCAN`：启动 IDF NimBLE 配对扫描，最多缓存 10 个可连接广播。
- `BLEPAIR=<1..10>`：连接 `BLE` 列表中的指定序号，校验 `FFE0/FFE2` 后保存到 NVS `ble_aircon/target_mac` 和 `ble_aircon/addr_type`。
- `BLECANCEL`：取消当前 IDF BLE 扫描/配对。
- `BLEACON` / `BLEACOFF`：通过 IDF NimBLE `FFE2` 写控制帧，打开或关闭 BLE 空调。
- `BLECTEMP <18~31>`：设置温度。
- `BLEMODE COOL|VENT|ECO|SLEEP`：设置制冷、送风、节能或睡眠模式；`FAN` 也按送风处理。
- `BLEFAN <1~5>`：设置风速。
- `BLEDISP ON|OFF`：设置空调显示。
- `BLELIGHT ON|OFF`：设置灯光。
- `BLESWING H|V`：设置水平或垂直摆风。
- `BLEKEEP=1|0`：连接保持策略。默认 `1`，命令成功后保持 BLE command connection；`0` 表示命令后自动断开并释放连接。
- `BLEDROP`：释放当前 IDF BLE 命令连接。
- `AICMD=<json>` / `CTRL=<json>`：把服务器样式 JSON 交给 `App_IdfCommandExecutor` 执行；支持顶层 `control`、顶层 `command` 或直接传 control object。当前 `aircon_ble_v1` 会执行 BLE 空调 steps，`speaker_v1` 会执行音量设置/步进；顶层 `audio_cue` 为 `wake_ack`、`settings_done` 或 `done` 时会在控制命令后播放 LittleFS 本地 cue。
- `AUDIO` / `AUDIOSTATUS`：显示 ESP-IDF ES8311/I2S 音频状态、codec ID、音量、麦克风和 PA 状态。
- `AUDIOSTART`：手动初始化 IDF ES8311/I2S 音频底座。
- `SINETEST [100..4000]`：通过 IDF I2S 播放约 3 秒正弦波，默认 1000Hz。
- `AUDIOTEST`：开启 PA 并写约 5 秒静音，用于听功放底噪。
- `AUDIORESET` / `AUDIORECOVER`：关闭 PA，重启 IDF I2S TX/RX 和 ES8311 DAC，关闭麦克风通道并恢复音量。
- `AUDIOCUE=wake_ack|settings_done|done`：按 LittleFS manifest 随机选择对应本地固定语音并同步播放。
- `AUDIOGEN=boot|low_battery|record_stop`：播放 `include/audio_cues_data.h` 中的固件短提示音。
- `VOLUME=<0..100>` / `MICGAIN=<0..100>` / `MIC=ON|OFF`：IDF 音量和麦克风调试。
- `I2CSCAN` / `I2C`：扫描 ES8311 所在 I2C 总线。
- `WIFI` / `WIFISTATUS`：显示 IDF WiFi STA 初始化、凭据、SSID/IP/RSSI、配网 portal、断开原因和最近错误。
- `WIFISCAN`：阻塞式 WiFi STA 扫描，最多显示 12 个网络。
- `WIFICONNECT=<ssid>,<password>`：保存到 `wifi_cfg` NVS 并请求 STA 连接；SSID/password 用逗号分隔，开放网络密码可留空。
- `WIFICLEAR`：只清除 `wifi_cfg` 里的 WiFi 凭据，不动模型、BLE 和 UI 配置；清除成功后会启动 ESP-IDF 配网页。
- `WIFIPORTAL` / `WIFIAP` / `PORTAL`：启动 ESP-IDF 配网热点，SSID 为 `ESP32_` 加 MAC 后两字节，AP IP 为 `192.168.4.1`；同时启动 HTTP portal 和 captive DNS。
- `WIFIPORTALSTOP` / `WIFIAPSTOP` / `PORTALSTOP`：停止 ESP-IDF 配网页，保留 STA 模式。
- `WIFIDROP` / `WIFIDISCONNECT`：断开当前 WiFi STA。
- `NET` / `NETSTATUS`：显示 ESP-IDF active transport，包含 AUTO/WIFI/4G 模式、当前 active、pending、4G PPP/MQTT 阶段、fallback 抑制和最近错误。
- `NET=AUTO` / `NET=WIFI` / `NET=4G`：切换 ESP-IDF 网络模式。
- `NETCANCEL`：取消当前 4G PPP/MQTT 尝试并回到 WiFi 优先路径。
- `4G` / `CELL`：显示 ESP-IDF 4G modem/PPP 状态、IMEI、CSQ、IP、拨号次数和最近错误。
- `4GON` / `CELLON`：打开 4G 电源并初始化 UART。
- `4GOFF` / `CELLOFF`：断开 PPP 并关闭 4G 电源。
- `4GHANGUP` / `CELLHANGUP`：断开当前 PPP。
- `4GCSQ` / `CELLCSQ`：在 PPP 未运行时刷新 CSQ。
- `4GDIAL[=<apn>]` / `CELLDIAL[=<apn>]`：手动拨号，默认 APN 为 `cmnet`。
- `MQTT` / `MQTTSTATUS`：显示 IDF MQTT 客户端状态、设备 ID、topic、收包数、命令执行计数和最近错误。
- `MQTTPUBSTATUS`：立即发布一次 online status。
- `MQTTINFO`：立即发布一次 device_info。
- `OTA` / `OTASTATUS`：显示 IDF OTA 下载、pending verify、进度、目标版本、request_id 和最近错误。
- `SERVER` / `SERVERSTATUS`：显示 IDF 业务 TCP 服务器 endpoint、最近 probe、响应 JSON 和错误状态。
- `SERVERPROBE`：通过当前 active transport 默认路由连接业务 TCP 服务器，发送设备身份包和 META 能力包后关闭连接；不上传录音。
- `AIREC` / `AIRECSTATUS`：显示 IDF 录音上传状态、已录字节数、上传次数、最近错误和 `IDF_Recorder` 状态。
- `AIRECSTART`：开始 IDF 手动录音；会开启麦克风 ADC，设置录音增益并写入 512KB PSRAM buffer。
- `AIRECSTOP`：停止录音，播放 `record_stop` 固件短提示音，然后通过 IDF TCP server 上传 PCM 并执行服务器返回 JSON。
- `AIRECCANCEL`：停止当前录音但不上传。
- `AIRECUPLOAD`：重新上传上一段已经录好的 PCM。
- `AIWAKE` / `WAKEONCE`：不依赖真实唤醒词，模拟一次 WakeWord 命中，进入 `wake_ack` + VADNet 自动录音上传流程。
- `WW` / `WAKE` / `WAKEWORD`：显示 IDF WakeWord 模型、任务、暂停、RMS、命中次数和栈水位。
- `WWSTART` / `WAKESTART`：初始化并启动 IDF WakeWord。
- `WWPAUSE` / `WAKEPAUSE`：暂停 WakeWord feed/fetch 检测。
- `WWRESUME` / `WAKERESUME`：恢复 WakeWord，重置 AFE buffer 和 WakeNet 阈值。
- `WWSTOP` / `WAKESTOP`：停止 WakeWord 并释放 feed/fetch 任务栈。
- `THEME` / `THEME=LIGHT` / `THEME=DARK`：查看或切换 ESP-IDF UI 主题；切换会写入 NVS `ui/theme` 并重建 MainScreen、QRScreen、IDF 设置页和 IDF BLE 配对页。
- `MODE` / `APPMODE`：查看当前应用模式（BLE/IR/RF433）。
- `MODE=BLE` / `MODE=IR` / `MODE=RF433`：切换互斥外设模式，会写入 NVS namespace `appmode` 后调用 `esp_restart()` 软重启（约 ~3 秒后生效）。详见 `docs/ir-433-scene-status.md`。
- `IR` / `IRSTATUS`：显示 IR 模块状态（是否启动、是否在学习、当前学习名、IDF_IR 任务高水位、引脚）。
- `IRLIST`：列出 LittleFS `/littlefs/ir_codes.json` 中已学的 IR 指令名字。
- `IRLEARN=<name>`：开始学习一个 IR 信号，需要在 IR 模式下；按提示用遥控器对着接收头按两次同一个键进行二次确认。
- `IRLEARN`（不带参数）：停止当前学习。
- `IRSEND=<name>[,<count>]`：按名字回放学到的 IR raw 波形，可选重复次数。
- `IRCLEAR`：删除整个 `/littlefs/ir_codes.json`。
- `RF` / `RF433` / `RFSTATUS`：显示 RF433 模块状态（是否启动、当前 RF mode、IDF_RF433 任务高水位、引脚）。
- `RF=IDLE` / `RF=LEARN` / `RF=LISTEN` / `RF=SNIFF`：切换 RF433 工作模式，需要在 RF433 模式下。
- `RFTEST`：发射 5 段 5ms 高低脉冲测试波形，验证 TX 路径硬件通。
- `RFDUMP`：读 13 个关键寄存器（init 表 anchor + MODE_CTL/IO_SEL/INT_EN/FIFO_CTL + TS3260 扩展 0x61）对比预期值，verdict 一键归类（all match / all 0xFF MISO 浮空 / all 0x00 / 部分 mismatch）。排查 CMT2300A / TS3260 / MRF2300A 板子"按键无响应"的第一手段，能直接区分硬件虚焊 / 芯片没电 / SPI 时序问题。
- `RFSEND=<hex_code>,<bits>[,<T_us>]`：按学到的 code/bitLen/T 直接发射；T 缺省时使用模块默认 300us。
- `SCENE` / `SCENESTATUS`：显示场景模块状态、当前模式、条目数量。
- `SCENELIST`：列出所有场景，显示 `[id] type desc`（type=IR 或 433）。
- `SCENERUN=<id>`：执行场景；type 与当前 app mode 不匹配时返回 `ESP_ERR_INVALID_STATE`。
- `SCENEDEL=<id>` / `SCENECLEAR`：删除单个或全部场景。
- `SCENEADDIR=<desc>,<ir_name>`：添加 IR 场景，desc 是描述（不含逗号），ir_name 必须是已学 IR 指令名。
- `SCENEADD433=<desc>,<hex>,<bits>,<T_us>`：添加 433 场景；hex 是十六进制 code，bits 是位长（1-64），T_us 是脉冲基本宽度。
- `UIKEY=KEY1|KEY2|KEY1_LONG`：通过串口向 `App_IdfUi::handleKeyEvent()` 注入 UI 导航按键，用于不按实体键验证 MainScreen、BLE 配对页和设置页导航；故意不提供 `KEY2_LONG` 注入，避免误触发录音上传/AI 路径。
- `OTAPREFLIGHT=<json>` / `OTANOTIFY=<json>`：通过串口把 OTA preflight 和正式 OTA JSON 注入同一套 `App_IdfOta` 入口，用于本地闭环验证；它仍会执行版本、`request_id`、电量/录音状态、URL/MD5/size、active transport 和 OTA 分区大小检查。
- `PART` / `PARTITIONS`：关键分区、OTA slot 和模型分区头部。
- `VER` / `VERSION` / `V`：固件版本和 OTA 分区信息。
- `CHIP` / `CHIPINFO`：ESP32-S3 芯片和 Flash 信息。
- `TASKS` / `TASK`：已迁入 IDF 的任务诊断。
- `LOG=OFF`、`LOG=0`、`LOG=1`、`LOG=2`、`LOG=3`：日志级别。
- `RESTART` / `RESET` / `REBOOT`：重启设备。

当前固件**已恢复** IR / RF433 / 场景串口命令（详见 `docs/ir-433-scene-status.md`），不保留旧 AT 透传或 Arduino 专属调试命令。网络、音频、按键和 WiFi 配网页诊断都使用上面的 IDF 命令。

## Python 工具

- `tools/serial_monitor.py`：轻量串口监视和单命令发送。
- `tools/mysql_tunnel.py`：本地打开 SSH 隧道访问云服务器 MySQL，供 DBeaver/Navicat/DataGrip 等可视化工具连接。默认本地连接参数是 `127.0.0.1:3307`，数据库名和用户名来自 `server-side files/.env`，密码不打印，需手动填 `.env` 中的 `MYSQL_PASS`。
- `tools/ble_aircon_voice_probe.py`：BLE 空调语音链路联合探针。可用 EdgeTTS 生成模拟语音并按 ESP32 TCP 协议发给 `ai_server.py`，同时 tail Python 服务日志和 ESP32 串口日志；加 `--execute-ble-via-serial` 时会把服务器返回的 `aircon_ble_v1` steps 映射成现有 `BLEACON` / `BLECTEMP` 等串口命令，用来验证 BLE 连接、写入、断开和耗时。
- `tools/generate_audio_cues.py`：离线生成固件提示音。当前只生成 `boot`、`low_battery`、`record_stop`。默认输出到 `asset/audio_cues/`，格式是 16kHz、16-bit、单声道、无 WAV 头 `.pcm`，并在 `asset/audio_cues/preview_wav/` 生成同名 `.wav` 方便试听。常用命令：`python tools/generate_audio_cues.py --header include/audio_cues_data.h`；只生成固件用 PCM 可加 `--no-wav`；调音量可加 `--volume 0.55`。
- `data/audio_cues/`：LittleFS 本地固定语音资源。修改这些 `.pcm` 或 `manifest.json` 后运行 `idf.py build` 验证镜像，烧录时运行 `idf.py -p PORT spiffs-flash` 或直接 `idf.py -p PORT flash`。
- `server-side files/test_ai_profiles.py`：服务器侧 AI product profile smoke test，覆盖 BLE 空调 profile、旧 `profile=ble_only` fallback、未知 `product_id`、非法温度和不支持的控制协议。

`ble_aircon_voice_probe.py` 依赖 `pyserial` 和 `edge-tts`；macOS 上会优先用系统自带 `afconvert` 把 EdgeTTS MP3 转成 16k/16bit/mono PCM，其他系统需要 `ffmpeg`。若本机缺少 EdgeTTS：

```powershell
python -m pip install edge-tts
```

## 调试建议

- 改固件后先编译，再上板看 `DIAG`。
- 涉及 BLE、WakeWord、WiFi 配网、OTA 时，重点看 `Internal SRAM free` 和 `Largest internal block`。
- 遇到网络问题先用 `STATUS` 看 active transport/MQTT transport，再用 `WIFI`、`NET=AUTO`/`NET=4G` 区分 WiFi、PPP、MQTT、服务器问题。
- 需要试听 MCU 实际上传录音时，在服务器 `.env` 设置 `AI_SAVE_UPLOADED_AUDIO=true` 并重启 `ai_server.py`。录音会以 WAV 保存到 `ai_server.py` 所在目录下的 `debug/uploaded_audio/`，日志会打印完整路径；`AI_UPLOADED_AUDIO_MAX_FILES` 默认只保留最近 200 个，避免调试文件长期占满磁盘。
- 排查“服务器已经回复成功 TTS，但空调没有动作”时，优先用 `tools/ble_aircon_voice_probe.py --text "打开空调" --repeat 3 --execute-ble-via-serial --diag-before --diag-after` 做拆分验证：服务器回包只证明 ASR/NLU 生成了控制 JSON，真正 BLE 是否连接、写入并断开，要看串口里的 `BLE air conditioner command write succeeded`、`BLE command connection has been released` 和 `OK`/`ERR`。
- 对比语音理解链路时，服务器需先设置 `AI_ALLOW_META_PIPELINE_OVERRIDE=true` 并重启，然后用 `tools/ble_aircon_voice_probe.py --ai-pipeline split ...` 或 `--ai-pipeline omni ...` 让单次测试走指定链路。
