# 音频与语音唤醒现状

## 音频链路

音频模块在 `src/idf/App_IdfAudio.cpp`，硬件是 ES8311 + NS4150B。当前全链路采样率是 16kHz，16-bit，单声道。

`App_IdfAudio` 使用 `esp_driver_i2c` 在 GPIO47/GPIO48 上探测并配置 ES8311，使用 `esp_driver_i2s` 在 I2S0 上启动 16kHz、16-bit、mono left、RX/TX 全双工，GPIO21 控制 NS4150B PA。启动顺序保持“先配置 ES8311、再启动 I2S 时钟、最后重启 DAC/解除 mute/恢复音量”，并提供串口 `AUDIO`、`AUDIOSTART`、`I2CSCAN`、`SINETEST`、`AUDIOTEST`、`AUDIORESET`、`AUDIOGEN=boot|low_battery|record_stop`、`VOLUME=`、`MICGAIN=` 和 `MIC=` 验证命令。当前 IDF 侧已迁入硬件底座、PCM 读写、同步 LittleFS 本地固定语音播放、Boot/LowBattery/RecordStop 固件短提示音播放、手动录音上传、WakeWord feed/fetch 和 VADNet 自动断句。

`App_IdfAudio::readPcm()` / `writePcm()` 封装 I2S0 RX/TX，播放、录音和 WakeWord 共享同一条硬件链路，并通过音频/I2C 互斥和 WakeWord pause/resume 交接所有权。I2S 和 ES8311 I2C 访问分别有：

- `i2sMutex`
- `i2cMutex`

ES8311 是 mono codec，ESP32-S3 I2S 流固定选择左侧 mono slot，不再按默认 stereo slot 读取后软件混合成 mono。录音上传的 raw PCM 预期大小是约 32000 bytes/s（16kHz * 16-bit * 1ch）；如果上板后唤醒/录音 RMS 接近 0，可在 `src/idf/App_IdfAudio.cpp` 中把 mono slot 从 left 切到 right 试板。

ES8311 通过 I2C 初始化后，固件会先启动 ESP32-S3 I2S `RXTX_MODE`，等 MCLK/BCLK/LRCK 真正输出后再显式重启 ES8311 DAC、解除 mute 并恢复当前音量。这个顺序用于避免 codec 在外部时钟尚未稳定时进入“寄存器正常但模拟输出无声”的状态。

WakeWord 和录音路径都使用 `AppIdfAudio::setMicGain(70)`，对应 ES8311 ADC PGA 寄存器 `0x16` 第 5 档（约 30dB）。两路使用同一档增益避免唤醒/录音切换时的增益跳变；选择 70 而不是拉满 100 是为驻极体咪头/模拟硅麦预留削顶余量并降低 WiFi/BLE 经电源耦合到模拟输入端的噪声。当前直接保存 I2S 读出的 16-bit PCM，不在固件侧做 DC 偏移去除。ES8311 ADC 数字增益寄存器 `0x17` 保持 `0xBF`，即 0dB；WakeWord AFE 软件增益保持 `1.0x`。

## 主要缓冲

- 录音 buffer：512KB，优先 PSRAM；`App_IdfRecorder` 固定把该 buffer 分配在外置 PSRAM，不回落到内部 SRAM。
- 本地提示音 PCM 由生成脚本写入 `include/audio_cues_data.h`，以 flash 常量保存在 app 中；ESP-IDF 默认固件通过 `App_IdfAudio::playGeneratedCue()` 按小块同步写 I2S，不进入播放 ring buffer。
- 本地固定语音回复放在 LittleFS 的 `data/audio_cues/`，随 `idf.py flash` 或 `idf.py spiffs-flash` 烧录，不编进 app 固件。ESP-IDF 默认固件从文件按 512B 小块同步读取并写入 I2S，不整段加载到内部 SRAM。

## 本地提示音资源

`tools/generate_audio_cues.py` 可离线生成一组统一风格的本地提示音，默认写入 `asset/audio_cues/`：

- 固件用文件：16kHz、16-bit、单声道、无 WAV 头 `.pcm`。
- 试听文件：`asset/audio_cues/preview_wav/` 下的同名 `.wav`。
- `manifest.json` 记录每个提示音的用途、优先级、时长和字节数。
- 构建用头文件：`include/audio_cues_data.h` 由同一脚本生成，固件通过 `App_IdfAudio::playGeneratedCue()` 播放。

当前保留 5 个 PROGMEM 合成提示音：

- `boot`：ES8311/I2S 启动后播放一次。
- `low_battery`：电量从 `>= 20%` 运行中下降到 `< 20%` 时播放一次；开机已经低于 20% 不立即播放，只有回到 `>= 20%` 后再次跌破才会重新触发。
- `record_stop`：停止录音后播放，等待提示音和约 100ms 静音尾部排空，并确认 PA 关闭后再上传；当前是约 370ms 的两声短促提示音，前置少量静音用于功放稳定，避免第一声被吞掉。
- `learn_capture`：学习流程进入 Capturing 时播一次（约 240ms 低-高双音），提示用户"按遥控器"。
- `learn_label`：学习流程进入 AwaitingLabel 时播一次（约 320ms 上升三音），提示用户"现在说话给信号起标签"。

`learn_capture/learn_label` 由 `App_IdfLearnFlow.cpp` 直接调 `AppIdfAudio::playGeneratedCue()` 触发；学习落盘成功播放 spiffs 现有 `done` cue，落盘失败播 `op_failed`，故无需新增 LittleFS PCM。

这些提示音和当前音频链路采样率一致，播放时不需要临时切换 I2S 采样率。固件通过 `PROGMEM` 表小块实时喂给 I2S，不把整段提示音搬入内部 SRAM。按键、网络连接/断开、BLE 配对、错误、OTA 开始/完成和 MQTT `beep` 不再播放本地提示音。

当前 WakeWord 命中后不再请求服务器 `"我在。"` TTS；Net 任务进入 AI 界面后直接播放 LittleFS 中的 `wake_ack` 本地语音，播完后额外延迟约 100ms，然后直接进入录音。录音开始提示音已经移除，`wake_ack` 是唤醒录音前唯一的音频反馈。

WakeWord 暂停、播放 `wake_ack`、录音和恢复 WakeWord 之间不再频繁 Power Down ES8311 ADC；麦克风通道尽量保持 warm，只通过暂停 feed/fetch、I2S 互斥、增益切换、I2S DMA flush、AFE buffer reset 和 WakeNet reset 来切换音频所有权。恢复 WakeWord 时先保持 feed/fetch 暂停，开启 ADC、等待稳定、reset AFE/WakeNet，再放开检测，避免恢复过程和 feed/fetch 并发。

唤醒录音使用 VADNet 自动断句：开麦后前 500ms 不做 VAD 判断；检测到语音后，连续静音约 1.5 秒会停止录音并上传。演示场景下，如果开麦后约 3 秒仍未检测到有效语音，也会停止录音并上传当前录音，避免长时间卡在录音态。VADNet 初始化失败时不回退旧 `esp_vad`，本次 AI 交互会直接结束。

## LittleFS 本地固定语音

LittleFS 资源目录：

- `data/audio_cues/manifest.json`
- `data/audio_cues/wake_ack/*.pcm`
- `data/audio_cues/settings_done/*.pcm`
- `data/audio_cues/done/*.pcm`
- `data/audio_cues/op_failed/*.pcm`
- `data/audio_cues/network_failed/*.pcm`
- `data/audio_cues/not_understood/*.pcm`
- `data/audio_cues/chitchat_unsupported/*.pcm`
- `data/audio_cues/device_unsupported/*.pcm`

每个 `.pcm` 必须是 16kHz、16-bit、单声道、无 WAV 头的 raw PCM。当前每类的中文变体：

- `wake_ack`（3 变体）：`我在`、`我在呢`、`嗯，我在`
- `settings_done`（3 变体）：`好的，已经设置完毕`、`设置好了`、`已经设置完成`
- `done`（3 变体）：`搞定了`、`好了`、`已完成`
- `op_failed`（3 变体）：`操作失败，请重试`、`没成功，请再试一次`、`命令发送失败`
- `network_failed`（3 变体）：`网络出问题了`、`连不上服务器`、`请检查网络`
- `not_understood`（3 变体）：`我没听清`、`没听明白`、`再说一次`
- `chitchat_unsupported`（2 变体）：`我不会聊天`、`我不太会闲聊`
- `device_unsupported`（2 变体）：`只能控制空调`、`我只管空调`

`chitchat_unsupported` / `device_unsupported` 只配 2 变体是为了把当前 LittleFS 占用控制在 99% 以下；继续加 cue 类必须先裁短现有变体或重新规划 `spiffs` 分区大小（见 `docs/partitions-and-build.md`）。

失败/兜底反馈在不同链路阶段自动选择：

- `op_failed`：设备命令执行失败（BLE ACK 超时、ACK 不一致、speaker 失败等），或服务器明确返回 `audio_cue=op_failed`（用户意图是空调但参数不支持，例如 35 度、自动风、定时）。固件优先权高于服务器选择：执行器探测到 `result.handled && (err != OK || !success)` 时强制覆盖服务器 `audio_cue` 播这条。
- `network_failed`：录音上传到 AI 服务器失败（连不上 socket、上传中断、响应超时、十六进制 JSON 长度异常等）。由 `App_IdfRecorder::uploadCurrentRecording` 在 `App_IdfServer::uploadPcmAndReceive` 返回非 OK 时直接播放，这一阶段服务器响应根本没回来，不经过 executor。
- `chitchat_unsupported`：服务器在 LLM prompt 里识别到闲聊/问答/问候，主动返回 `audio_cue=chitchat_unsupported`，固件按服务器指定的 cue 播放。
- `device_unsupported`：服务器识别到用户在控制非空调设备（灯、电视、音乐、导航等），主动返回 `audio_cue=device_unsupported`。
- `not_understood`：兜底用。两条触发路径：(a) 服务器在极不确定时主动返回 `audio_cue=not_understood`；(b) 上传成功、JSON 解析成功，但服务器既没下发设备命令也没指定任何 `audio_cue`，由 `App_IdfServer.cpp` 在 `executeControlJson` 完成后检查 `result.cuePlayed && result.handled` 兜底播放，避免用户得到一段静音。`Result` 结构体新增 `cuePlayed` 字段标记当前 JSON 是否真的发出过音频反馈。

MQTT 控制通道走的是同一个 `executeControlJson`，但不会触发 `not_understood` 兜底（兜底逻辑只在 `App_IdfServer` 语音路径，机机通信不需要语音应答）。

固件通过 `App_IdfAudio::playLocalCue()` 使用 manifest 和 LittleFS 路径，同步打开 PA、写约 50ms 静音、按 512B 小块向 I2S 写 PCM、尾部再写约 100ms 静音后关闭 PA；这条路径会被 `AUDIOCUE=<name>` 和 `App_IdfCommandExecutor` 的 `audio_cue` 播放调用。manifest 缺失、JSON 解析失败、文件缺失或文件为空时只跳过本地语音，不阻塞控制命令。LittleFS 本地语音播放时不再主动关闭 ES8311 麦克风 ADC；WakeWord 已暂停且播放后会 flush I2S/AFE，优先避免 ADC 反复关开造成后续唤醒不稳定。`boot`、低电量和录音结束使用 `include/audio_cues_data.h` 中的固件短提示音；串口命令是 `AUDIOGEN=boot|low_battery|record_stop`。IDF bootstrap 会在 ES8311/I2S 启动后播放 `boot`；`IDF_Sensors` 在运行中从 `>=20%` 下降到 `<20%` 时播放 `low_battery`；`AIRECSTOP` 和 Wake/VAD 自动录音结束会在上传前播放 `record_stop`。

## 播放来源和打断策略

普通本地 cue 不打断录音或测试音；这些链路忙时 cue 会跳过。`LocalCue` 和 `LocalFileCue` 本地音频都会绕过网络缓冲水线，不再因为短音频达不到历史网络缓冲阈值而卡住不响。

播放任务只在实际播放时开启 PA。每次开启 PA 前会先写约 50ms 静音让功放稳定；播放队列空闲约 800ms 后再写约 20ms 静音并拉低 `PIN_PA_EN`，避免配网 AP、WiFi STA 保活或 MQTT 心跳期间把射频/电源串扰持续放大到喇叭。显式调用 `waitPlaybackDone()` 的 LittleFS 本地语音路径会在尾部静音排空后主动请求播放任务关闭 PA，并等待 `PIN_PA_EN` 变低。`isPlaybackActive()` 不把 PA_EN 高电平本身视为播放中，避免短暂收尾窗口影响 OTA、电量保护等业务判断。

本地提示音使用专用实时路径，每次只写约 5ms PCM 到 I2S，然后按 5ms 节奏继续喂下一块。新的本地 cue 到来时只重置 cue 指针和 offset，DMA 中最多残留约一个小块。录音结束提示音使用 `playCueAndWait()`，会等待本地 cue 被播放任务写入 I2S 后再写约 100ms 静音排空 DMA，并请求关闭 PA，避免提示音尾部被上传流程截断。

ESP-IDF 默认固件不引入 1MB 异步播放 ring buffer：当前运行时不再接收服务器推送的云端 TTS PCM，服务器只返回 JSON 和 `audio_cue` 名称；`boot/low_battery/record_stop` 都是短固件 cue，`wake_ack/settings_done/done` 是 LittleFS 短 PCM 文件。IDF 侧用调用方任务同步小块写 I2S，配合 I2S mutex、WakeWord pause/resume、PA 尾部静音和 FlashGuard，避免为已移除的网络 PCM 流长期占用 1MB PSRAM 和一个额外播放任务。

串口 `AUDIORESET` / `AUDIORECOVER` 会关闭 PA、disable/enable I2S TX/RX、重启 ES8311 DAC、关闭麦克风通道并恢复当前音量。WakeWord 可用 `WWPAUSE` / `WWRESUME` 手动停启；Flash 写入和录音路径会自动暂停 WakeWord 并在安全时恢复。执行音频恢复后用 `SINETEST` 或 `AUDIOCUE=settings_done` 验证播放链路。

## ESP-IDF 录音上传与唤醒交互

`App_IdfRecorder` 用于手动 AI 语音链路和 WakeWord/VADNet 自动语音交互：

- `IDF_Recorder` 是常驻内部 SRAM 静态栈任务，栈 4096 words。
- 512KB 录音线性 buffer 固定放外置 PSRAM，录音数据是 16kHz、16-bit、mono PCM。
- `AIRECSTART` 会启动 ES8311/I2S、关闭 PA、开启麦克风 ADC、设置麦克风增益 70（与唤醒一致，约 30dB ES8311 PGA），并丢弃 3 个 1024B I2S chunk 清空开启瞬间的旧数据。
- `AIRECSTOP` 会停止录音、关闭麦克风、播放 `record_stop` 固件短提示音，然后调用 `App_IdfServer::uploadPcmAndReceive()` 上传 PCM 并执行服务器返回 JSON。
- `AIRECCANCEL` 停止录音但不上传，`AIRECUPLOAD` 可重新上传上一次已录制的 PCM。
- 录音最长约 16 秒，达到 512KB buffer 上限会自动停止并上传。
- MainScreen 上 KEY2 长按也会走同一套 IDF recorder：按下进入 AI 聆听并录音，松开后上传，上传结束后显示成功或错误。
- `App_IdfWakeWord` 检测到 WakeNet 后会暂停检测、进入 AIScreen、播放 LittleFS `wake_ack`，再启动 Wake/VAD 模式录音。
- Wake/VAD 模式会创建 `App_IdfVadNet::Detector`，前 500ms 跳过 VAD 判断；检测到语音后连续静音约 1.5 秒停止并上传，开麦后约 3 秒仍无有效语音也会结束并上传当前录音。

VADNet 初始化失败时不回退旧 `esp_vad`，本次 AI 交互会直接结束并恢复 WakeWord。

## 语音唤醒

WakeWord 在 `src/idf/App_IdfWakeWord.cpp`，VADNet 在 `src/idf/App_IdfVadNet.cpp`，使用本地 `components/esp_sr_local` 包装 ESP-SR 2.4.3 头文件和预编译库：

- `esp_srmodel_init("model")` 从 `model` 分区加载模型。
- 优先用 `ESP_WN_PREFIX` 找模型。
- 找不到时会 fallback 搜索 `wn9` 或 `nihaoxiaoan`。
- AFE 只开启 WakeNet，不启用 AEC 和旧 AFE VAD，单麦配置；录音断句由 `App_IdfVadNet` 直接调用 VADNet 完成。
- AFE 优先使用 PSRAM：`AFE_MEMORY_ALLOC_MORE_PSRAM`。
- 当前检测模式：`DET_MODE_95`。

WakeWord 使用两个内部 SRAM 静态栈任务：

- `WW_Feed`：持续从 I2S 读音频并喂给 AFE。
- `WW_Fetch`：从 AFE 取检测结果，fetch 可能阻塞，所以不能和 feed 合并成一个任务。

如果 ES8311/I2S 初始化失败导致 WakeWord 任务提前退出，任务退出前会从 TWDT 退订，避免音频硬件不可用时拖死整机或干扰网络配网调试。

## 模型文件

仓库当前已下载 ESP-SR 2.4.3 源码包到 `lib/esp-sr/`。ESP-IDF 默认构建通过 `components/esp_sr_local` 显式注册 ESP-SR 2.4.3 的 model path、WakeNet、VADNet、AFE 及其依赖库，避免直接把 `lib/esp-sr` 作为普通源码目录扫描。

当前模型源文件在 ESP-SR 包内：

- WakeNet：`lib/esp-sr/model/wakenet_model/wn9_nihaoxiaoan_tts2/`，包含 `_MODEL_INFO_`、`wn9_data`、`wn9_index`。
- VADNet：`lib/esp-sr/model/vadnet_model/vadnet1_medium/`，包含 `_MODEL_INFO_`、`vadn1_data`、`vadn1_index`。

原来仓库根目录下单独的 `wn9_nihaoxiaoan_tts2/` 原始模型目录已移除。`tools/generate_srmodels.py` 会把 WakeNet 和 VADNet 一起打包到 `board_models/srmodels.bin`；官方 ESP-IDF CMake 会在该文件存在时把它加入 `idf.py flash`，写入 `0x610000` 的 `model` 分区。

`.gitignore` 默认忽略普通 `*.bin` 构建产物，但显式放行并跟踪 `board_models/srmodels.bin`，方便换机或完整烧录时直接带上当前 WakeNet/VADNet 模型镜像。更新模型源文件后要重新运行 `python tools/generate_srmodels.py`，并确认镜像同时包含 `wn9_nihaoxiaoan_tts2` 和 `vadnet1_medium`。

注意：VADNet 是 ESP-SR 2.x 的独立 VAD 模型，不是 `*_tts2` / `*_tts3` WakeNet 的一部分。ESP-IDF 默认构建现在同时从 `model` 分区加载 `wn9_nihaoxiaoan_tts2` 和 `vadnet1_medium`；如果模型镜像缺失任意一个，Wake/VAD 自动交互会失败。

## 暂停和释放

- 本地语音播放、录音、BLE、Flash 写入等路径会暂停、停止或恢复 WakeWord。
- `AppFlashGuard` 用于保护 Flash 写入期间的音频/WakeWord 状态。
- BLE 内部 SRAM 不足时会尝试停止 WakeWord 释放任务栈。
- 省电模式 L1：WakeWord 仍**常驻**（I2S/AFE 不动），命中后在 `fetchTask` 调用 `startWakeInteraction` 之前会先 `AppIdfPowerSave::exitL1()` 亮屏，再走原录音流程。`AppIdfAudio::isPaEnabled()` 是 L1 的抑制条件之一，TTS/cue 播放期间不会进入 L1。

## 本地语音响应现状

运行时不再让服务器推送 Qwen-TTS PCM。服务器只返回十六进制 JSON、`*` 结束符、`audio_cue` 和可忽略的 EOS；MCU 收到 `*` 后停止读取响应，优先执行 `control`，再播放 `audio_cue` 对应的 LittleFS 本地语音。`App_IdfCommandExecutor` 执行顺序是先执行 `aircon_ble_v1` 或 `speaker_v1`，再播放 JSON 顶层 `audio_cue`。

云端 TTS 生成、下载、重采样和 `generate_tts_cache.py` 旧脚本已经从当前流程移除。新增或替换语音回复时，直接更新 `data/audio_cues/` 下的 `.pcm` 和 `manifest.json`，再用 `idf.py build` 验证 LittleFS 镜像。

## 修改注意事项

- 不要创建第二个 I2S 实例和当前全局 I2S 抢端口。
- 如果临时切换采样率，恢复 WakeWord 前必须回到 16kHz。
- 麦克风 ADC 开关和增益通过 ES8311 I2C 寄存器控制，改动时必须保护 `i2cMutex`。
- WakeWord 的内部 SRAM 栈和 AFE PSRAM 占用都很敏感，改动后要用 `DIAG` 看高水位。
