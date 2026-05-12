# 项目现状文档索引

这个目录用于给 AI 和开发者快速了解项目实际状态。文档只记录当前仓库已经体现出来的事实和高风险约束，不替代源码。

如果文档和源码冲突，以源码为准，并在同一次修改里更新相关文档。

## 开始前必读

- `../AGENTS.md`：仓库总规则、构建命令、提交要求。
- `docs/README.md`：本索引，根据任务类型决定继续读哪些文档。
- `docs/memory-notes.md`：ESP32-S3 内部 SRAM 紧张，涉及固件代码时优先阅读。
- `docs/partitions-and-build.md`：涉及烧录、OTA、模型分区、官方 ESP-IDF 配置时阅读。
- `docs/esp-idf-migration.md`：ESP-IDF 迁移完成记录和验证要求。

## 按任务读取

- 硬件、引脚、外设：`docs/hardware.md`
- 分区、模型烧录、构建配置：`docs/partitions-and-build.md`
- ESP-IDF 迁移完成记录：`docs/esp-idf-migration.md`
- 固件任务、队列、启动流程：`docs/firmware-architecture.md`
- 内存、任务栈、PSRAM/SRAM 注意事项：`docs/memory-notes.md`
- LVGL、显示、UI 逻辑：`docs/display-lvgl.md`
- 音频、录音、TTS、语音唤醒：`docs/audio-wakeword.md`
- WiFi、4G、MQTT、服务器、OTA：`docs/network-server-ota.md`
- OTA 分批调度、preflight 握手、充电筛选：`docs/ota-preflight-rollout-design.md`
- BLE 空调控制：`docs/ble-aircon.md`
- IR、433、本地场景的当前状态：`docs/ir-433-scene-status.md`
- 串口调试、工具脚本、常用命令：`docs/tools-and-debug.md`
- 省电模式（L0 亮屏 ↔ L1 熄屏待机）：`docs/power-save.md`

## 已有协议资料

- `docs/2026加热器通信协议.xlsx`：加热器通信协议表格。
- `docs/芯瑞物联网串口通信协议V1.8悠巡发.docx`：芯瑞物联网串口通信协议资料。

注意：这些协议资料不代表当前固件构建已启用对应功能。当前官方 ESP-IDF 默认构建启用 BLE/IR/RF433 三选一外设（运行期通过 `App_IdfAppMode` 互斥状态机切换，软重启生效）、本地场景管理、WiFi/4G/MQTT/OTA、音频录音上传、WakeWord/VADNet 和 LVGL shell UI（设置屏可切换模式 + 查看场景列表）；详见 `docs/ir-433-scene-status.md`。
