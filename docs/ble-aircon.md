# BLE 空调控制现状

## 当前现状

当前 BLE 空调控制由 `App_IdfBleAircon` 负责，使用 ESP-IDF NimBLE 连接目标空调并写入控制帧，已完成 BLE 扫描、配对、NVS 目标保存、连接状态上报和开关机/温度/模式/风速等控制帧写入。

关键文件：

- `include/App_IdfBleAircon.h`
- `src/idf/App_IdfBleAircon.cpp`
- `src/idf/App_IdfUi.cpp`
- `src/idf/App_IdfCommandExecutor.cpp`

## BLE 目标

当前目标地址优先来自配对流程写入的 NVS：

- NVS namespace：`ble_aircon`
- MAC key：`target_mac`
- 地址类型 key：`addr_type`
- 演示机开机自动配对名称：`ZX5B25030400166`
- 未配对时保留默认 Target MAC：`48:87:2d:c2:61:60`
- Service UUID：`0xFFE0`
- TX Characteristic UUID：`0xFFE2`（手机端→空调，固件用 write-no-response）
- RX Characteristic UUID：`0xFFE1`（空调→手机端 NOTIFY，回显固件刚写入的整帧作为应用层 ACK）
- Product code：`0x01`
- 最大帧长度：16

当前启动时只读取 NVS 目标，不做开机自动配对；进入 MainScreen 底部 Dock 的“蓝牙/设备”入口后，会由 `IDF_BLE` 工作任务后台扫描最多 10 个可连接 BLE 广播源。

配对页用 6 行滚动窗口显示结果，列表只显示蓝牙名称，不显示 MAC 地址或 RSSI，`KEY1` 短按切换列表项，到最后一项后再按会回到第一项，`KEY1` 长按退出，`KEY2` 短按确认。ESP-IDF 扫描层会把同一 MAC 的 scan response 合并回扫描结果，用于补全把完整名称放在 scan response 中的设备。确认后显示“配对中”弹窗，固件会连接选中设备并校验 `FFE0/FFE2`，成功后用新 MAC 和地址类型替换 NVS 中旧目标，并保留这个 BLE client 连接。主屏顶部 4G 右侧的 BLE 图标根据当前 client 连接状态显示：已连接为蓝色，断开或仅保存配对目标时为灰色。

2026-05-04 上板验证：演示机开机后，ESP-IDF `BLESCAN` 能扫到默认目标 `48:87:2d:c2:61:60`，广播标记包含 `FFE0`；`BLEPAIR=2` 成功连接、发现 `FFE0/FFE2`，保存该目标到 NVS 并保持连接；随后 `BLECTEMP 24` 通过 handle 29 执行 write-no-response，日志显示 `BLE air conditioner command write succeeded`。这次实测中广播名仍显示为 `BLE设备`，未显示 `ZX5B25030400166`，因此如果现场需要靠名称区分多台设备，仍应优先用扫描结果的地址、RSSI 或后续协议扩展字段确认目标。

## 支持的动作

`App_IdfBleAircon` 暴露：

- 开机/关机
- 制冷、送风、节能、睡眠模式
- 温度 18~31
- 风速 1~5
- 显示开关
- 灯光开关
- 水平/垂直摆风

串口调试命令：

串口当前支持：

- `BLE` / `BLESTATUS`：显示 BLE bridge、NimBLE stack、目标地址、连接状态、扫描结果和最近错误。
- `BLESCAN`：启动配对扫描。
- `BLEPAIR=<1..10>`：连接并校验指定扫描结果，成功后保存目标到 NVS。
- `BLECANCEL`：取消扫描或配对。
- `BLEACON`
- `BLEACOFF`
- `BLECTEMP <18~31>`
- `BLEMODE COOL|VENT|ECO|SLEEP`
- `BLEFAN <1~5>`
- `BLEDISP ON|OFF`
- `BLELIGHT ON|OFF`
- `BLESWING H|V`
- `BLEKEEP=1|0`：连接保持开关，默认 `1`，即 BLE 写入成功后保持连接不主动 disconnect；输入 `BLEKEEP=0` 后切换为写入成功后自动 disconnect，并立即释放当前连接。
- `BLEACK=1|0`：FFE1 应用层 ACK 校验开关，默认 `1`。打开时每次 FFE2 写入后必须在 800ms 内收到 FFE1 NOTIFY 回显完整原帧（含校验和、`0x0D 0x0B` 结束符）才算成功；超时或字节不一致返回 `ESP_ERR_TIMEOUT` / `ESP_ERR_INVALID_RESPONSE`，并主动释放当前连接强制下次重连。`BLEACK=0` 退回早期 fire-and-forget 行为，仅用于 A/B 调试。
- `BLEDROP`：手动释放当前 BLE 命令连接。

控制帧格式：`0x5B 0x5B` 起始、Product code `0x01`、function code、payload、累加 checksum、`0x0D 0x0B` 结束，单帧最大 16 字节。控制命令由复用型 `IDF_BLE` 工作任务执行：如果配对流程保留的连接仍有效则直接写入 `FFE2`；否则会按 NVS 目标或默认 MAC 连接、发现 `FFE0` service 内全部 characteristic 后写入。写入优先使用 characteristic 的 write-no-response 属性，否则等待 write response。

配对/重连阶段会同时发现 `FFE2` 和 `FFE1`，并对 `FFE1` 的 CCCD（`0x2902`）写 `0x0001` 启用 NOTIFY；启用成功后 `BLE_GAP_EVENT_NOTIFY_RX` 把 FFE1 收到的字节复制到内部缓冲并通过信号量通知工作任务。FFE1 是 BLE↔UART 透传桥的回传通道，空调主控 MCU 收到帧后会通过 UART 把同一帧原样回写，BLE 模块再 NOTIFY 给固件，因此用 `memcmp(echo, sent, frameLen)` 字节匹配是这台空调能拿到的最强应用层 ACK。这台空调没有独立的状态字段或失败码，匹配失败和超时同等当成发送失败处理。如果配对时 FFE1 不存在、没有 NOTIFY 属性或 CCCD 写入失败，固件只会记录告警，写入路径自动退回 fire-and-forget，不阻塞控制命令。

`writeControlFrame` 内置 1 次自动重试：连接失败、写入 rc 非 0、ACK 超时或 ACK 字节不匹配都会触发 `terminateActiveConnection`，再 sleep 150ms 之后重新走一遍 `connectTargetForControl` + 写入 + ACK 校验。第二次仍失败才把错误抛回 executor。`ESP_ERR_INVALID_ARG` / `ESP_ERR_NOT_SUPPORTED`（帧格式错误、FFE2 不可写）属于配置类错误，不会重试。重试代价：单次失败时多增加 ~150ms backoff + 一次重连开销（约 1~3 秒），换来在 RF 抖动/对端瞬时忙碌等边界场景下显著降低 `op_failed` 误报。

`App_IdfCommandExecutor` 把服务器 `control` JSON 接到业务执行路径。串口 `AICMD=<json>` 可直接验证 `aircon_ble_v1` steps，例如服务器返回的 `control` 对象会被解析后调用 `App_IdfBleAircon`。

## 内存与 WakeWord 关系

BLE 对内部 SRAM 连续块敏感。执行 BLE 命令前会检查：

- 内部 free 是否低于约 32KB。
- 最大连续块是否低于约 20KB。

如果余量不足，当前只打印告警并保持 WakeWord 继续运行，不再为了 BLE 自动停止唤醒词任务；低内存时 BLE 连接、扫描或特征解析可能更容易失败。
默认 BLE 命令写入成功后保持当前命令连接，减少连续控制时的重连开销。配对流程成功保存目标后也会保留同一个已连接 client，让后续第一条控制命令可以直接复用。保持命令连接会占用 NimBLE client 资源，也可能影响 WakeWord/RMS 稳定性；需要释放连接时可输入 `BLEKEEP=0` 切换为命令后自动 disconnect，或输入 `BLEDROP` 手动释放当前连接。
配对扫描/配对连接也走同一套 BLE 操作互斥和 WakeWord 内存保护；固件使用复用型 `IDF_BLE` 工作任务和 NimBLE host task，扫描/连接前会记录 internal free、largest block 和 PSRAM 余量，低于阈值时只打印告警并继续尝试。

## UI 与 AI 命令

`App_IdfCommandExecutor.cpp` 中保留 MCU 端最小执行协议。AI 返回 JSON 后只按 `control.protocol`、
`control.protocol_version` 和 `control.steps` 执行。MCU 不保存完整产品 prompt 或协议说明，只保留执行器、
长度/版本/参数边界校验和未知协议拒绝逻辑。

修改 AI 指令格式时需要同步检查：

- `src/idf/App_IdfCommandExecutor.cpp`
- `src/idf/App_IdfBleAircon.cpp`
- `src/idf/App_IdfUi.cpp`

服务器侧 BLE 空调 profile 已简化为单文件：

- `server-side files/products/esp32s3_ble_aircon.txt`：直接说明模型要输出的最终 MCU `command.protocol`、`command.protocol_version` 和 `command.steps` 格式。

`ai_server.py` 会对单文件 profile 的直出结果做轻量白名单校验，只允许 `aircon_ble_v1` 和 `speaker_v1` 的已知 step。

`App_IdfServer.cpp` 建连时的 `META` 会发送 `product_id=esp32s3_ble_aircon`。旧固件如果没有 `product_id`，服务端仍会按 `profile=ble_only` fallback 到这个 profile。

## 修改注意事项

- 不要在 BLE 连接期间启动会大量占用内部 SRAM 的新任务。
- 连接失败后要保证 client cleanup，避免 NimBLE client 残留。
- 如果换空调，优先通过 MainScreen 的“蓝牙”入口重新配对；如果换协议，再改 UUID/帧格式并用串口 BLE 命令逐项验证；同时确认新协议是否仍然把命令通过 FFE1 原帧回显，否则需要调整 ACK 匹配策略或改成 `BLEACK=0`。
- ACK 校验把"假成功"语音反馈（详见 `docs/audio-wakeword.md` 里 `op_failed` cue）的覆盖率显著提高；如果在距离/干扰边界出现误报，可临时 `BLEACK=0` 验证是不是本地 BLE 问题，不要直接删除校验逻辑。
