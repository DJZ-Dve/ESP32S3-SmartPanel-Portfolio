# IR、433 与本地场景当前状态

## 当前结论

当前固件已恢复 **IR 学习/回放、433（CMT2300A）学习/回放、本地场景** 三个模块，并补全了 **AI 打标签 + 语音派发 + MainScreen 独立场景按钮 + 字体补字**。运行期通过 `App_IdfAppMode` 互斥状态机三选一启动 BLE / IR / RF433 中的一个；**场景模块三种模式下都启动**：BLE 模式下注入 5 条 ROM 预制（不写盘），IR/RF433 模式从 `/littlefs/scenes.json` 加载持久条目；执行场景时按 SceneItem.type 校验当前模式。

切换模式由设置屏的「模式」项弹窗或串口 `MODE=BLE|IR|RF433` 触发，会写入 NVS namespace `appmode` 后软重启（`esp_restart`）。NimBLE 在 ESP-IDF 5.x 上热卸载有"second start 偶发 hci init failed"已知坑，所以走软重启而不是热切换；切换可视为 ~3 秒黑屏，对客户测试可接受。

## MainScreen 入口

MainScreen 底部导航按钮按当前 mode 动态显示：

- **BLE 模式**：4 个按钮（语音 / 蓝牙(设备) / 场景 / 设置），顶部状态栏显示蓝牙图标。
- **IR / RF433 模式**：3 个按钮（语音 / 场景 / 设置），蓝牙(设备)按钮 + 顶部状态栏蓝牙图标都隐藏（无意义）。`focusMainIndex` 自动跳过隐藏项；flex 容器自动重排。

短按 KEY1 循环焦点，短按 KEY2 进入对应屏，长按 KEY1 返主屏。**场景按钮**：

- BLE 模式：显示 5 条 ROM 预制（开空调、关空调、节能模式=POWER_ON+ECO+26°C+3 档风、开灯、关灯）；短按 KEY2 执行；长按 KEY2 ROM 预制不可删，播 op_failed cue 提示。
- IR / RF433 模式：显示当前模式下持久学到的场景；短按 KEY2 执行选中条目；**长按 KEY2 删除当前条目**（直接删，不二次确认；删除后播 done cue 并刷新列表）。

学习入口改为**仅语音**（说"进入场景学习"）—— 长按 KEY2 不再进学习屏。模式切换继续放在 设置→模式 弹窗里。

## 模块对照

- `include/App_IdfAppMode.h` + `src/idf/App_IdfAppMode.cpp`：模式管理器，namespace=`appmode` / key=`mode` u8（0=BLE、1=IR、2=RF433），默认 0 BLE。提供 `init/current/persist/switchAndRestart`。
- `include/App_IdfIr.h` + `src/idf/App_IdfIr.cpp`：IR RAW 学习/回放，IDF_IR task core=1 优先级=2 栈=4096 words；RMT TX/RX (5.x API)，38kHz carrier，RX DMA + IRAM 回调；学习容差 + 二次按键确认；存储 LittleFS `/littlefs/ir_codes.json`。
- `include/App_IdfRf433.h` + `src/idf/App_IdfRf433.cpp`：CMT2300A / TS3260（硅传 P2P 替代）/ MRF2300A 通用 bit-banging SPI（不是硬件 SPI），IDF_RF433 task core=1 优先级=2 栈=4096 words；GPIO ISR + portMUX_TYPE 临界区帧队列；4 种模式 IDLE/LEARN_CLOUD/LISTEN_NORMAL/SNIFF_RAW；学习走候选+聚类投票；发射 Direct TX 模式 sync HIGH T+LOW 31T，bit=1 HIGH 3T+LOW T，bit=0 HIGH T+LOW 3T，重复 8 次。学习成功推 `learnEventQueue`。`rxInit()` 用两阶段初始化：阶段 1 写 `kTs3260InitReg[0x32]`（前置模拟前端校准参数）+ `0x61=0x10`，软复位 32ms 让模拟前端基于这份配置完成自校准，进入 STBY；阶段 2 写 `kCmt2300aSData`（CMT2300A 标准运行时参数表）；最后 SLEEP→STBY 锁定参数。CMT2300A 上跑同样安全（阶段 1 被阶段 2 覆盖），TS3260 上能补回约 13dB 灵敏度。
- **TX 异步分核**：`sendCodeAsync` + `IDF_RF433_TX` worker task（core=0 优先级=5 栈=3072 words）+ 4 槽 FreeRTOS queue。`AppIdfScene::executeById` 走 async，把发射调度到 core 0 跑；console `RFSEND` 保持 sync。**必须 async**：sendCode 是 600ms `esp_rom_delay_us` + `gpio_set_level` bit-banging，如果直接跑在 LVGL task（core=1）持着 LVGL mutex 的上下文里，会被 LCD flush DMA 中断（core 1，每 33ms fire）切碎 OOK pulse，让 48-bit 码被接收端拆成 30-50 pulses 碎帧、被控设备解码 IC 拒掉；core 0 没 LVGL DMA 中断、worker 优先级也高于 LVGL，bit-banging 全程不被打扰。
- **Direct TX 寄存器组合**：`IO_SEL=0x10` + `FIFO_CTL=0xC0`（bit7=TX_DIN_EN, bit6=TX_DIN_SEL=GPIO3）+ `MODE_CTL=0x40`（GO_TX，bit6=state 6=TX；CMT2300A/TS3260 state ID: 1=SLEEP 2=STBY 3=TFS 4=RFS 5=RX 6=TX）。这三个值缺一不可，FIFO_CTL=0xC0 才会把 PA 调制源接到 GPIO3 引脚电平。诊断面：`debugReadRegs(addrs, values, count)` 走 `g_modeMutex` 在不打断 RF 任务前提下读寄存器，配合 console `RFDUMP` 命令一行验证 SPI 通信、寄存器表是否真的写进芯片。
- `include/App_IdfScene.h` + `src/idf/App_IdfScene.cpp`：场景统一表，无独立 task 纯数据层；最多 20 条持久条目（不含 BLE 预制）；存储 LittleFS `/littlefs/scenes.json`。SceneItem 字段：`{id, type(IR=0/RF433=1/BLE=2), label[48], irName[32], code433, len433, T433, presetId}`。**JSON 双写 label+desc、type+mode**（旧字段冗余，保证向前向后兼容）。`executeById` 按 type 派发：IR→`AppIdfIr::sendLearned`、RF433→`AppIdfRf433::sendCodeAsync`（投 worker queue，**不直接调 sendCode**）、BLE→`AppIdfBlePresetScenes::executePresetById`；type 不匹配当前模式或对应模块未启动返回 ESP_ERR_INVALID_STATE。新增 `listForCurrentMode()` 与 `labelsForServerMeta()`。
- `include/App_IdfBlePresetScenes.h` + `src/idf/App_IdfBlePresetScenes.cpp`：BLE 预制 ROM 表（5 条）+ 顺序执行器（每步 120ms 间隔，与 BLE executor 一致）。预制 id 段保留 `0xE0..0xEF`，持久条目 id 永远 `< 0xE0`。
- `include/App_IdfLearnFlow.h` + `src/idf/App_IdfLearnFlow.cpp`：学习状态机 `Idle→Capturing→AwaitingLabel→UploadingLabel`，常驻 `IDF_LEARN` task（core=1 优先级=2 栈=4096 words）100ms 轮询 IR/RF433 `learnEventQueue` + 倒计时（CAPTURE 30s / LABEL 45s）。落盘只发生在 `onLabelArrived` 成功后，中途取消/超时不留半成品。

## 引脚

- IR TX：GPIO16
- IR RX：GPIO15
- 433 CMT2300A / TS3260（硅传 P2P 替代）/ MRF2300A：FCSB=GPIO4 / CSB=GPIO5 / SDIO=GPIO6 / SCLK=GPIO8 / GPIO3=GPIO9（INT/数据信号）

## 串口入口

- `MODE` / `MODE=BLE|IR|RF433`
- `IR` / `IRSTATUS` / `IRLIST` / `IRCLEAR` / `IRLEARN=<name>` / `IRLEARN`（停止） / `IRSEND=<name>[,<count>]`
- `RF` / `RFSTATUS` / `RF=IDLE|LEARN|LISTEN|SNIFF` / `RFTEST` / `RFSEND=<hex>,<bits>[,<T_us>]` / `RFDUMP`（读关键寄存器对比预期，用来定位"按键没反应"是 SPI/虚焊/芯片不响应中的哪一种）
- `SCENE` / `SCENELIST` / `SCENERUN=<id>` / `SCENEDEL=<id>` / `SCENECLEAR` / `SCENEADDIR=<desc>,<ir_name>` / `SCENEADD433=<desc>,<hex>,<bits>,<T>`

## UI 入口

- **MainScreen**：3–4 个底部导航按钮（BLE 模式 4 个含蓝牙；IR/RF433 模式隐藏蓝牙），KEY1 循环焦点（自动跳过隐藏项），KEY2 进入对应屏，长按 KEY1 返主屏。
- **设置屏**：保留 6 项 —— 音量 / 网络 / 重置WiFi / 主题 / 模式 / 固件。「场景」项已删除（主屏场景按钮已是唯一入口）。「模式」弹窗三选一（蓝牙空调/红外/射频433），确认后写 NVS + 800ms 后 `esp_restart`。
- **场景屏**：标题动态显示「场景 · <当前模式中文名>」；列表数据源走 `AppIdfScene::listForCurrentMode()` 按当前模式过滤；底部 hint 按模式切换（IR/RF433 显示「长按 KEY2 删除」，BLE 显示「长按 KEY1 返回」）。短按 KEY2 执行；**IR/RF433 模式长按 KEY2 删除当前条目**（直接删，不二次确认）；BLE 模式长按 KEY2 播 op_failed cue（ROM 预制不可删）。
- **学习屏（`ui_LearningScreen` 等价）**：状态机驱动（`AppIdfLearnFlow`），200ms `lv_timer` 刷新文案 + 进度条（25/50/75/100%）+ 还剩秒数。控件：标题 / 步骤计数（"1/3" `2/3` "3/3"） / 6px 蓝色进度条 / 居中提示文字 / 底部操作 hint。
  - `Capturing`（IR 第一次按）→「请按一下要学习的遥控器按键」+ 本地 TTS `learn_press_key`
  - `Capturing`（IR 第一次后等第二次确认，由 `irFirstStageCaptured()` 标志驱动）→「请再按一次相同的按键」+ 本地 TTS `learn_press_again`
  - `Capturing`（RF433）→ 一次按键直接到下一步（聚类一次通过）
  - `AwaitingLabel`→「请说出这条信号的名称」+ 本地 TTS `learn_say_name`
  - `UploadingLabel`→「正在保存...」
  - 落盘成功播 LittleFS `done` cue 退回场景屏；失败播 `op_failed` cue。**学习中长按 KEY1 退出**（取消并退回场景屏）。
- **学习入口**：仅语音命令「进入场景学习」（云端约定下发 `{"control":{"protocol":"local_learn_v1"}}`）→ `AppIdfCommandExecutor::executeControlObject` → `AppIdfLearnFlow::startCapture()` + `AppIdfUi::showLearningScreen()` 切屏。BLE 模式下 `startCapture` 直接返回 INVALID_STATE 拒绝。无串口 / 设置屏 / 按键兜底入口。

## 存储

`idf_bootstrap.cpp` 启动期挂载**两块独立的 LittleFS 分区**：

- **`spiffs` 分区** (1MB, offset `0x6B0000`) → `/littlefs`：放只读静态资源（`audio_cues/`、字体、图片等）。由 `src/CMakeLists.txt` 的 `littlefs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)` 在构建期由 `data/` 目录打成 image，`idf.py flash` **会**用 image 覆盖整个分区。`mountResourcePartition(false)` 用 `format_if_mount_failed=false`：分区有 image 保底，不需要也不该自动格化。
- **`userdata` 分区** (256KB, offset `0x7B0000`, subtype=`littlefs`) → `/userdata`：放运行期用户数据 `scenes.json` / `ir_codes.json`。**不绑定 image，`idf.py flash` 永不覆盖**。`mountUserDataPartition()` 用 `format_if_mount_failed=true`：首次烧录后分区为 0xFF，挂载时自动格化为合法 littlefs；后续启动正常 mount 不格化。

切模式（`MODE=` 软重启）、断电硬重启、`idf.py app-flash`、`idf.py flash` 完整烧 —— 这四类操作都不再丢失用户学到的 IR / RF433 / 场景数据。

历史背景（已修复）：早期 `scenes.json` / `ir_codes.json` 也存在 `spiffs` 分区下的 `/littlefs/`，但 `FLASH_IN_PROJECT` 让每次 `idf.py flash` 把这两个文件一并抹掉，用户感知是"切模式 / 重启清空"。2026-05-11 拆出独立 userdata 分区彻底解决。

分区表升级路径：从旧布局（spiffs 1.25MB、无 userdata）升级到新布局必须 `idf.py erase-flash` 全擦再烧；NVS（WiFi、appmode 等）会一起被擦，需要重新配置。

**audio_cues 容量管理（spiffs 1.25MB）**：6 个常用 TTS cue（done / network_failed / not_understood / op_failed / settings_done / wake_ack）原本各有 3 个候选 PCM，已压成 2 候选（删 `_03.pcm`），节省 ~320KB。`data/audio_cues/manifest.json` 同步去掉 `_03` 条目。腾出空间留给场景学习 3 句新 TTS：`learn_press_key` / `learn_press_again` / `learn_say_name`（manifest 内已加空数组占位，PCM 文件需用 TTS 工具按 `format` 字段规格 16kHz/16-bit/mono/裸 PCM 合成后放入 `data/audio_cues/<cue>/<cue>_01.pcm` 并补 manifest 条目）。`AppIdfAudio::isSupportedCueName` 已扩展接受这 3 个新 cue 名。

## META 上报

`App_IdfServer.cpp:sendIdentity()` 中 META JSON 现在按当前模式动态拼接：

```
{"product_id":"esp32s3_ble_aircon","profile":"<ble|ir|rf433>",
 "capabilities":{"aircon_ble":<bool>,"ir":<bool>,"rf433":<bool>,"scenes":<bool>},
 "scene_labels":[{"id":<u8>,"label":"<utf-8>"},...],
 "pending_signal":{"kind":"ir|rf433","hash":"<hex>","raw_len":<int>?,"bits":<int>?,"T":<int>?}?,
 "idf":true}
```

- `product_id` 固定 `esp32s3_ble_aircon` 保护服务器路由；`profile` 反映当前模式；`capabilities` 对应模式置 true，`scenes` 由 `AppIdfScene::isStarted()` 决定（**BLE 模式下也为 true**，因为预制 ROM 也走 Scene 模块）。
- `scene_labels` 按当前模式过滤：BLE 模式恒为 `[]`（避免 `aircon_ble_v1` 与本地预制歧义），IR/RF433 模式列出当前 type 的持久条目。
- `pending_signal` 仅在 LearnFlow 处于 AwaitingLabel/UploadingLabel 时出现；服务器看到此字段时使用 `local_label_v1` 协议返回 label。

## 服务器协议

`server-side files/products/esp32s3_ble_aircon.txt` 末尾包含三个本地协议：

- `local_scene_v1`：当 IR/RF433 模式且 scene_labels 非空时优先匹配；返回 `{"steps":[{"type":"run_scene","scene_id":<id>}]}`，MCU 调 `AppIdfScene::executeById(id)`。
- `local_label_v1`：当 `pending_signal` 非空时使用；返回 `{"label":"<≤8汉字>"}`，MCU 调 `AppIdfLearnFlow::onLabelArrived()` 落盘。
- `local_learn_v1`：用户语音「进入场景学习」时下发 `{"control":{"protocol":"local_learn_v1"}}`；MCU 调 `AppIdfLearnFlow::startCapture()` + `AppIdfUi::showLearningScreen()` 切到学习屏。BLE 模式下 startCapture 返回 INVALID_STATE，executor 回错误，AI 端应给"当前模式不支持学习"提示。

`server-side files/ai_server.py` 在 `render_prompt()` 增加 `{{SCENE_LABELS_JSON}}` 和 `{{PENDING_SIGNAL_JSON}}` 两个 placeholder。**字体扩到 GB2312 一级（3755 字 + ASCII）后，已不再做字符白名单校验**，AI 可自由表达常用汉字 + 阿拉伯数字 + 字母。

`_sanitize_direct_mcu_result` 已把这两个协议加入白名单：`local_scene_v1` 校验 `steps` 长度=1、`type=run_scene`、`scene_id` 在 `device_meta.scene_labels` 中；`local_label_v1` 只校验 `label` 长度 ≤ 8 + `device_meta.pending_signal` 非空。校验失败会回落成 `has_command=false`，MCU 走 audio_cue 兜底语音。

## 字体子集

`src/my_font_misans_20.c` 由 `lv_font_conv` 生成，2026-05-11 已扩到 **GB2312 一级常用 3755 字 + ASCII（0x20-0x7f）+ 少量标点**，开 LVGL RLE 压缩（`lib/lv_conf.h:419 LV_USE_FONT_COMPRESSED=1`）。当前体积约 2.3MB（.c）→ ~280KB（.bin），App 分区占用 2.59MB / 3MB（剩余 18%）。

常用汉字基本不会缺字。如需扩到 GB2312 二级（生僻字 3008 字）或重生成，命令在文件第 4 行注释里。新增 UI 文案前如不确定字符，可在生成命令里加进 `--symbols` 后重 build。

## 修改注意事项

恢复 IR/433/场景后修改时常见踩坑点（已写入项目记忆）：

- **ESP-IDF 5.x RMT TX 后 RX 进 not-enable 态**：`AppIdfIr::sendRawData` 必须 `rmt_disable(rxChannel)` → transmit → `rmt_enable(rxChannel)` 包裹；`startReceiving` 加 `ESP_ERR_INVALID_STATE` 兜底重试。直接复制老 Arduino 仓库的"transmit → 立即 startReceive"会触发 `rmt_receive(401): channel not in enable state`。
- **App_Log.h 已经有 `TAG_IR` / `TAG_RF433` 宏**，模块文件不要再 `constexpr const char* TAG_IR = ...`，会和宏冲突。
- **C++23 volatile-qualified ++ deprecated**：ISR 里 `g_count++` 要改成 `g_count = g_count + 1` 或拆开。
- **bit-banging 时序受调度影响**：CMT2300A 寄存器写入用 `esp_rom_delay_us(2)` 两微秒间隔；不要在 BLE/WiFi 高负载场景下并发触发，可能导致 SPI bit 漂移。
- **互斥软重启替代热切换**：如果以后想做热切换 BLE↔IR/RF433，必须解决 NimBLE deinit 问题 + 各模块的 stop()/任务清理路径，工程量较大。
