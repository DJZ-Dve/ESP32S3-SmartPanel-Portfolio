# 网络、服务器与 OTA 现状

## 网络模式

WiFi PS 当前默认 `WIFI_PS_NONE`（`App_IdfNetwork.cpp:566`），省电模式 L1 由 `AppIdfPowerSave` 切到 `WIFI_PS_MIN_MODEM`，退出 L1 时回 `WIFI_PS_NONE`；MQTT keepalive 60s 不变，OTA preflight/notify 期间 `AppIdfOta::isBusy()` 抑制进入 L1，详见 `docs/power-save.md`。

当前固件支持三种网络模式，实现在 `App_IdfTransport::NetMode`：

- `NET_MODE_AUTO`：默认，WiFi 优先。WiFi 连续失败 **2 次**（`kWifiMaxRetries`）或超过 30 秒仍未连上后，按需拨 4G PPP 兜底；WiFi 恢复稳定约 15 秒后切回 WiFi 并断开 PPP 以省电。AUTO 模式下 WiFi 失败不会自动开启 AP 配网门户，4G 负责兜底。
- `NET_MODE_WIFI_ONLY`：只走 WiFi，禁用并断开 PPP fallback。WiFi 连续失败 **2 次**后自动开启 AP 配网门户；配网门户激活时停止自动重连，等用户重新配对。
- `NET_MODE_4G_ONLY`：强制 4G PPP-only 业务模式，WiFi/配网页不参与业务。

**WiFi 重连计数**：`App_IdfNetwork::Snapshot::wifiFailCount` 记录 `WIFI_EVENT_STA_DISCONNECTED` 累计次数，连接成功（`IP_EVENT_STA_GOT_IP`）或凭据更新（`saveCredentials`/`clearCredentials`）时归零。

**4G PPP 故障处理**：
- 拨号失败：按 30 秒间隔无限重试，直至 WiFi 恢复。
- PPP 就绪但 MQTT 约 75 秒仍未连上：挂断 4G，进入抑制状态（`autoCellularSuppressed`）。抑制期间 WiFi 仍持续重连；若 WiFi 恢复则立即解除抑制；若 WiFi 始终未回来，抑制 **5 分钟**（`kAutoSuppressResetMs`）后自动解除，重新走 PPP + MQTT 流程，避免设备永久无网。

手动切换到 4G-only，或 AUTO 下进入 PPP fallback 时，active transport 进入 PPP/MQTT 连接阶段。`NETCANCEL` 可放弃当前 4G/MQTT 连接尝试；4G-only 下 PPP 约 120 秒仍未就绪，或 PPP 就绪后 MQTT 约 75 秒仍未连上，会自动切回 AUTO。切换模式（`requestMode`）会同步重置所有 4G 计时器和抑制状态。

串口命令：

- `NET=AUTO`
- `NET=WIFI`
- `NET=4G`

4G 硬件 bring-up 调试命令（必须先 `NET=WIFI` 退出 4G_ONLY 让 dial 循环停下）：

- `4GDIAG` — 打印 GPIO45/GPIO46 电平、UART 状态、started/powered/dialing/ppp/connected、IMEI/CSQ/last_err
- `4GAT=<at>` — 直接发裸 AT，等 3 秒并打印响应（PPP 跑起来后会拒绝）
- `4GUART` — 抓 3 秒 UART2 RX 原始字节，看模组有没有 boot/URC 输出
- `4GPWRKEY=PULSE|PULSE_HIGH|HIGH|LOW|READ` — 手动驱动/读 GPIO46 PWRKEY，PULSE 默认低 600ms
- `PIN_4G_PWR/PIN_4G_PWRKEY` 已配 `GPIO_MODE_INPUT_OUTPUT`，`gpio_get_level()` 反映真实输出电平

## WiFi 配网

WiFi 配网模块是 `App_IdfNetwork`：

- NVS namespace：`wifi_cfg`
- 热点 SSID：`ESP32_` 加 MAC 后两字节。
- AP IP：`192.168.4.1`
- Web 配网页端口：80
- 路由：`/`、`/scan`、`/save`、`/status`

如果没有保存的 WiFi 凭据，设备会启动配网热点。内部 SRAM 不足时会拒绝启动配网门户。
配网页开启但 STA 未连接时，`NET_MODE_AUTO` 会暂停 4G PPP fallback，避免手机配网页、WiFi scan、MQTT/TLS fallback 同时抢 WiFi/网络栈资源；配网完成并连上 WiFi 后恢复正常 WiFi-first AUTO 行为。

它使用 NVS namespace `wifi_cfg` 和 key `ssid/password`，启动时有凭据就请求 STA 连接，没有凭据就启动开放配网热点 `ESP32_xxxx`，AP IP 为 `192.168.4.1`，HTTP 端口 80，路由为 `/`、`/scan`、`/save`、`/status`，并把 Android 常见联网检测路径 `/generate_204`、`/generate204` 映射到配网页。`/scan` 使用 active scan，并在串口打印本次扫描结果数量，便于诊断手机配网页没有列出 WiFi 的情况；扫描结果和 JSON 响应放在堆上，portal HTTP server 使用 8KB 栈，避免手机 captive portal 自动请求触发扫描时压垮 httpd 栈；STA 未连接时网络快照不会调用 `esp_wifi_sta_get_ap_info()` 查询 RSSI，避免 AP 配网扫描入口触发 STA 信息查询。配网 portal 还会启动轻量 UDP DNS，端口 53，对任意 A 查询返回 `192.168.4.1`。串口命令包括 `WIFI`、`WIFISCAN`、`WIFICONNECT=<ssid>,<password>`、`WIFICLEAR`、`WIFIPORTAL`、`WIFIPORTALSTOP`、`WIFIDROP`；`WIFICLEAR` 会清除凭据后直接重新打开配网页。固件通过当前 active transport 订阅设备/广播指令、上报 online/device_info、处理 OTA preflight 和正式 OTA 下载，并可通过 `SERVERPROBE` 验证业务 TCP 服务器的身份/META 包路径，通过 `AIRECSTART/AIRECSTOP`、`AIWAKE` 或 MainScreen KEY2 长按上传 16kHz PCM。

## 4G

4G 模块是 LE270-EU，主要代码在：

- `src/idf/App_IdfCellular.cpp`：ESP-IDF 默认构建的 UART2 + lwIP PPPoS 底座。
- `src/idf/App_IdfTransport.cpp`：ESP-IDF 默认构建的 AUTO/WIFI/4G active transport 管理器。

当前业务网络层是 PPP-only：

- `App_IdfCellular` 负责 4G 上电、UART AT 探测、PPP 拨号、断开、默认路由、拨号前 IMEI/CSQ 缓存和 lwIP PPPoS。
- `NET=4G` 是正式 PPP-only 模式，不再是 MQTT 测试模式。
- 原始 `AT...` 透传和 `DEBUG4G` 在 PPP-only 固件路径下直接不可用，不做运行时退出 PPP 再执行 AT 的路径。
- 业务不保留旧 AT/TCP 路径。

全机业务层同一时刻只有一个 active transport：

- `NONE`：无可用业务网络。
- `WIFI`：WiFi 是 MQTT/AI/OTA 默认网络。
- `PPP_4G`：4G PPP 是 MQTT/AI/OTA 默认网络。

MQTT 只有一个 `App_IdfMqtt/esp-mqtt` 客户端实例。active transport 切换时会先断开 MQTT，再设置默认路由并重连，避免 WiFi 和 4G 同时保留两个 MQTT 会话。

`AppIdfMqtt::start()` 不在 `app_main()` 里直接调用，而是由 `AppIdfTransport` 在拿到 WiFi IP（`IP_EVENT_STA_GOT_IP`）或 4G PPP 拨号就绪后，通过 `activateWifi/activateCellular → restartMqttForRoute()` 拉起，避免 DHCP/DNS 还没就绪时触发 `esp-tls`/`mqtt_client` 的 `getaddrinfo` 失败日志。

`STATUS` / `NET` 会显示 WiFi、PPP、active transport 和 MQTT 状态。ESP-IDF 下 `4G` / `4GCSQ` 显示 PPP 进入 data mode 前缓存的 CSQ；PPP 运行中不做实时 AT 查询。

## 服务器地址

业务 TCP 服务器在 `src/idf/App_IdfServer.cpp`：

- host：`<your-server-host>`
- port：`9090`

它使用 lwIP socket 连接同一 host/port，发送原有 `[4B id_len] + device_id` 身份包和 `META` 能力包，并按十六进制 JSON + `*` 结束符解析响应后交给 `App_IdfCommandExecutor`。`SERVERPROBE` 只做连接和身份/META 写入验证，不上传录音；`App_IdfRecorder` 的 `AIRECSTOP` 会调用 `uploadPcmAndReceive()` 发送 `[4B audio_bytes] + PCM` 并等待服务器响应。

MQTT 在 `src/idf/App_IdfMqtt.cpp`：

- broker：`<your-mqtt-broker-host>`
- port：`1883`

它使用同一套 broker/topic 规则。设备身份优先使用 `App_IdfCellular` 拨号前缓存到的 IMEI；如果 IMEI 尚不可用，则使用 WiFi MAC fallback。

不要把服务器凭据复制进新文档。具体 host / port / 凭据由部署者自行通过本地配置注入，不进版本库。

## AI 产品 Profile

`ai_server.py` 现在把 AI 交互层拆成公共主流程和产品 profile：

- 公共主流程继续负责 TCP 传输、ASR/NLU、`audio_cue` 选择、OTA、MQTT、MySQL 和管理接口；服务端不再生成或下载 TTS PCM。
- 语音理解链路可用 `AI_PIPELINE` 切换：未配置时默认 `split`，即 `qwen3-asr-flash` ASR + 文本 NLU；`omni` 使用 `qwen3.5-omni-flash` 通过 OpenAI 兼容 HTTP 流式接口直接接收音频+prompt 并输出 JSON。线上可在 `.env` 中把 `AI_PIPELINE` 设为 `omni`。
- `AI_OMNI_MODEL` 默认是 `qwen3.5-omni-flash`，`AI_OMNI_TIMEOUT_SEC` 默认是 `8` 秒，`DASHSCOPE_OPENAI_BASE_URL` 默认是 `https://dashscope.aliyuncs.com/compatible-mode/v1`。测试对比时可临时打开 `AI_ALLOW_META_PIPELINE_OVERRIDE=true`，让调试工具通过 `META.ai_pipeline` 指定单次请求链路。
- Omni 请求超时、HTTP 失败或 SSL 异常时，服务端快速返回无控制命令的本地缓存短回复 `请说。`，避免 MCU 长时间卡在 `receiveResponse`。
- 产品 profile 支持旧目录式 `server-side files/products/{product_id}/product.json`，也支持新的单文件 `server-side files/products/{product_id}.txt`。
- 当前默认产品是 `esp32s3_ble_aircon`。

产品知识和协议资产保存在服务器，不放进 MCU 固件。MCU 只保留通用 TCP 传输协议、`META.product_id` 上报和少量执行器。
当前 BLE 空调产品已改为单文件 `products/esp32s3_ble_aircon.txt`，prompt 直接要求模型输出 MCU 可执行的 `aircon_ble_v1`、`speaker_v1`、`local_scene_v1` 或 `local_label_v1` steps（后两者用于 IR/RF433 模式下的本地场景执行和学习时打标签）。
服务端响应只下发协议化控制字段：

```json
{
  "control": {
    "has_command": true,
    "protocol": "aircon_ble_v1",
    "protocol_version": 1,
    "steps": []
  }
}
```

服务端对单文件 profile 的直出 command 做白名单校验，只允许当前固件支持的协议、版本和 step 参数；固件再按 `protocol/steps` 执行，未知协议或版本拒绝执行。`speaker_v1` 用于音量设置和音量步进；`local_scene_v1` 校验 `steps=[{"type":"run_scene","scene_id":int}]`，并要求 `scene_id` 出现在 `device_meta.scene_labels`；`local_label_v1` 不走 steps，直接在 command 顶层带 `label` 字段，服务端校验长度 ≤ 8、字符落在 ASCII + `ALLOWED_LABEL_CHARS`、且 `device_meta.pending_signal` 非空。`App_IdfCommandExecutor` 可解析同一份服务器 JSON 中的 `control`，把 `aircon_ble_v1` steps 直接接到 `App_IdfBleAircon`，把 `speaker_v1` 音量 steps 接到 `App_IdfAudio`，并在顶层 `audio_cue` 存在时执行控制后播放 LittleFS 本地语音。串口可用 `AICMD=<json>` 独立验证这条业务执行路径。

服务器日志现在会输出分段耗时：

- `[ASR] 完成: ...ms`
- `[NLU] 首包: ...ms`
- `[NLU] 完成: ...ms`
- `[OMNI] 首包: ...ms`
- `[OMNI] 完成: ...ms`
- `[AI Timing] pipeline=split asr=...ms nlu=...ms total=...ms`
- `[AI Timing] pipeline=omni omni=...ms total=...ms`

调试上传录音质量时，可在服务器 `.env` 中设置 `AI_SAVE_UPLOADED_AUDIO=true` 并重启 `ai_server.py`。服务端会把 MCU 上传的 16kHz、16-bit、单声道音频另存为 WAV，默认目录是 `ai_server.py` 所在目录下的 `debug/uploaded_audio/`；可用 `AI_UPLOADED_AUDIO_DIR` 改目录，用 `AI_UPLOADED_AUDIO_MAX_FILES` 限制最多保留多少个 `.wav`，默认 `200`，设为 `0` 表示不自动清理。

MCU 建连后发送 `META`，现在按当前 `App_IdfAppMode` 动态拼接（`src/idf/App_IdfServer.cpp::sendIdentity`）：

- `product_id`：固定 `esp32s3_ble_aircon`，**所有模式不变**——保护服务器侧路由，避免按模式分裂多个 product profile。
- `profile`：随当前模式变化，取值 `ble`（默认）/ `ir` / `rf433`。旧 BLE-only 固件缺少 `product_id` 时仍可通过 `profile=ble_only` fallback。
- `capabilities`：当前模式三选一置 true：`aircon_ble`、`ir`、`rf433` 中只有匹配模式的为 true，另两项 false。`scenes` 由 `AppIdfScene::isStarted()` 决定（IR 或 RF433 模式下场景模块启动则 true，BLE 模式下 false）。

例：IR 模式 META = `{"product_id":"esp32s3_ble_aircon","profile":"ir","capabilities":{"aircon_ble":false,"ir":true,"rf433":false,"scenes":true},"idf":true}`。

未知 `product_id` 不会执行设备控制，服务端只返回无控制指令的错误回复。新增 IR、433 或其他设备的产品语义时，应新增独立产品目录，不要把产品 prompt 和协议校验重新写回 `ai_server.py` 主流程。IR/RF433 模式下的场景执行/学习现已纳入服务器协议白名单：AI 按 prompt 输出 `local_scene_v1`/`local_label_v1`，服务端校验通过后下发，MCU 由 `App_IdfCommandExecutor` 路由到 `App_IdfScene::executeById` / `App_IdfLearnFlow::onLabelArrived` 执行。

## 本地固定语音协议

运行时不再让服务器推送 Qwen-TTS PCM。`ai_server.py` 只发送十六进制 JSON、结束符 `*` 和可忽略的 EOS 标记；MCU 收到 `*` 后停止读取响应，根据 JSON 里的 `audio_cue` 从 LittleFS 播放固定本地语音。

响应字段：

```json
{
  "status": "ok",
  "reply_text": "",
  "audio_cue": "settings_done",
  "control": {
    "has_command": true,
    "protocol": "aircon_ble_v1",
    "protocol_version": 1,
    "steps": []
  }
}
```

`audio_cue` 白名单（`ai_server.py` 中 `ALLOWED_AUDIO_CUES`，与固件 `App_IdfAudio::isSupportedCueName` 保持一致）：

| cue 名 | 触发方 | 触发条件 |
|---|---|---|
| `wake_ack` | 固件 | WakeWord 命中后本地播放（不走服务器） |
| `settings_done` | 服务器 | `has_command=true` 默认值 |
| `done` | 服务器 | 通用完成回应（备用） |
| `op_failed` | 服务器 / 固件 | 服务器：用户意图是空调但参数不支持。固件：BLE 写入/ACK 失败时覆盖 |
| `chitchat_unsupported` | 服务器 | LLM 判定用户在闲聊/问答/问候 |
| `device_unsupported` | 服务器 | LLM 判定用户在控制非空调设备 |
| `not_understood` | 服务器 / 固件 | 服务器：极不确定时主动下发。固件：上传成功但 JSON 既无命令也无 cue 时兜底 |
| `none` | 服务器 | 无控制命令的默认值（已被上面分类替代，保留兼容） |

LLM prompt（`server-side files/products/esp32s3_ble_aircon.txt`）已要求模型对每条无控制命令的回复显式分类填入 `audio_cue`，覆盖闲聊/外设控制/参数不支持/纯听不清四种情况。控制命令成功时不写 `audio_cue`，由服务器 `choose_audio_cue` 自动补 `settings_done`。

MCU 收到 JSON 后优先执行 `control`，再按"BLE 失败覆盖 → 上传失败覆盖 → 服务器 cue → 兜底 cue"的优先级播放本地 cue；如果 LittleFS 资源缺失、manifest 解析失败或 cue 为 `none`，只跳过本地语音，不再显示"服务器未返回语音"，控制命令仍照常执行。

旧的 `server-side files/cache/wake_tts/` 运行缓存和 `generate_tts_cache.py` 云端 TTS 生成脚本已从当前仓库流程移除。固定语音统一放在固件 LittleFS 的 `data/audio_cues/`，服务器只下发 `audio_cue` 名称。

MCU 侧接收语音响应时不再用 `Client::connected()` 判断服务端是否关闭连接。该 API 在 PPP/lwIP 路径上可能阻塞并导致 Net 任务 WDT，当前接收循环改为依赖数据可读、`*` 结束符和 30 秒超时退出。

## 设备身份与 MQTT Topic

设备优先使用 4G IMEI 作为身份；如果 IMEI 为空，则 fallback 到 MAC。

主要 Topic：

- 指令下行：`server/cmd/{imei}/down`
- OTA 通知：`server/ota/{imei}/notify`
- 广播指令：`server/cmd/broadcast/down`
- 状态上报：`esp32/{imei}/telemetry/status`
- OTA 结果：`esp32/{imei}/ota/result`
- OTA 预检 ACK：`esp32/{imei}/ota/preflight_ack`
- 设备信息：`esp32/{imei}/telemetry/device_info`

下行 `beep` 命令当前只记录日志，不再触发本地提示音。

## OTA

固件版本定义在根 `CMakeLists.txt` 的 `PROJECT_VER`。

OTA 流程：

1. 服务端通过 `/ota/batch` 创建开放 OTA 批次。
2. 调度器向候选设备下发 `ota_preflight`。
3. MCU 回复实时 ACK；服务端只对通过 preflight 的设备定向发送正式 OTA JSON。
4. 正式 OTA JSON 必须带 `request_id`，且必须匹配最近一次通过的 `ota_preflight`，否则拒绝。
5. MCU 比较 `version` 是否大于当前 app desc 版本。
6. MCU 在保存 OTA 信息前会拒绝空 URL、超长 URL、空/非 32 位十六进制 MD5、`size == 0`、超长版本号或超长 `request_id`。
7. 调度 OTA（`request_id` + `force=true`）会自动设置 `requested`，进入下载流程。
8. `performOTA()` 下载前二次检查版本、充电/电量、录音、播放、`size > 0` 和 OTA 分区大小；最低电量阈值独立生效，即使要求充电也必须满足 `battery_pct >= min_battery_pct`。
9. 通过当前 active transport 的默认路由 HTTP 下载固件；下载期间 `g_OtaInProgress` 禁止 WiFi/PPP 路由切换。下载使用 1024B 内部 SRAM buffer 流式写入 OTA 分区；HTTP `Content-Length` 如果存在，必须等于 OTA JSON 的 `size`，最终写入字节数也必须等于 `size`。
10. 增量计算 MD5，完成后必须校验通过；空 MD5 不再允许跳过校验。
11. 切换 boot partition 并重启。
12. 新固件启动后 pending verify，联网/MQTT 成功后确认有效；超时可回滚。

> 2026-05-12 起 `sdkconfig.defaults` 已开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（clean build 后会自动合并到 sdkconfig）。开启前 OTA 切分区后状态直接为 `ESP_OTA_IMG_VALID`，`App_IdfOta` 里的 pending verify / 回滚逻辑全部是死代码；开启后新分区进入 `ESP_OTA_IMG_PENDING_VERIFY`，必须在 `kHealthConfirmTimeoutMs=120s` 内由 `confirmRunningApp()` 调用 `esp_ota_mark_app_valid_cancel_rollback()`，否则 `checkRollbackTimeout()` 会调 `esp_ota_mark_app_invalid_rollback_and_reboot()` 回滚旧分区。首次出厂烧录（`idf.py flash`）分区状态是 VALID 不受影响。
>
> 双层兜底：(1) 应用层 `checkRollbackTimeout` 主动调 mark_app_invalid_rollback_and_reboot——要求 `esp_ota_check_rollback_is_possible()=true`，但 USB 烧录后首次 OTA 这个返回 false 不会主动回滚。(2) bootloader 自动 fallback——同一个 PENDING_VERIFY 分区启动 2 次未 mark_valid，bootloader 标 INVALID 跳到 ota_0，**对首次 OTA 也生效**。配合设备物理 reset 按钮和 task watchdog，OTA 失败自救覆盖率足够商业化。

`App_IdfOta` 校验正式 OTA JSON 的 `request_id/url/version/md5/size`，要求匹配最近一次通过的 preflight；下载使用当前 active transport 的默认路由和 `esp_http_client`，1024B 内部 SRAM buffer，写入 `esp_ota_get_next_update_partition()`，校验最终大小和 MD5 后切换 boot partition。新固件启动后如处于 pending verify，`App_IdfMqtt` 成功连接会触发确认；120 秒内未确认且可回滚时会回滚重启。IDF preflight 会把 `App_IdfRecorder` 录音或上传中视为 `recording` 并拒绝升级；WakeWord 常驻麦克风不再算 recording，真正写 Flash 时由 `AppFlashGuard` 暂停 WakeWord。`App_IdfUi` 会读取 OTA snapshot 显示“固件更新”页，覆盖待下载、下载/写入进度、pending verify 和失败 reason；OTA 活跃时不响应普通按键离开页面。

迁移期串口调试可用 `OTAPREFLIGHT=<json>` 和 `OTANOTIFY=<json>` 注入测试 JSON。它不绕过 MQTT OTA 逻辑，只是把 payload 直接传给 `App_IdfOta::handlePreflightRequest()` / `handleNotify()`，用于没有服务端批次调度时验证同一套 request_id 匹配、HTTP 下载、MD5、OTA slot 写入和重启流程。

### OTA preflight 与服务端调度

服务端现在支持 OTA batch 调度。默认配置仍是严格串行，但调度器已经按阶段提供并发参数，后续只需要改服务器 `.env` 并重启 `ai_server.py`：

- `POST /ota/batch`：创建 OTA 批次，从 MySQL 粗筛最近在线、版本较低且同时满足充电和电量策略的设备；默认每批最多 10 台，可通过 `OTA_BATCH_SIZE` 调整。
- `POST /ota/batch/cancel`：取消指定开放批次，停止尚未正式进入 OTA 下载的 `pending`、`preflight_sent`、`preflight_ok` job。
- `GET /ota/batches`：查看最近 OTA 批次，包括当前 `open` 批次，以及 `paused_at` / `pause_reason` / `expires_at`。
- `GET /ota/jobs`：查看最近 OTA job；可追加 `?batch_id=...` 查询单个批次。
- `GET /ota/batch/summary?batch_id=...`：查看指定批次的进度汇总，含 `total`、各状态计数、`completion_pct`、`success_rate`、失败原因 Top 5 和最久未上线的 in-flight 设备列表（含 `hours_offline`）。是运营判断"批次该不该手动 cancel"和"剩下卡的是不是僵尸设备"的主入口。
- 调度器通过 `server/cmd/{imei}/down` 下发 `{"type":"ota_preflight",...}`。
- MCU 实时回复 `esp32/{imei}/ota/preflight_ack`，包含版本、电量、充电、录音/播放、内部 SRAM 等状态。
- 服务端只对 preflight 通过且 ACK 再次满足充电和最低电量的设备发送正式 OTA 通知；MCU 收到正式 OTA 后也会再次检查这两个条件，并等待 `ota/result(success)` 后进入 `rebooting`。
- 最终成功条件是设备重新上报 `fw_version == target_version`，此时 job 标记为 `verified`。

调度并发参数：

- `OTA_BATCH_SIZE`：创建批次时最多粗筛多少台候选设备，默认 `10`。
- `OTA_MAX_PREFLIGHT_IN_FLIGHT`：最多同时推进多少台 preflight/待正式通知设备，默认 `1`。
- `OTA_MAX_OTA_DOWNLOADS`：最多同时发送多少台正式 OTA 下载通知，默认 `1`。
- `OTA_MAX_REBOOT_VERIFYING`：最多同时等待多少台设备重启后 `verified`，默认 `1`。
- `OTA_MAX_RETRY_PER_DEVICE`：单台设备单轮最多尝试次数，默认 `3`；达到后 job 标记为 `skipped`，`last_error=max_retry:<reason>`，避免当下连续空转。
- `OTA_SCHEDULER_TICK_SEC`：调度线程唤醒间隔，默认 `2` 秒。
- `OTA_SKIPPED_RECOVERY_SCAN_LIMIT`：正常队列有空位时，每个 tick 最多主动恢复多少个可恢复 `skipped` job，默认 `5`。
- `OTA_LOW_SRAM_REQUEUE_MIN_LARGEST_BLOCK`：低内部 SRAM 失败后，设备后续上报的 `largest_internal_block` 至少达到该值才重新入队，默认 `12288` 字节。
- `OTA_DEFAULT_EXPIRES_IN_DAYS`：批次默认过期天数，默认 `7`。`/ota/batch` 也支持请求体传 `expires_in_days` 覆盖。

调度器会同时参考下游下载和重启验证槽位，不会无限堆积已经通过 preflight 但长时间未下载的设备。每个 tick 先处理正常 `pending` 和 `preflight_ok` 队列；只有仍有调度空位时，才少量扫描可恢复 `skipped` job，满足最新状态后改回 `pending` 并立即进入下一轮 preflight。HTTP 固件服务当前使用 `ThreadingHTTPServer`，可支撑多设备并发下载；大规模并发时仍建议迁移到更稳定的静态文件服务或 CDN。

批次创建时会自动设置 `expires_at = NOW() + OTA_DEFAULT_EXPIRES_IN_DAYS`（默认 7 天）。到期后调度器会把仍处于 `pending`、`preflight_sent`、`preflight_ok` 的 job 标 `skipped`（`last_error=batch_expired`），批次状态转 `superseded`；已经在 `ota_notified` / `rebooting` 阶段的设备继续沿原有 deadline 走完。`/ota/batch` 请求体可以传 `expires_in_days` 覆盖默认值。这条规则解决了开放批次因为僵尸设备永远卡在 `pending` 导致进度永远到不了 100% 的问题。

批次不会因为当前候选设备全部处理完而自动结束。最新 `/ota/batch` 创建的批次在过期之前保持 `open`，离线低版本设备后续上线并上报状态时，如果满足该批次的粗筛策略，会自动加入这个开放批次。已经在开放批次中因可恢复原因达到 `max_retry` 的 `skipped` job，也会在设备后续上报仍低于目标版本且策略满足时重新入队，并重置这一轮恢复后的重试次数；可恢复原因包括 `preflight_timeout`、`low_internal_sram`、`ota_busy`、录音/播放占用、电池状态暂不可用或下载结果超时等。`low_internal_sram` 会额外参考最新 `largest_internal_block`，仍低于阈值时暂不重新入队。再次创建新的 `/ota/batch` 会把旧开放批次标记为 `superseded`，后续设备直接加入最新版本批次，因此低版本设备可以跨多个历史版本直接升级到当前目标版本。需要停止当前开放批次时，调用 `POST /ota/batch/cancel` 并传入 `batch_id`；已经收到正式 OTA 通知或已经写入成功等待重启验证的设备不会被强行回退。

服务端有自动止损：固件级硬失败（`MD5 mismatch`、`Invalid size`、`Firmware too large`、`Verify error`、`Boot partition error`、`No partition`、`Init failed`）会立即把当前批次设为 `paused`；同一批次 2 台不同设备出现 `reboot_verify_timeout`，或最近 3 台不同设备都因 `max_retry` 跳过，也会暂停。暂停时 `pending`、`preflight_sent`、`preflight_ok` job 会标记为 `skipped`，已进入 `ota_notified` / `rebooting` 的设备继续等待结果。暂停不自动恢复，修正问题后创建新的 `/ota/batch`。

旧 `/ota/push` 手动广播/定向接口已停用；MCU 也不再订阅广播 OTA topic，并会拒绝没有 `request_id` 的旧 OTA 通知。

设备信息上报现在额外包含：

- `charging`
- `largest_internal_block`

更多服务端 OTA 细节见：

- `server-side files/OTA_PROCESS_AND_INTEGRATION.md`
- `server-side files/Server-info.md`
- `docs/ota-preflight-rollout-design.md`：分批 OTA 调度、preflight 实时确认、充电状态筛选和避免流量拥挤的设计报告。

## 时间同步 (SNTP)

设备没有外置 RTC，开机时间默认为 UTC 1970。`App_IdfTime`（`include/App_IdfTime.h` / `src/idf/App_IdfTime.cpp`）负责通过 SNTP 拉取真实时间，由 `app_main()` 在 `AppIdfNetwork::start()` 之后调用 `AppIdfTime::init()`：设置 `TZ=CST-8`、`tzset()` 并配置 SNTP 服务器列表，此时不启动 SNTP，避免没网时反复 DNS 失败。

WiFi 拿到 IP（`App_IdfNetwork.cpp` 的 `IP_EVENT_STA_GOT_IP` 处理）和 4G PPP 拨号成功（`App_IdfCellular.cpp` 的 `pppStatus` 在 `errCode==PPPERR_NONE` 分支）都会调用 `AppIdfTime::onNetworkUp()`：第一次执行 `esp_sntp_init()`，后续重新触发 `esp_sntp_restart()`。SNTP 同步成功后回调里日志一行 `SNTP sync ok: YYYY-MM-DD HH:MM:SS`。

NTP 服务器优先级：`ntp.aliyun.com` → `cn.pool.ntp.org` → `time.windows.com`，国内可达，不依赖海外服务器。

主屏 `ui_ClockLabel` 和 `ui_DateLabel` 在 `App_IdfUi` 的 1Hz `statusTimerCallback` 里通过 `updateMainClockLocked()` 刷新。`AppIdfTime::isSynced()` 用 `tm_year >= 124`（2024 年）判定是否已同步：未同步显示 `--:--` 与 `--/--`；已同步显示 `HH:MM` 和 `MM/DD 周X`。

## 修改注意事项

- 改服务器协议时同时检查 `src/idf/App_IdfServer.cpp`、`src/idf/App_IdfCommandExecutor.cpp` 和云端 `ai_server.py`。
- 改 OTA 时要同时考虑 app 分区大小、HTTP 端口、防火墙、MD5、回滚确认。
- MQTT buffer 当前是 512B，Topic buffer 是 80B，payload 不要无节制变大。
