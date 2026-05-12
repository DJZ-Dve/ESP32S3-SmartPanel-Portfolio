# Repository Guidelines（仓库指南）

## 项目结构与模块划分
`src/` 是固件主代码目录，当前入口是 `src/idf_bootstrap.cpp` 的 `app_main()`，`src/idf/` 存放已迁移的 ESP-IDF 功能模块，`src/ui_*` 是 LVGL 生成界面文件。`include/` 存放对应头文件、引脚定义和配置，例如 `Pin_Config.h`。`components/` 放本地 ESP-IDF component wrapper，`tools/` 放置 Python 辅助工具，`asset/` 和 `data/` 保存音频/图片资源。`lib/` 主要是第三方依赖，除非明确升级库版本，否则不要随意修改。项目是 ESP32S3F8N8，8MB Flash、8MB PSRAM，具体 Flash 分区文件是 `partitions_8mb_espsr.csv`，有单独的语音唤醒 `model` 分区。

## 项目现状文档索引
开始修改前，优先阅读 `docs/README.md`。根据任务类型再阅读对应文档：

- 硬件、引脚、外设：`docs/hardware.md`
- 分区、模型烧录、官方 ESP-IDF 构建：`docs/partitions-and-build.md`
- 固件主流程、任务、队列：`docs/firmware-architecture.md`
- 内部 SRAM、PSRAM、任务栈风险：`docs/memory-notes.md`
- LVGL、显示、UI 逻辑：`docs/display-lvgl.md`
- 音频、录音、TTS、语音唤醒：`docs/audio-wakeword.md`
- WiFi、4G、MQTT、服务器、OTA：`docs/network-server-ota.md`
- BLE 空调控制：`docs/ble-aircon.md`
- IR、433、本地场景当前状态：`docs/ir-433-scene-status.md`
- 串口调试和 Python 工具：`docs/tools-and-debug.md`

如果文档和源码冲突，以源码为准，并在同一次修改里更新相关文档。
修改核心功能、硬件约束、分区表、网络/服务器协议、内存策略或调试流程时，必须同步更新 `docs/` 中对应的项目现状文档。

## 构建、测试与开发命令
本项目使用官方 ESP-IDF：

- `idf.py set-target esp32s3`：首次配置目标芯片。
- `idf.py build`：编译 ESP32-S3 固件，并生成 LittleFS 资源镜像。
- `idf.py -p PORT flash`：烧录 app、分区表、`board_models/srmodels.bin` 和 LittleFS 资源镜像。
- `idf.py -p PORT monitor -b 921600`：以仓库默认波特率打开串口日志。
- `python tools/serial_monitor.py --list-ports`：列出当前可用串口，连接前先确认端口号。
- `python tools/serial_monitor.py --port COMx --send DIAG --send-delay 1 --duration 5`：使用仓库自带串口监视工具查看日志并发送单条命令；默认波特率同样为 `921600`。后续调试串口时优先使用这个脚本，并注意同一时间只能有一个程序占用串口。
- `python -m pip install -r server-side files/requirements.txt`：安装服务器侧 Python 工具依赖。

## 代码检索与导航
项目已配置 clangd（`.clangd` + `idf.py build` 生成的 `build/compile_commands.json`），LSP 工具就绪可用，优先于 grep 用于符号语义查询。

- 优先用 LSP 的场景：找符号所有引用、调用链、定义、类型、实现，或判断改动 / 重命名 / 删除会影响哪些位置。可用操作 `findReferences` / `goToDefinition` / `hover` / `incomingCalls` / `outgoingCalls` / `documentSymbol` / `workspaceSymbol` / `goToImplementation` / `prepareCallHierarchy`。改函数签名、字段名、namespace 之前必须用 `findReferences` 拿到全部 callsite，**不要只靠 grep**——grep 会漏宏展开、函数指针赋值、重载解析和模板实例化。
- 仍然用 grep / find 的场景：注释、字符串、错误信息、TODO、字面量配置等纯文本搜索；按文件名 / 路径 pattern 找文件。LSP 不处理这类查询。
- 不要重复检索：同一个符号已经 `findReferences` 过的，不要再 grep 一遍兜底。

## 测试说明
仓库目前没有独立的一方测试目录，因此每次改动至少要通过 `idf.py build`。如果修改了 Python 工具，请直接运行相关脚本验证，并保留关键日志或截图。`lib/` 目录下自带的 `tests` 属于上游依赖，不作为本项目日常提交流程的一部分。若产生临时文件，记得用完之后要删除掉。

## 提交与 Pull Request 规范
最近提交记录以简短、祈使句风格为主，常见前缀有 `feat:`、`fix:`。建议一次提交只解决一个问题，并在主题中点明子系统，例如 `feat: improve IR analyzer sampling`。提交 PR 时应说明影响的硬件或工具链路、列出验证步骤、关联 issue；对了记得用中文提交。

## 服务器文件说明
配套的 Python AI Server（`ai_server.py`）部署在云端服务器，负责语音识别、AI 推理、设备指令下发与 MQTT 主题转发。具体 host / 部署路径 / 凭据由部署者自行通过本地配置注入，不进版本库。本仓库**不包含**服务器代码，仅展示嵌入式固件实现。
