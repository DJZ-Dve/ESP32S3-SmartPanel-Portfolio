# 省电模式（L0 亮屏 ↔ L1 熄屏待机）

## 设计目标

智能面板大部分时间是无人交互的待机态，但当前固件 CPU 满跑、背光常亮、WiFi 主动禁用了省电模式（`esp_wifi_set_ps(WIFI_PS_NONE)`）、LVGL 16ms 周期常态刷新。本模块在不影响"喊一声/按一下立刻可用"体验的前提下，把这些常态浪费收回来。

## 状态机

只有两级：

```
L0 亮屏  ──60s 无活动 + 抑制条件全过─→  L1 熄屏待机
   ↑                                          │
   └── 按键事件 / 唤醒词命中 ─────────────────┘
```

约束：

- 屏幕**无触摸**，唯一物理输入是 GPIO3 ADC 双按键（Key1/Key2/BOTH）。
- 用户要求语音 + 按键双唤醒都保留 → I2S/AFE 必须在 L1 继续工作 → **不能进 light/deep sleep**，做"软省电"。
- 不在 UI 暴露开关，超时常量写死。

## L0 → L1 入口动作（`enterL1`）

1. 拿 LVGL mutex（`AppIdfUi::applyPowerSaveEnter` 内部 `runLocked`，超时 200ms），在 mutex 内：
   - `g_statusTimer` 周期 1s → 5s（`lv_timer_set_period`）
   - `g_aiWaveTimer` 暂停（`lv_timer_pause`）
   - `AppIdfDisplay::setBacklight(false)`（避免 SPI flush 中途熄屏残影）
2. `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`
3. **不动**：音频 / WakeWord / MQTT / NimBLE / CPU 频率

如果 LVGL mutex 拿不到（200ms 超时），下一秒 tick 重试。

## L1 → L0 退出动作（`exitL1`）

1. `AppIdfDisplay::setBacklight(true)` —— 先亮屏，立刻可见
2. `esp_wifi_set_ps(WIFI_PS_NONE)`
3. 拿 LVGL mutex（`AppIdfUi::applyPowerSaveExit`），在 mutex 内：
   - `g_statusTimer` 周期回 1s
   - `g_aiWaveTimer` 恢复
   - `lv_obj_invalidate(lv_scr_act())` 强制重画当前屏

## 唤醒源

| 源 | 文件:行号（钩子） | 行为 |
|---|---|---|
| GPIO3 ADC 按键 | `src/idf/App_IdfInput.cpp:publishEvent` | `notifyActivity()` + 若 L1 则 `exitL1()`；当 `kConsumeFirstKey=true`（默认）首键被吃掉，只亮屏不切焦 |
| 唤醒词命中 | `src/idf/App_IdfWakeWord.cpp:fetchTask`（`WAKENET_DETECTED` 分支） | `notifyActivity()` + `exitL1()`，再走原 `startWakeInteraction` 流程 |
| OTA preflight 通过 | `App_IdfOta::handlePreflightRequest`（`ok=true` 分支） | `notifyActivity()` + `exitL1()`，让用户看到即将到来的 OTA 进度页。被拒的 preflight 不唤醒，避免开放批次期间未充电设备每 5 分钟心跳被亮屏一次 |
| OTA notify | `App_IdfOta::handleNotify` | `notifyActivity()` + `exitL1()`（preflight 已唤醒时幂等），CPU 回 240MHz 和 WiFi 回 `WIFI_PS_NONE` 提升下载速度 |

其他活动事件只重置空闲计时器，不强制亮屏：

| 事件 | 钩子位置 |
|---|---|
| 录音 / 上传开始 | `App_IdfRecorder::startRecording`、`startWakeInteraction` |
| BLE 配对扫描 / 配对启动 | `App_IdfBleAircon::startPairingScan`、`startPairing` |

## 抑制进入 L1 的条件（`canEnterL1()`）

只要任一为真，本次 tick 不进 L1：

- `AppIdfRecorder::snapshot().recording / uploading / startPending`
- `AppIdfAudio::isPaEnabled()` — TTS / cue 播放期间 PA_EN（GPIO21）拉高
- `AppIdfBleAircon::getPairingState() == Scanning || Pairing`
- `AppIdfOta::isBusy()`

## 默认值（`include/Power_Config.h`）

| 常量 | 默认 | 说明 |
|---|---|---|
| `kPowerSaveEnable` | `true` | 整体开关，false 则 `start()` 直接返回、所有钩子 no-op |
| `kIdleTimeoutMs` | 60000 | 60s 无活动进入 L1 |
| `kStatusTimerSlowPeriodMs` | 5000 | L1 下 status timer 周期 |
| `kStatusTimerFastPeriodMs` | 1000 | L0 下 status timer 周期 |
| `kWifiPsModeL1` | `WIFI_PS_MIN_MODEM` | MQTT 60s keepalive 兼容 |
| `kWifiPsModeL0` | `WIFI_PS_NONE` | 与现状一致 |
| `kConsumeFirstKey` | `true` | L1 下首键只亮屏不切焦 |
| `kLvglLockTimeoutMs` | 200 | enter/exit 拿 LVGL mutex 超时 |
| `kTickPeriodMs` | 1000 | 空闲检查周期 |

## 串口诊断命令

| 命令 | 行为 |
|---|---|
| `POWER` | 打印当前状态：L0/L1、enabled、idle ms |
| `POWER L1` | 强制进 L1（用于功耗实测） |
| `POWER L0` / `POWER WAKE` | 强制退出 L1 |
| `POWER ON` | 启用省电（默认） |
| `POWER OFF` | 禁用省电（强制 L0、所有钩子 no-op） |

## 关于 esp_pm / DFS / light sleep

**本期不启用**。理由：

- 当前 CPU = 160MHz（`sdkconfig:1535-1537`），DFS 收益空间小（只有 160→80 一档）。
- I2S `I2S_CLK_SRC_DEFAULT` + AFE feed 60ms 周期，light sleep 窗口几乎为零。
- SPI LCD pclk = 80MHz 要求 APB = 80MHz，DFS 降 APB 会破坏时序。
- ESP-SR 文档无 CPU 频率下限保证，降到 80MHz 唤醒词可能漏检。
- NimBLE controller modem-sleep 当前关闭（`sdkconfig:886`）。

如关背光 + WiFi PS 后实测仍超目标，可后续考虑：

1. 启用 NimBLE controller modem-sleep。
2. 长时间（>5min）空闲关 4G（GPIO45）。
3. 在 WakeWord feed 循环显式 `esp_pm_lock_acquire(APB_FREQ_MAX)` 包裹后再开 light sleep。

## 验证

### 编译

`idf.py build` —— 通过；二进制增量约 4KB。

### 串口验证

```
python tools/serial_monitor.py --port COMx --duration 90
```

预期日志：

- 主屏静置 60s：`I (xxxx) IDF_POWER: entering L1 (idle=60003ms)` + 屏黑
- 按 Key1：`I (xxxx) IDF_POWER: exiting L1` + 屏亮（首键被吃，焦点不变）
- 喊"你好小安"：`exiting L1` + AI 屏弹出 + 录音正常

### 抑制矩阵

| 场景 | 60s 后是否进 L1 |
|---|---|
| 主屏静置 | ✓ 进 |
| 录音 / 上传中 | ✗ 不进 |
| TTS / cue 播放中（PA 拉高） | ✗ 不进 |
| BLE 配对中（Scanning / Pairing） | ✗ 不进 |
| OTA 中 | ✗ 不进 |

## 风险与回退

| 风险 | 缓解 |
|---|---|
| 关背光残影 | LVGL mutex 内关 |
| L1 下唤醒词漏检 | 实测；如有问题改 `kPowerSaveEnable=false` 或 WiFi PS 回 NONE |
| WiFi PS 切断 MQTT | DTIM × 60s keepalive 应稳；不行 `kWifiPsModeL1=WIFI_PS_NONE` |
| 退 L1 后首帧延迟 | `lv_obj_invalidate(lv_scr_act())` 强制重画 |
| 首键被吃用户不适 | `kConsumeFirstKey=false` |
| 状态机死锁 | `runLocked` 200ms 超时，下一秒重试 |

**全功能关闭**：`Power_Config.h` 改 `kPowerSaveEnable=false` 重新编译，或运行时 `power off`。

---

# 低电量自动关机（独立于 L0/L1 软省电）

L0/L1 是亮屏待机，但不会保护电池——锂电池跌到 3.0V 以下会损伤电芯。本节描述当电压降到危险区间时的关机机制。

## 阈值表

电池电压百分比线性映射 `(V-3.00)/(4.20-3.00)·100`。锂电池实际 3.0V 是放电终止电压，且 LCD+WiFi+4G 峰值瞬态压降可达 150-250mV，所以阈值不能定到 5%。

| 档位 | 触发电压 (CCV) | percent | 行为 | 退出条件 |
|------|---------|---------|------|----------|
| **NORMAL** | > 3.30V | > 25% | 无 | — |
| **WARN_25** | ≤ 3.30V | ≤ 25% | 一次性 `low_battery` cue + 顶栏红色图标常驻 | 电压 ≥ 3.36V (≈30%) |
| **COUNTDOWN_15** | ≤ 3.18V | ≤ 15% | LVGL 弹窗 30 秒倒计时 → deep sleep | charging 上升沿 或 电压 ≥ 3.24V (≈20%) 撤窗 |
| **EMERGENCY_8** | ≤ 3.10V，连续 ≥ 2 个采样 | ≤ 8% | 跳过倒计时、不让步 OTA/Recorder，立即 deep sleep | （不可逆） |

**早期开机门控**：boot 时若电压 ≤ 3.20V (≈17%) 且未充电，直接进入裸屏闪烁电池图标 6 秒 → deep sleep。期间检测到充电立即恢复正常 boot。

## 关机方式

板子**无硬件锁存掉电电路**，"关机" = `esp_deep_sleep_start()`。漏电 ~10μA，8% 残量下还能撑约 40 天。

**唤醒源**：ESP32-S3 ext1 唤醒源只支持 RTC IO (GPIO 0-21)，而 `PIN_CHARGE_DETECT=GPIO37` 不在 RTC 域，无法直接做唤醒源。改用 **timer wakeup 60s 周期**重走 `app_main` 早期门控，判断充电状态：插电 → 继续 boot，未插电 → 再次 deep sleep。

**这意味着用户不插充电器就无法手动开机**。如果硬件后续把 VBUS 接到 GPIO 0-21 任意一个 RTC IO，可改用 ext1 唤醒源，开机响应零延迟。

## 让步规则

倒计时弹窗在以下忙状态下延迟启动（最长 90s 等待）：

| 忙状态 | 查询 | 让步 |
|--------|------|------|
| OTA 写 flash | `AppIdfOta::isBusy()` | COUNTDOWN 延迟 |
| Flash guard | `AppFlashGuard::isActive()` | COUNTDOWN 延迟 |
| 录音 + 上传 | `AppIdfRecorder::isBusy()` | COUNTDOWN 延迟 |

**EMERGENCY_8 不让步**：电压物理上撑不住，立即调用 `AppIdfOta::cancel()` + `AppIdfDisplay::enterDeepSleepForLowBattery()`，OTA 中断由 app rollback 保护。

## 关键文件

- `include/Battery_Config.h` — 四档阈值 + 倒计时 + 唤醒源配置
- `src/idf/App_IdfSensors.cpp` — `PowerState` 状态机、`updatePowerStateMachine`
- `src/idf/App_IdfDisplay.cpp` — `drawLowBatteryFrame` / `runLowBatteryShutdownScreen` / `enterDeepSleepForLowBattery`
- `src/idf/App_IdfUi.cpp` — `showLowBatteryCountdown` / `hideLowBatteryCountdown`（挂 `lv_layer_top()`）
- `src/idf/App_IdfOta.cpp` — `cancel()` 紧急中止入口（设 `g_cancelRequested`，下载循环下一 chunk 退出）
- `src/idf_bootstrap.cpp` — `AppIdfLvgl::start()` 之前的早期门控

## 调试命令

| 命令 | 行为 |
|------|------|
| `BAT-INJECT=3.15` | 覆写滤波后电压 = 3.15V，触发 COUNTDOWN_15 |
| `BAT-INJECT=3.05` | 覆写 = 3.05V，连续 2 个采样后触发 EMERGENCY_8 |
| `BAT-INJECT=0` | 清除电压覆写 |
| `BAT-CHARGING=1` | 模拟充电插入，撤回弹窗状态机 |
| `BAT-CHARGING=-1` | 清除充电覆写 |

## 调整滞回 / 阈值

所有数字都在 `include/Battery_Config.h`，改完 `idf.py build` 即可。注意：

- 滞回 +5% 是为了高于 EMA 单步 `kMaxStepPerUpdate=0.03f` 的最大滑步，避免状态机抖动
- 紧急档要求 `kEmergencyConsecutiveSamples=2` 个连续采样命中，避免单次毛刺误触
- 倒计时 `kLowBatteryCountdownSec=30` 按手机行业惯例

## 风险

| 风险 | 缓解 |
|------|------|
| GPIO37 不能做 ext1 唤醒源 | timer 60s 周期苏醒检测；可接受 |
| 紧急档误触发 | EMA 平滑 + 2 次连续采样确认 |
| 倒计时期间用户找不到充电器 | 30s 已是行业惯例；如果触发太频繁说明阈值偏高 |
| deep sleep 后没充电器无法开机 | 物理限制；文档已说明 |

