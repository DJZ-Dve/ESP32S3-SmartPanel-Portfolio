# 显示与 LVGL 现状

## 当前现状

显示栈是 ESP-IDF `App_IdfDisplay` + `App_IdfLvgl`。屏幕是 ST7789P3 240x240，IDF `esp_lcd` SPI panel 初始化 ST7789P3、打开背光、设置 VCOM `0xBB=0x1D`，再启动 LVGL、注册 flush/tick/task，并加载现有 `ui_init()` 生成界面。`App_IdfInput` + `App_IdfUi` 用 GPIO3 ADC KEY1/KEY2 驱动主屏焦点、BLE 配对页、设置页和 AIScreen 手动/唤醒录音上传；`App_IdfSensors` 把 GPIO1 电池、GPIO2 温度和 GPIO37 充电状态接到 MainScreen 顶部状态栏，`App_IdfNetwork` / `App_IdfTransport` / `App_IdfCellular` 接入 WiFi/4G active 状态，`App_IdfAudio` 接入音量文本，`App_IdfBleAircon` 接入 BLE 配对页和控制帧 API，`App_IdfOta` 接入 OTA 进度/验证/失败状态页，WakeWord/VADNet 自动唤醒会更新 AIScreen。

## 显示配置

关键配置在 `Pin_Config.h` 和 `src/idf/App_IdfDisplay.cpp`：

- SPI host：`SPI2_HOST`
- SPI mode：0
- 写入频率：80MHz
- 读取频率：16MHz
- 分辨率：240x240
- GRAM 高度：320
- rotation 对齐原 240x240 可视窗口
- VCOM：写寄存器 `0xBB = 0x1D`，用于修正 ST7789P3 暗部偏蓝
- ESP-IDF 显示链路：`src/idf/App_IdfDisplay.cpp` 使用 SPI2、80MHz、RGB565、`LCD_RGB_DATA_ENDIAN_LITTLE`、反色、mirror X/Y 和 `y_gap=80` 对齐 Arduino rotation=2 的 240x240 可视窗口；`src/idf/App_IdfLvgl.cpp` 在该 panel 上注册 LVGL flush
- 背光（GPIO18，开关式）由 `AppIdfDisplay::setBacklight(bool)` 直接控制；首帧由 `enableBacklightAfterNextFrame` 在最后一段 SPI flush 完成后开启；省电模式 L1 由 `AppIdfPowerSave` 在 LVGL mutex 内关闭，并把 `g_statusTimer` 周期 1s→5s、暂停 `g_aiWaveTimer`，详见 `docs/power-save.md`

## LVGL buffer

ESP-IDF LVGL 当前使用两个 `240 * 240` 像素全屏 draw buffer（`kDrawBufferLines = kScreenHeight`，每块 ≈115 KB，共 ≈230 KB），明确分配在外置 PSRAM，等价于第一帧 / 第二帧双帧缓冲，渲染与 flush 可并行。SPI master DMA 不能直接搬 PSRAM，所以 flush 时由 CPU `memcpy` 把脏区按最多 16 行一块拷到 **2 块 ping-pong** `240 * 16 * 2` 字节内部 SRAM DMA bounce buffer 中的当前一块，然后调用 `esp_lcd_panel_draw_bitmap()` 让 SPI DMA 推屏。SPI panel io 配置 `trans_queue_depth=4`，flush 时交替使用两块 bounce buffer：chunk N+1 的 `memcpy` 与 chunk N 的 SPI DMA 自然重叠，仅当 inflight 块数追上 `kDmaBounceCount` 时才通过 `AppIdfDisplay::waitForPendingTransfers()` fence 一次再复用。`g_dmaInflightCount` 跨 flush 持久；首帧背光点亮分支在 `setBacklight(true)` 前显式 fence 一次以保证全屏已上屏。不要改成全屏内部 SRAM buffer。

## UI 结构

- `src/ui*.c` 和 `include/ui*.h` 是 LVGL/UI 生成或半生成文件。
- `include/ui_theme.h` / `src/ui_theme.c` 提供轻量主题 token 和全局样式初始化；当前支持暗色主题和“晨光清爽”亮色主题。
- `src/ui_AIScreen.c` / `include/ui_AIScreen.h` 提供 AI 界面相关对象和状态。
- AI 界面只显示顶部短状态文字（如“请说”“上传中...”“思考中...”“播报中...”“已完成”）；中间区域不再显示助手回复、命令内容或音量/表情提示，避免 1.54 英寸屏幕被长文本撑出。
- MainScreen 顶部状态栏会显示 WiFi、4G、BLE 连接状态、扬声器音量百分比和电池状态；BLE 图标根据 `AppIdfBleAircon::hasActiveConnection()` 变色，表示当前 BLE client 已连接，断开或仅保存了配对目标时为灰色；扬声器百分比来自 `AppIdfAudio::snapshot()`，与设置页音量数值保持一致。UI 当前刷新电池、充电、温度、WiFi connected/connecting、4G active/connecting、ES8311 音量和 BLE 连接状态。
- 手动切换 4G Only 或 AUTO fallback 开始 4G PPP 链路时，UI 在状态栏显示 4G active/connecting；串口 `NET` 可查看 PPP/MQTT 阶段，`NETCANCEL` 可取消当前尝试。
- 电池格数由 `IDF_Sensors` 每秒采样并缓存，`App_IdfUi` 的 LVGL timer 只读缓存刷新 MainScreen。
- MainScreen 底部 Dock 当前包含“语音 / 蓝牙(亮色主屏显示为设备) / 设置”。“蓝牙/设备”会进入 BLE 配对列表，最多扫描 10 个设备，页面用 6 行滚动窗口只显示蓝牙名称，不显示 MAC 地址；列表内 `KEY1` 短按切换，最后一项后回到第一项，`KEY1` 长按退出、`KEY2` 短按确认；配对页底部不显示按键提示文字。后台用 NimBLE central/observer 扫描，确认后连接并校验 `FFE0/FFE2`，成功后保存 NVS 目标地址。
- MainScreen 上 KEY2 长按会进入 AIScreen 并调用 `App_IdfRecorder::startRecording()`；松开 KEY2 后调用 `stopRecordingAndUpload()`，停止录音、播放 `record_stop`、上传 PCM，并在上传完成后把 AIScreen 更新为成功或错误。WakeWord 命中也会进入 AIScreen，播放 `wake_ack` 后由 VADNet 自动断句上传。上传期间 UI 只轮询 recorder 快照，不在 LVGL 回调里执行网络阻塞。
- OTA 下载、写入或 pending verify 时，`App_IdfUi` 会从 `App_IdfOta::snapshot()` 切到“固件更新”页，显示状态、进度条、版本、已写入大小和 reason。OTA 活跃期间按键不离开该页；下载失败会保留失败状态约 5 秒后回到 MainScreen，写入成功则等待 OTA 层重启。
- 设置页由 `App_IdfUi` 动态创建，包含「音量 / 网络 / 重置WiFi / 主题 / 模式 / 场景 / 固件」7 项。
- 设置页的音量、网络、主题和模式项都会打开三行弹窗：音量支持减小 10%、完成、增大 10%；网络支持 Auto/WiFi/4G 并调用 `AppIdfTransport::requestMode()`；主题支持亮色/暗色并写入 NVS namespace `ui` 的 `theme` key；**模式**支持蓝牙空调/红外/射频433 三选一，确认后调 `AppIdfAppMode::switchAndRestart`（写 NVS namespace `appmode` 的 `mode` key + 800ms 后 `esp_restart`）。无保存值时默认暗色 + BLE 模式，避免改变既有设备体验。ESP-IDF 设置页中 `KEY1` 切换设置项或弹窗选项，`KEY2` 确认；串口也可用 `THEME=LIGHT` / `THEME=DARK` / `MODE=BLE|IR|RF433`。
- 场景列表屏（`g_sceneScreen`）由 `App_IdfUi` 动态创建，进入设置页「场景」项 KEY2 时打开。屏幕标题"场景"+ 可滚动列表（20 个固定 row placeholder hide/show）+ 底部提示`KEY1 切换 KEY2 执行 长按 KEY1 返回`。空表显示居中提示「暂无场景\n用串口 SCENEADDIR 或 SCENEADD433 添加」。每行格式 `[id] type desc`，type 显示 `IR` 或 `433`。`KEY1` 短按环形切焦点（含 `lv_obj_scroll_to_view` 自动滚动），`KEY2` 短按调 `AppIdfScene::executeById` 执行；type 与当前模式不匹配时场景模块内部返回 `ESP_ERR_INVALID_STATE` 并打印日志，UI 上没有视觉反馈（MVP 范围决策，后续可加 toast）。`KEY1` 长按沿用全局 `showMainLocked` 返回主屏。

## 主题与配色

- 暗色主题保留原有深色毛玻璃和青色发光风格。
- 亮色主题使用接近白色的浅冷背景、深灰墨色文字、青绿色焦点色，尽量通过 LVGL style/token 换肤实现，不新增大位图资源。
- AI 表情屏在亮色/暗色主题下统一使用暗色视觉：深色背景、白色眼白、黑色瞳孔和青色声波/发光，避免亮色主题下眼睛语义反转。
- 亮色主屏按参考拟物风格调整：顶部保留 WiFi、4G、扬声器音量百分比和电池；中部显示大时钟、日期和一个合并的“在线 | 温度”长胶囊；底部 Dock 在亮色/暗色主题下都使用同一套当前焦点卡片横向展开、失焦收回效果。
- 配网阶段主屏状态胶囊会切到整条状态牌模式，隐藏温度段并静态显示 AP 名称或配网状态；配网完成后通过轻量宽高动画翻回“在线 | 温度”。亮色/暗色主题使用同一套状态胶囊尺寸、圆角、间距和动画参数，但保留各自的颜色、透明度和暗色质感。该效果只使用现有 LVGL 对象和动画，不新增位图资源。
- MainScreen 主入口图标改为 LVGL 轻量自绘线性图标（机器人、手机+蓝牙、滑杆设置），通过对象重染色表达未选中/选中态；原 40x40 图标资源暂保留给其它页面或功能对照，不新增大位图资源。
- 切换主题时重建 MainScreen、QRScreen、设置页、BLE 配对页和 OTA 状态页；AI 表情屏本身保持暗色视觉。重建主题屏幕时使用立即切屏后再删除旧屏幕，避免 LVGL 动画尚未接管 active screen 时删除旧 active screen 导致刷新任务崩溃。
- 设置页等列表图标仍复用现有图片资源；亮色主题下通过 LVGL image recolor 做深色/焦点色染色，不新增 Flash 图片资源。
- 新增主题文字补进 `my_font_misans_20` 裁剪字库，避免“主题 / 亮色 / 暗色”显示方框。

## 字体

- 中文主字体是 `src/my_font_misans_20.c`，对象名 `my_font_misans_20`。这是用官方 MiSans Medium 裁剪生成的 LVGL 字体，不包含完整中文字库。
- 原始字体放在 `tools/MiSans_font/MiSans/ttf/MiSans-Medium.ttf`，来源为 HyperOS/MiSans 官方下载包。重新生成字体时用 `lv_font_conv`，生成参数记录在 `src/my_font_misans_20.c` 文件头。
- 当前裁剪字符集包含原有 UI 字符，并补齐源码字符串审计发现的缺字，例如“蓝”“牙”“描”等。
- **MainScreen 时间显示注意**：日期标签的星期使用 `周一`…`周日` 形式（`App_IdfUi::updateMainClockLocked` 里写死），不要改成“星期X”——当前 MiSans 裁剪集**不包含 `星`**。需要改格式时要么改回 `周X`，要么先在 `tools/MiSans_font/` 下用 `lv_font_conv` 把 `星` 加进 `--symbols` 重新生成 `src/my_font_misans_20.c`。
- `lv_font_montserrat_*` 只用于 ASCII、数字、符号等小字（项目实际启用 10/12/14/42 px）；不要把中文提示放到 Montserrat label 上，否则会显示方框。

## 并发注意事项

- `xGuiSemaphore` 定义在 `App_IdfLvgl.cpp`。
- LVGL 调用需要注意 GUI 互斥，跨任务直接操作 UI 容易引入竞态。
- 内部 SRAM 静态栈任务 `IDF_LVGL` 调用 `lv_timer_handler()`；`App_IdfUi` 通过 `App_IdfLvgl::runLocked()` 在同一个 GUI mutex 内处理按键驱动的 LVGL 操作，串口 `DISPLAY/LCD/SCREEN` 会请求 LVGL 刷新。`IDF_LVGL` 在调用 `lv_timer_handler()` 前会确认默认显示仍有 active screen，避免 UI 重建异常把设备拖进重启循环。MainScreen 状态栏的电池/温度/WiFi/4G/active transport/音量刷新在 LVGL timer 中执行，但只读取 `App_IdfSensors`、`App_IdfNetwork`、`App_IdfCellular`、`App_IdfTransport` 和 `App_IdfAudio` 缓存，不直接做 ADC 采样或网络阻塞操作。AI 录音上传由 `IDF_Recorder` 后台任务执行，LVGL timer 只读取 recorder 快照更新 AIScreen；OTA 状态页只读取 `App_IdfOta` 快照，不执行 HTTP 下载或 Flash 写入。

## 修改注意事项

- 修改 UI 行为优先改 `src/idf/App_IdfUi.cpp`，除非明确要改生成界面结构。
- UI 图片资源分布在 `asset/` 和 `src/ui_img_*`。
- 不要在 UI 回调里做网络、BLE、文件写入等长阻塞工作。
- 新增 UI 资源要考虑 Flash 体积和 PSRAM/SRAM 占用。
- 迁移到 IDF 时继续复用 `App_IdfDisplay` 已建立的 ST7789P3 初始化、VCOM、背光和 offset 策略；不要重新引入全屏内部 SRAM buffer。
