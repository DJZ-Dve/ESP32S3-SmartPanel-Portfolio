# 分区与构建现状

## 当前官方 ESP-IDF 环境

当前项目已经移除 PlatformIO 工程入口，使用官方 ESP-IDF 构建：

- target：`esp32s3`
- 已验证 ESP-IDF：`v5.5.4`
- Flash：8MB
- PSRAM：外置 8MB Quad SPI PSRAM，`sdkconfig.defaults` 中启用 `CONFIG_SPIRAM_MODE_QUAD`，80MHz
- 文件系统镜像：`joltwallet/littlefs` 通过 `littlefs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)` 生成
- 串口监视和上传波特率：921600
- ESP-IDF console：`sdkconfig.defaults` 中把主控制台切到 ESP32-S3 USB Serial/JTAG，串口命令由 `App_IdfConsole` 处理

常用命令：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor -b 921600
```

> ⚠️ **换电脑 / 新环境 / 拉到 `sdkconfig.defaults` 改动后**：先删 `sdkconfig` 和 `sdkconfig.<board>` 再 `idf.py reconfigure`，否则 IDF 不会重新读 `sdkconfig.defaults`（已有 `sdkconfig` 优先级最高）。这一步漏做会导致新增的 Kconfig 开关（比如 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）静默失效，build 看起来成功但实际行为是旧的。

当前 ESP-IDF 默认构建编译 `src/idf_bootstrap.cpp`、`App_FlashGuard`、`src/idf/` 下的迁移模块、现有 `ui_*.c` 生成界面、LVGL shell UI、ADC 按键输入、GPIO1/GPIO2/GPIO37 传感器采样、WiFi STA/AP Web/DNS 配网 portal、4G PPP、active transport、MQTT、OTA 下载与进度 UI、服务器 TCP 协议层、ES8311/I2S 音频底座、固件短提示音、LittleFS 本地固定语音同步播放、手动/唤醒录音上传、WakeWord/VADNet、BLE 配对/控制帧迁移层、主题偏好 NVS 和轻量 UI bridge。

最近一次官方 `idf.py build` 编译通过，app 大小 `0x213030` bytes，占单个 `0x300000` OTA app slot 约 69%；app 分区剩余约 31%。同次构建会生成 `build/spiffs.bin`，并把 `board_models/srmodels.bin` 和 LittleFS 镜像加入 `idf.py flash` 写入列表。

ESP-IDF 环境通过 `src/idf_component.yml` 引入 `joltwallet/littlefs`，由 `App_IdfFilesystem` 只读挂载资源分区。分区标签仍是 `spiffs`，但镜像格式是 LittleFS；不要把这个分区改成 SPIFFS 格式，除非同步迁移所有资源烧录和读取路径。

ESP-IDF 环境通过 `components/lvgl/CMakeLists.txt` 把仓库内置 `lib/lvgl` 包装成 IDF component，组件名为 `lvgl`。ESP-SR 2.4.3 通过 `components/esp_sr_local/CMakeLists.txt` 包装成本地 IDF component，组件名为 `esp_sr_local`，只显式链接 WakeNet/VADNet/AFE 需要的预编译库。`App_IdfAdc`、ADC 按键输入和 `App_IdfSensors` 依赖 ESP-IDF 自带 `esp_adc` 组件；`App_IdfNetwork` 依赖 `esp_event`、`esp_netif`、`esp_wifi`、`esp_http_server` 和 `lwip`，用于 WiFi STA/AP/NVS/scan/Web portal；`App_IdfCellular` 和 `App_IdfTransport` 使用 `esp_driver_uart`、`esp_netif` 和 lwIP PPPoS；`App_IdfMqtt` 使用 `mqtt` component；`App_IdfOta` 使用 `esp_http_client`、`app_update` 和 `mbedtls`；`App_IdfServer` / `App_IdfRecorder` 使用 lwIP socket 和既有音频/server 封装；`App_IdfAudio` 依赖 `esp_driver_i2c`、`esp_driver_i2s` 和 `json`，用于 ES8311/I2S、固件短提示音与 LittleFS cue manifest 解析；`App_IdfWakeWord` / `App_IdfVadNet` 依赖 `esp_sr_local`；`App_IdfBleAircon` 使用 ESP-IDF 自带 `bt` component 的 NimBLE central/observer；`App_IdfCommandExecutor` 使用 ESP-IDF 自带 `json` component 的 cJSON。

## 分区表

分区文件是 `partitions_8mb_espsr.csv`：

| Name | Offset | Size | 说明 |
| --- | ---: | ---: | --- |
| `nvs` | `0x9000` | `0x5000` | NVS 配置 |
| `otadata` | `0xe000` | `0x2000` | OTA 状态 |
| `app0` | `0x10000` | `0x300000` | OTA app slot 0 |
| `app1` | `0x310000` | `0x300000` | OTA app slot 1 |
| `model` | `0x610000` | `0xA0000` | ESP-SR WakeNet + VADNet 模型 |
| `spiffs` | `0x6B0000` | `0x100000` | 只读资源（audio_cues 等），LittleFS 挂 `/littlefs` |
| `userdata` | `0x7B0000` | `0x40000` | 用户运行期数据（scenes/ir_codes），LittleFS 挂 `/userdata` |
| `coredump` | `0x7f0000` | `0x10000` | coredump |

OTA 固件必须能放进单个 `0x300000` app 分区。

**两块 LittleFS 分区分工**：
- `spiffs` (1 MB)：只读静态资源镜像。由 `src/CMakeLists.txt` 的 `littlefs_create_partition_image(spiffs ../data FLASH_IN_PROJECT)` 在构建期把 `data/audio_cues/` 等打包成 image；`idf.py flash` 会随 app 一起把整个分区**完全覆盖**。也可用 `idf.py -p PORT spiffs-flash` 单独烧资源。挂载选项 `format_if_mount_failed=false`（有 image 保底）。
- `userdata` (256 KB)：用户运行期数据。**不绑定 image，`idf.py flash` 永不覆盖**。挂载选项 `format_if_mount_failed=true`：首次烧录后分区是 0xFF，挂载时自动格化为合法 littlefs；后续启动数据已经存在则正常 mount 不格化。`scenes.json` / `ir_codes.json` 走这块分区，切模式 / 断电 / 重烧固件都保留。

LittleFS spiffs 分区当前占用 audio_cues 约 800 KB（缩到 1 MB 后剩 ~200 KB 余量），9 类 cue 共 22 个 PCM 文件。继续添加新的 cue 类或新的语音变体之前必须先做以下其中一项：(1) 删除冗余变体；(2) 用更短/更紧凑的语音重新合成现有 PCM；(3) 调整 `spiffs` / `userdata` 分区大小（需要同步改 `partitions_8mb_espsr.csv`，且必须全擦烧录，OTA 升级吃不下分区表变化）。可以用 `python3 -c "data=open('build/spiffs.bin','rb').read(); B=4096; N=len(data)//B; free=sum(1 for i in range(N) if data[i*B:(i+1)*B]==b'\xff'*B); print(f'free {free*B/1024:.1f}KB')"` 实测当前镜像剩余空间。

ESP-IDF 侧挂载点：`spiffs` → `/littlefs`，`userdata` → `/userdata`。把 Arduino `LittleFS.open("/audio_cues/...")` 代码移植为 IDF/POSIX 文件访问时，需要通过 `App_IdfFilesystem::makeResourcePath()` 拼成 `/littlefs/audio_cues/...`；运行期写盘的文件走 `/userdata/...` 路径直接 `fopen`。

**分区表升级路径**：从旧布局（spiffs 1.25 MB、无 userdata）升级到新布局必须 `idf.py erase-flash` 全擦再烧；NVS（WiFi、appmode、theme 等）会一起被擦，需要重新配置。OTA 不能在 app 层升级里改分区表。

## 语音模型烧录

官方 ESP-IDF CMake 会在 `board_models/srmodels.bin` 存在时把它加入 `idf.py flash`，写入 `model` 分区 `0x610000`；也可以用 `idf.py -p PORT model-flash` 单独烧录模型分区。

当前仓库已下载 ESP-SR 2.4.3 源码包到 `lib/esp-sr/`。模型源文件当前来自该包：

- WakeNet：`lib/esp-sr/model/wakenet_model/wn9_nihaoxiaoan_tts2/`，约 292KB，包含 `_MODEL_INFO_`、`wn9_data`、`wn9_index`。
- VADNet：`lib/esp-sr/model/vadnet_model/vadnet1_medium/`，约 288KB，包含 `_MODEL_INFO_`、`vadn1_data`、`vadn1_index`。

原来仓库根目录下单独的 `wn9_nihaoxiaoan_tts2/` 原始模型目录已移除。

使用 `python tools/generate_srmodels.py` 生成 `board_models/srmodels.bin`。脚本只打包 `wn9_nihaoxiaoan_tts2` 和 `vadnet1_medium`，并校验镜像小于当前 `model` 分区 `0xA0000`。`.gitignore` 默认忽略普通 `*.bin` 构建产物，但显式放行并跟踪 `board_models/srmodels.bin`；烧录前仍要确认本机存在该文件，否则带模型烧录会失败或设备无法加载 WakeNet/VADNet 模型。

## 当前构建裁剪

旧 Arduino 入口、模块源码、头文件和 Arduino 专属本地库已从仓库移除。当前编译边界由 `src/CMakeLists.txt` 显式列出：`src/idf_bootstrap.cpp`、`src/idf/`、`src/App_FlashGuard.cpp`、`src/ui*.c`、`src/my_font_misans_20.c` 和 `src/ui_theme.c`。IR/433/本地场景学习源码、场景 UI 文件、`App_Scene.cpp` 和 `include/App_Scene.h` 已从仓库移除。

## 依赖

当前显式依赖：

- ESP-IDF NimBLE：来自 `bt` component。
- ESP-IDF MQTT：来自 `mqtt` component。
- ESP-IDF HTTP client / OTA：来自 `esp_http_client`、`app_update` 和 `mbedtls`。
- `joltwallet/littlefs`：通过 IDF component manager 声明在 `src/idf_component.yml`。
- `espressif/cjson`、`espressif/dl_fft`、`espressif/esp-dsp`：ESP-SR 2.4.3 本地 wrapper 的显式依赖，声明在 `src/idf_component.yml`
- `esp_sr_local`：通过 `components/esp_sr_local` 本地包装 `lib/esp-sr` 中的 WakeNet/VADNet/AFE 预编译库。
- `lvgl`：通过 `components/lvgl` 本地包装 `lib/lvgl`。

ESP-IDF 默认环境在 `sdkconfig.defaults` 中启用 `CONFIG_BT_ENABLED`、`CONFIG_BT_NIMBLE_ENABLED`、central 和 observer，并关闭 Bluedroid、peripheral 和 broadcaster；MainScreen 的蓝牙配对列表依赖 observer 扫描广播源。如果本机已经生成旧的 `sdkconfig.esp32-s3-fn8-8mb`，需要确保其中同样启用了这些 NimBLE 选项，否则 IDF BLE 头文件和组件不会进入构建。

仓库在 `include/lvgl.h` 和 `include/lvgl/lvgl.h` 放置了 LVGL 转发头，显式转到内置 LVGL 8.3.11 的 `lib/lvgl/lvgl.h`。这样可以让 SquareLine 生成的 UI 稳定命中项目内置 LVGL 头文件，避免同名头文件解析到错误位置。

不要把 `lib/lvgl` 或 `lib/esp-sr` 迁回普通源码扫描路径；当前需要通过 `components/lvgl` 和 `components/esp_sr_local` 明确注册成 IDF component，避免同名头文件、重复符号或预编译库链接顺序问题。

## 修改注意事项

- 分区、app slot 大小、模型分区 offset 不要随意改。
- 如果改 OTA 版本，统一修改根 `CMakeLists.txt` 的 `PROJECT_VER`，并确认生成的 `firmware.bin` 大小仍小于 app 分区。
- 如果恢复 IR/433 构建，先做内存评估，再调整 `src/CMakeLists.txt` 和相关组件依赖。
