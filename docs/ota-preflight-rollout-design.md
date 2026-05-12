# OTA 分批调度与 Preflight 握手设计报告

## 当前落地状态

本设计已在当前仓库落地分阶段限流闭环：

- MCU 支持在 `server/cmd/{device_id}/down` 接收 `ota_preflight`，并向 `esp32/{device_id}/ota/preflight_ack` 回复实时状态。
- MCU 正式 OTA payload 带 `request_id` 时，会校验它是否匹配最近通过的 preflight。
- 带 `request_id` 且 `force=true` 的调度 OTA 会自动开始；没有 `request_id` 的旧 OTA 通知会被 MCU 拒绝。
- MCU 保存 OTA 信息前会校验 `url`、`md5`、`size`、`version`、`request_id`；下载前会二次检查版本、充电/最低电量、录音、播放、`size > 0`、OTA 分区大小、HTTP `Content-Length` 和最终写入字节数。
- 服务端新增 `ota_batches` / `ota_jobs` 表、`POST /ota/batch`、`GET /ota/batches`、`GET /ota/jobs` 和后台 OTA 调度线程，默认每批最多创建 10 台候选设备。
- 服务端订阅 `ota/preflight_ack`、`ota/result`、`telemetry/status`、`telemetry/device_info`，并以新版本重新上线作为 `verified` 条件。
- 最新批次保持 `open`，后续上线的低版本设备会自动加入；再次创建 `/ota/batch` 会把旧开放批次标记为 `superseded`。
- 调度器默认保持单设备串行行为，同时可通过 `.env` 调整 preflight、正式 OTA 下载和 reboot verify 三个阶段的并发槽位。
- 服务端单台默认最多尝试 3 次，达到后标记 `skipped`；固件级硬失败、同批次 2 台 reboot 验证超时、同批次最近 3 台不同设备 max retry 跳过都会自动暂停批次。

当前默认并发策略仍等价于串行调度，也就是同一时间只让一个 OTA job 完整走完；批次初始粗筛大小由 `OTA_BATCH_SIZE` 控制，默认 10。后续服务器和带宽能力上来后，不需要重构协议，只需调整调度器并发参数。

## 结论

当前“MQTT 通知 + HTTP 下载 + MCU 写 OTA 分区”的基础流程是可用的，但如果直接广播给所有设备，会遇到离线设备错过通知、服务器带宽拥挤、4G 流量不可控、设备电量状态过期等问题。

建议在现有 OTA 流程上增加一层服务端 OTA 调度器：

1. 服务端先用 MySQL 进行粗筛。
2. 对候选设备发起 MQTT preflight 实时确认。
3. MCU 返回当前版本、充电状态、电量、空闲状态等实时信息。
4. 服务端只对通过 preflight 的设备定向下发 OTA。
5. 设备下载前再次本地检查充电和音频忙碌状态。
6. 服务端等待设备以新版本重新上线后，再释放并调度下一台或下一批。

这套方案比单纯 MQTT retained 广播更可控，也更适合当前带电池、4G/WiFi 混合、内部 SRAM 紧张的设备。

## 设计目标

- 避免关机或离线设备错过 OTA 后无法补升级。
- 避免大量设备同时下载固件导致 HTTP 服务、服务器公网带宽或 4G 流量拥挤。
- 只选择确实在线、版本低、正在充电或电量足够的设备升级。
- 避免数据库里的旧充电状态误导 OTA 调度。
- OTA 成功判定以“新版本重新上线”为准，而不是仅以写入成功为准。
- 服务端重启或脚本中断后，OTA 进度仍可恢复。

## 当前已有基础

当前仓库已经具备这些 OTA 基础能力：

- MQTT 订阅 OTA 通知 topic。
- 服务端推送 OTA 元数据：`version`、`size`、`md5`、`url`、`force`。
- MCU 比较 `version` 是否大于当前 app desc 版本。
- MCU 在调度 OTA 通过后通过 HTTP 下载固件。
- MCU 使用 1024B 内部 SRAM buffer 流式写 OTA 分区。
- 下载完成后进行 MD5 校验。
- 成功后切换 boot partition 并重启。
- 设备上线后上报 `fw_version`，服务端可据此更新 MySQL。

当前不足：

- 关机或离线设备会错过普通 MQTT OTA 推送。
- MySQL 中的充电状态可能过期。
- 广播 OTA 容易造成下载拥挤。
- `ota/result(success)` 只表示写入完成并准备重启，不代表新固件已经稳定运行。

## 推荐总体架构

### 服务端角色

新增或扩展一个 OTA 调度器，负责：

- 读取当前发布版本和固件元数据。
- 从 MySQL 粗筛候选设备。
- 对候选设备发送 preflight 请求。
- 等待 MCU ACK。
- 对通过检查的设备发送真正 OTA 通知。
- 跟踪每台设备的 OTA 状态、超时和重试。
- 等待设备新版本上线后释放调度槽位。

### MCU 角色

MCU 需要新增 OTA preflight 处理逻辑：

- 收到 preflight 请求后，不直接升级。
- 立即读取当前实时状态。
- 回复当前版本、设备 ID、充电状态、电量、空闲状态、内存余量等。
- 收到正式 OTA 通知后，开始下载前再次检查充电、电量和音频忙碌状态。
- 如果不满足条件，拒绝 OTA 并上报原因。

## 数据新鲜度策略

MySQL 仍然需要保留 `last_seen_at` 或类似字段。粗筛时先排除明显不在线的设备：

```sql
SELECT imei, fw_version, charging, battery_pct, last_seen_at
FROM device_info
WHERE fw_version < :target_version
  AND charging = 1
  AND battery_pct >= :min_battery_pct
  AND last_seen_at > NOW() - INTERVAL 720 SECOND
LIMIT 10;
```

这个 SQL 只是粗筛，不作为最终 OTA 依据。最终依据必须是 MCU 对 preflight 的实时 ACK。

如果设备上报间隔较长，可以把 `OTA_DEFAULT_ONLINE_WINDOW_SEC` 调整为 `2 * 上报周期 + 网络容差`。当前默认 720 秒，但仍然必须做 preflight。

## Preflight 握手协议

### 服务端下发

建议使用现有设备定向命令 topic，或者新增独立 preflight topic。示例：

- `server/cmd/{device_id}/down`
- 或 `server/ota/{device_id}/preflight`

payload：

```json
{
  "type": "ota_preflight",
  "request_id": "20260424-ota-001-0001",
  "target_version": "1.0.3",
  "min_battery_pct": 50,
  "require_charging": true
}
```

字段说明：

- `type`：消息类型，避免和普通命令混淆。
- `request_id`：本次 OTA 调度请求唯一 ID，防止旧 ACK 被误用。
- `target_version`：准备升级到的目标版本。
- `min_battery_pct`：最低电量要求。
- `require_charging`：是否必须正在充电。

### MCU 回复

建议新增上行 topic：

- `esp32/{device_id}/ota/preflight_ack`

payload：

```json
{
  "type": "ota_preflight_ack",
  "request_id": "20260424-ota-001-0001",
  "device_id": "xxxxxxxx",
  "fw_version": "1.0.2",
  "charging": true,
  "battery_pct": 78,
  "free_heap": 123456,
  "largest_internal_block": 24576,
  "recording": false,
  "playing": false,
  "ota_busy": false,
  "ok": true,
  "reason": "ready"
}
```

服务端只有在以下条件都满足时，才下发正式 OTA；MCU 在正式下载前也按同样的充电和最低电量策略二次检查：

- `request_id` 与当前任务匹配。
- `ok == true`。
- `fw_version < target_version`。
- `charging == true`。
- `battery_pct >= min_battery_pct`。
- `recording == false`。
- `playing == false`，或允许等待播放结束。
- `ota_busy == false`。
- ACK 在超时时间内收到，例如 10 到 30 秒。

## 为什么录音和播放时不建议 OTA

当前项目内部 SRAM 很紧，OTA、HTTPClient、TCP/MQTT、音频任务、WakeWord、I2S 都会争抢资源。尤其 OTA 是持续写 Flash，写入期间对 cache、任务调度和资源互斥更敏感。

建议策略：

- 正在录音：直接拒绝 OTA，等待下次调度。
- 正在 TTS 播放：可以拒绝，也可以等待最多 15 秒。
- WakeWord 正在运行：可以通过现有 `AppFlashGuard` 暂停或保护，不一定拒绝。
- 空闲、充电、网络稳定：适合 OTA。

## 正式 OTA 下发

preflight 通过后，服务端发送现有 OTA 元数据：

```json
{
  "version": "1.0.3",
  "size": 2438432,
  "md5": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
  "url": "http://<your-ota-host>:8090/firmware.bin",
  "force": true,
  "request_id": "20260424-ota-001-0001"
}
```

建议 MCU 在正式 OTA payload 中也检查 `request_id`。如果设备没有收到或没有通过对应 preflight，可以拒绝本次 OTA，避免误触发。

## 服务端状态机

服务端使用 `ota_batches` 记录当前开放升级目标，使用 `ota_jobs` 记录每台设备的推进状态，避免服务端重启或脚本中断后丢进度。

批次字段：

```text
batch_id
target_version
target_imei
firmware_size
firmware_md5
firmware_url
firmware_path
min_battery_pct
require_charging
force_update
status
superseded_at
paused_at
pause_reason
```

Job 字段：

```text
id
batch_id
device_id
target_version
firmware_size
firmware_md5
firmware_url
status
retry_count
last_error
request_id
started_at
updated_at
deadline_at
verified_at
```

推荐状态：

```text
pending
preflight_sent
preflight_ok
preflight_rejected
ota_notified
downloading
rebooting
verified
failed
timeout
skipped
superseded
```

批次本身不因为 jobs 全部进入终态而自动关闭。`ota_batches.status = open` 表示当前开放升级目标；设备离线后再上线，只要版本仍低于开放批次目标版本，并且满足充电/电量粗筛，就会被补入 `ota_jobs.pending`。当发布人创建新的 `/ota/batch` 时，旧开放批次变为 `superseded`，尚未发送正式 OTA 的旧 pending/preflight job 也会标记为 `superseded`，后续设备直接跟随最新批次。自动止损时批次会变为 `paused`，并记录 `paused_at` / `pause_reason`。

关键状态转换：

1. `pending -> preflight_sent`
   服务端对候选设备发送 preflight。

2. `preflight_sent -> preflight_ok`
   收到 MCU ACK，且实时条件满足。

3. `preflight_sent -> preflight_rejected`
   MCU 在线但不满足条件，例如未充电、正在录音、电量不足。

4. `preflight_ok -> ota_notified`
   服务端定向下发 OTA 元数据。

5. `ota_notified -> rebooting`
   收到 `ota/result(success)`。这个状态只表示写入成功并准备重启。

6. `rebooting -> verified`
   服务端收到设备重新上线，且 `fw_version == target_version`。

7. 任意中间状态 -> `pending`、`failed`、`timeout` 或 `skipped`
   可重试失败会重新入队；单台达到 `OTA_MAX_RETRY_PER_DEVICE` 后标记为 `skipped`，`last_error=max_retry:<reason>`；固件级硬失败会保留 `failed` 并暂停批次，重启验证超时保留 `timeout` 用于批次止损统计。

8. `pending/preflight_sent/preflight_ok -> cancelled`
   运维取消开放批次时，服务端只停止尚未收到正式 OTA 通知的 job。

已经进入 `ota_notified` 或 `rebooting` 的设备可能正在下载、写入或等待重启验证，不做服务端强制回退，只继续等待结果或超时。

设备状态类 preflight 拒绝（`not_charging`、`low_battery`、`battery_unknown`、`recording`、`playing`）会停在 `preflight_rejected`，等待后续上线或遥测触发重新入队，不立即反复重试，也不计入批次硬失败。

自动暂停规则：

- 任一设备上报固件级硬失败：`MD5 mismatch`、`Invalid size`、`Firmware too large`、`Verify error`、`Boot partition error`、`No partition`、`Init failed`。
- 同一批次 2 台不同设备进入 `reboot_verify_timeout`。
- 同一批次最近 3 台不同设备都因 `max_retry` 标记为 `skipped`。

暂停批次会把 `pending`、`preflight_sent`、`preflight_ok` job 标记为 `skipped`；`ota_notified` 和 `rebooting` 继续等待结果。暂停不自动恢复，修正问题后创建新 `/ota/batch`。

## 并发控制

当前服务端调度器已经支持分阶段限流，默认值仍保持一次只升级一台，方便观察现场问题。

可配置参数：

```text
OTA_BATCH_SIZE=10
OTA_MAX_PREFLIGHT_IN_FLIGHT=1
OTA_MAX_OTA_DOWNLOADS=1
OTA_MAX_REBOOT_VERIFYING=1
OTA_MAX_RETRY_PER_DEVICE=3
OTA_SCHEDULER_TICK_SEC=2
```

含义：

- `OTA_BATCH_SIZE`：一个开放批次初始最多写入多少台候选设备。
- `OTA_MAX_PREFLIGHT_IN_FLIGHT`：最多允许多少台设备处于 `preflight_sent` 或 `preflight_ok` 阶段。
- `OTA_MAX_OTA_DOWNLOADS`：最多允许多少台设备处于 `ota_notified`，也就是已经收到正式 OTA 通知并可能正在下载。
- `OTA_MAX_REBOOT_VERIFYING`：最多允许多少台设备处于 `rebooting`，也就是写入成功后等待新版本重新上线。
- `OTA_MAX_RETRY_PER_DEVICE`：单台设备最大尝试次数，默认 3。
- `OTA_SCHEDULER_TICK_SEC`：调度线程空闲时的唤醒间隔。

调度器派发新的 preflight 时，会同时参考下载槽位和 reboot verify 槽位，避免大量设备已经通过 preflight 却长时间排队等待正式下载，导致 ACK 变旧。

后续可以按网络类型或设备分组提高并发：

- 4G 设备：1 台或低并发。
- WiFi 设备：3 到 5 台。
- 同一批次失败率升高时自动暂停。

示例：服务器能力上来但仍希望保守放量时，可先配置：

```text
OTA_BATCH_SIZE=100
OTA_MAX_PREFLIGHT_IN_FLIGHT=10
OTA_MAX_OTA_DOWNLOADS=10
OTA_MAX_REBOOT_VERIFYING=100
```

释放 reboot verify 槽位、并最终判定 OTA 成功的条件应当是：

- 收到设备新版本上线消息：`fw_version == target_version`。

不要只用 `ota/result(success)` 释放，因为它发生在重启前，不能证明新固件已经跑起来。

## 超时和重试建议

推荐初始参数：

```text
preflight_ack_timeout: 10-30 秒
ota_result_timeout: 10 分钟
reboot_verify_timeout: 3 分钟
max_retry_per_device: 3
```

常见失败原因：

```text
preflight_timeout
not_charging
low_battery
recording
playing
audio_busy
http_error
download_timeout
md5_mismatch
ota_end_failed
boot_partition_failed
reboot_verify_timeout
```

这些原因要写入 MySQL 或日志，方便后续判断是网络问题、设备状态问题还是固件问题。

## 设备 ID 统一要求

当前固件 topic 主要使用 IMEI：

```text
server/ota/{imei}/notify
esp32/{imei}/ota/result
```

如果后续服务端用 CHIP ID 筛选设备，必须确保 CHIP ID 就是 MCU 实际订阅 topic 的 `device_id`。否则 MySQL 筛出来的是 CHIP ID，但 MQTT 发到 IMEI topic，会导致定向 OTA 发不到设备。

建议统一命名为 `device_id`：

- 优先 IMEI。
- IMEI 不可用时 fallback 到 MAC/CHIP ID。
- MySQL、MQTT topic、状态上报、OTA 调度器全部使用同一个 `device_id`。

## MQTT Retained 的位置

这套分批调度方案不依赖 retained message。正式 OTA 和 preflight 都不建议 retain：

- preflight 是实时状态确认，retain 会造成旧请求误触发。
- 定向 OTA 属于调度任务，也不希望设备很久后上线时执行过期任务。

如果后续需要“设备上线自动发现最新版本”，可以单独做一个 retained 的只读 manifest topic，设备订阅后只用于提示或主动查询，不直接开始 OTA。

## MCU 下载前二次检查

即使 preflight 通过，MCU 开始 OTA 前仍必须再次检查：

- 当前版本是否仍低于目标版本。
- 是否仍在充电，或电量是否仍满足要求。
- 是否正在录音。
- 是否正在播放。
- 是否已有 OTA 正在进行。
- URL、size、md5 是否合法。
- 固件大小是否小于 OTA 分区大小。

因为 preflight ACK 只能证明 ACK 那一刻状态满足条件，不能保证几秒后用户没有拔掉电源或开始交互。

## 实施优先级

### 第一阶段：服务端调度闭环

- MySQL 增加或确认 `charging`、`battery_pct`、`fw_version`、`last_seen_at`、`device_id` 字段。
- 增加 OTA job 表。
- 增加 curl 触发的 OTA batch 接口。
- 实现 MySQL 粗筛。
- 实现默认单设备串行 OTA 调度，并保留可配置并发槽位。
- 以新版本重新上线作为 `verified` 条件。

### 第二阶段：MCU preflight

- MCU 支持 `ota_preflight` 命令。
- MCU 回复 `ota_preflight_ack`。
- MCU OTA 前增加二次状态检查。
- 服务端按 ACK 决定是否下发正式 OTA。

### 第三阶段：可靠性增强

- 已支持分阶段并发数配置，后续按实际服务器带宽和失败率调参。
- 支持失败重试和暂停批次。
- 支持分组升级。
- 增加固件签名或 manifest 签名。
- 当前内置 HTTP 固件服务已切换为 `ThreadingHTTPServer`，后续可换成更可靠的静态文件服务或 CDN。

## 推荐最终流程

```text
curl 触发 OTA batch
        |
        v
服务端读取固件 size/md5/version
        |
        v
MySQL 粗筛：版本低 + 充电 + 最近在线
        |
        v
发送 ota_preflight 到候选设备
        |
        v
MCU 实时读取状态并 ACK
        |
        v
服务端判断 ACK 是否满足 OTA 条件
        |
        v
定向发送 OTA 元数据
        |
        v
MCU 二次检查状态，HTTP GET 下载固件
        |
        v
MD5 校验，写入 OTA 分区，切换 boot partition
        |
        v
MCU 上报 ota/result(success)，然后重启
        |
        v
MCU 新版本重新联网，上报 fw_version
        |
        v
服务端确认 fw_version == target_version
        |
        v
标记 verified，释放调度槽位，继续下一台
```
