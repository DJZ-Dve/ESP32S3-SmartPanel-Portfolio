#include "App_IdfConsole.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "App_FlashGuard.h"
#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfBleAircon.h"
#include "App_IdfCellular.h"
#include "App_IdfCommandExecutor.h"
#include "App_IdfDisplay.h"
#include "App_IdfFilesystem.h"
#include "App_IdfInput.h"
#include "App_IdfIr.h"
#include "App_IdfLvgl.h"
#include "App_IdfMqtt.h"
#include "App_IdfNetwork.h"
#include "App_IdfOta.h"
#include "App_IdfPowerSave.h"
#include "App_IdfRecorder.h"
#include "App_IdfRf433.h"
#include "App_IdfScene.h"
#include "App_IdfServer.h"
#include "App_IdfSensors.h"
#include "App_IdfSystem.h"
#include "App_IdfTasks.h"
#include "App_IdfTransport.h"
#include "App_IdfUi.h"
#include "App_IdfWakeWord.h"
#include "App_Log.h"
#include "Pin_Config.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

namespace AppIdfConsole {
namespace {

constexpr const char* TAG_CONSOLE = "IDF_CONSOLE";
constexpr uint32_t kConsoleTaskStackWords = 6144;
constexpr size_t kCommandBufferSize = 512;

AppIdfTasks::StaticTaskMemory g_consoleTaskMemory;
int g_usbSecondaryFd = -1;

void trimInPlace(char* text) {
    if (text == nullptr) {
        return;
    }

    char* start = text;
    while (*start != '\0' && isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }

    char* end = start + strlen(start);
    while (end > start && isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
    }
    *end = '\0';

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

void uppercaseCopy(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }

    size_t index = 0;
    while (input != nullptr && input[index] != '\0' && index + 1 < outputSize) {
        output[index] = static_cast<char>(toupper(static_cast<unsigned char>(input[index])));
        ++index;
    }
    output[index] = '\0';
}

bool equalsAny(const char* value, const char* first, const char* second = nullptr, const char* third = nullptr) {
    return strcmp(value, first) == 0 || (second != nullptr && strcmp(value, second) == 0) ||
           (third != nullptr && strcmp(value, third) == 0);
}

bool startsWith(const char* value, const char* prefix) {
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

void configureMainConsoleInput() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // 主控制台是 USB Serial/JTAG 时，必须装驱动并把 stdin 路由到 USB JTAG VFS，
    // 否则 read(STDIN_FILENO, ...) 永远读不到字节，串口只能收不能发。
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        LOG_W(TAG_CONSOLE, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(err));
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    LOG_I(TAG_CONSOLE, "USB Serial/JTAG VFS driver active for stdin");
#endif
}

void configureNonBlockingStdin() {
    const int fd = fileno(stdin);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void configureUsbSecondaryConsoleInput() {
#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    if (g_usbSecondaryFd >= 0) {
        return;
    }

    g_usbSecondaryFd = open("/dev/secondary", O_RDONLY | O_NONBLOCK);
    if (g_usbSecondaryFd < 0) {
        LOG_W(TAG_CONSOLE, "USB Serial/JTAG secondary console open failed: errno=%d", errno);
    }
#endif
}

ssize_t readConsoleChar(char* ch) {
    const ssize_t stdinReadLen = read(STDIN_FILENO, ch, 1);
    if (stdinReadLen == 1) {
        return stdinReadLen;
    }
    if (stdinReadLen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_W(TAG_CONSOLE, "stdin read failed: errno=%d", errno);
    }

    if (g_usbSecondaryFd >= 0) {
        const ssize_t usbReadLen = read(g_usbSecondaryFd, ch, 1);
        if (usbReadLen == 1) {
            return usbReadLen;
        }
        if (usbReadLen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_W(TAG_CONSOLE, "USB Serial/JTAG read failed: errno=%d", errno);
        }
    }

    return 0;
}

const char* logLevelName(int level) {
    switch (level) {
        case LOG_LEVEL_OFF:
            return "off";
        case LOG_LEVEL_ERROR:
            return "error";
        case LOG_LEVEL_WARN:
            return "warn";
        case LOG_LEVEL_INFO:
            return "info";
        case LOG_LEVEL_DEBUG:
            return "debug";
        default:
            return "unknown";
    }
}

void printPartitionLine(const char* displayName, const esp_partition_t* partition) {
    if (partition == nullptr) {
        Serial.printf("%-10s : not found\n", displayName);
        return;
    }

    Serial.printf("%-10s : label=%s type=0x%02x subtype=0x%02x offset=0x%06" PRIx32 " size=0x%06" PRIx32 "\n",
                  displayName,
                  partition->label,
                  partition->type,
                  partition->subtype,
                  partition->address,
                  partition->size);
}

void printAppInfo() {
    const esp_app_desc_t* desc = esp_app_get_description();
    Serial.println();
    Serial.println("=== ESP-IDF App ===");
    if (desc != nullptr) {
        Serial.printf("Project     : %s\n", desc->project_name);
        Serial.printf("Version     : %s\n", desc->version);
        Serial.printf("IDF         : %s\n", desc->idf_ver);
        Serial.printf("Build       : %s %s\n", desc->date, desc->time);
    } else {
        Serial.println("App desc    : unavailable");
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* nextOta = esp_ota_get_next_update_partition(nullptr);
    if (running != nullptr) {
        Serial.printf("Running     : %s @ 0x%06" PRIx32 "\n", running->label, running->address);
    }
    if (nextOta != nullptr) {
        Serial.printf("Next OTA    : %s @ 0x%06" PRIx32 "\n", nextOta->label, nextOta->address);
    }
    Serial.println("===================");
}

void printChipInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flashSize = 0;
    const esp_err_t flashErr = esp_flash_get_size(nullptr, &flashSize);

    Serial.println();
    Serial.println("=== Chip ===");
    Serial.printf("Cores       : %u\n", static_cast<unsigned>(chip.cores));
    Serial.printf("Revision    : %u\n", static_cast<unsigned>(chip.revision));
    Serial.printf("Features    : 0x%08" PRIx32 "\n", static_cast<uint32_t>(chip.features));
    if (flashErr == ESP_OK) {
        Serial.printf("Flash       : %u bytes\n", static_cast<unsigned>(flashSize));
    } else {
        Serial.printf("Flash       : unavailable (%s)\n", esp_err_to_name(flashErr));
    }
    Serial.println("============");
}

void printHeap() {
    const AppIdfSystem::HeapSnapshot heap = AppIdfSystem::getHeapSnapshot();

    Serial.println();
    Serial.println("=== Memory and Task Stack ===");
    Serial.printf("Internal free        : %u bytes\n", static_cast<unsigned>(heap.internalFree));
    Serial.printf("Internal largest     : %u bytes\n", static_cast<unsigned>(heap.internalLargest));
    Serial.printf("Internal min free    : %u bytes\n", static_cast<unsigned>(heap.internalMinimumFree));
    Serial.printf("PSRAM free           : %u bytes\n", static_cast<unsigned>(heap.psramFree));
    Serial.printf("PSRAM largest        : %u bytes\n", static_cast<unsigned>(heap.psramLargest));
    Serial.printf("PSRAM min free       : %u bytes\n", static_cast<unsigned>(heap.psramMinimumFree));
    Serial.printf("IDF_Console watermark: %u bytes\n",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_consoleTaskMemory.handle)));
    if (AppIdfLvgl::isStarted()) {
        Serial.printf("IDF_LVGL watermark   : %u bytes\n", static_cast<unsigned>(AppIdfLvgl::taskStackHighWatermark()));
    }
    if (AppIdfInput::isStarted()) {
        Serial.printf("IDF_Input watermark  : %u bytes\n", static_cast<unsigned>(AppIdfInput::taskStackHighWatermark()));
    }
    if (AppIdfSensors::isStarted()) {
        Serial.printf("IDF_Sensors watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfSensors::taskStackHighWatermark()));
    }
    if (AppIdfBleAircon::workerTaskStackHighWatermark() > 0) {
        Serial.printf("IDF_BLE watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfBleAircon::workerTaskStackHighWatermark()));
    }
    const AppIdfAudio::Snapshot audio = AppIdfAudio::snapshot();
    Serial.printf("IDF_Audio             : %s, codec=%s, last=%s\n",
                  audio.started ? "started" : "not started",
                  audio.codecFound ? "found" : "missing",
                  esp_err_to_name(audio.lastError));
    if (AppIdfNetwork::dnsTaskStackHighWatermark() > 0) {
        Serial.printf("IDF_DNS watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfNetwork::dnsTaskStackHighWatermark()));
    }
    if (AppIdfCellular::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_4G_RX watermark  : %u bytes\n",
                      static_cast<unsigned>(AppIdfCellular::taskStackHighWatermark()));
    }
    if (AppIdfTransport::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_Transport watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfTransport::taskStackHighWatermark()));
    }
    if (AppIdfRecorder::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_Recorder watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfRecorder::taskStackHighWatermark()));
    }
    if (AppIdfWakeWord::feedTaskStackHighWatermark() > 0) {
        Serial.printf("WW_Feed watermark     : %u bytes\n",
                      static_cast<unsigned>(AppIdfWakeWord::feedTaskStackHighWatermark()));
    }
    if (AppIdfWakeWord::fetchTaskStackHighWatermark() > 0) {
        Serial.printf("WW_Fetch watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfWakeWord::fetchTaskStackHighWatermark()));
    }
    Serial.println("=============================");
}

void printFilesystem() {
    Serial.println();
    Serial.println("=== LittleFS Resources ===");
    Serial.printf("Mounted     : %s\n", AppIdfFilesystem::isResourcePartitionMounted() ? "yes" : "no");
    Serial.printf("Partition   : %s\n", AppIdfFilesystem::kResourcePartitionLabel);
    Serial.printf("Base path   : %s\n", AppIdfFilesystem::kResourceBasePath);

    AppIdfFilesystem::ResourceUsage usage;
    const esp_err_t usageErr = AppIdfFilesystem::getResourceUsage(&usage);
    if (usageErr == ESP_OK) {
        Serial.printf("Total       : %u bytes\n", static_cast<unsigned>(usage.totalBytes));
        Serial.printf("Used        : %u bytes\n", static_cast<unsigned>(usage.usedBytes));
        Serial.printf("Free        : %u bytes\n", static_cast<unsigned>(usage.totalBytes - usage.usedBytes));
    } else {
        Serial.printf("Usage       : unavailable (%s)\n", esp_err_to_name(usageErr));
    }

    Serial.printf("Manifest    : %s\n",
                  AppIdfFilesystem::resourceExists("/audio_cues/manifest.json") ? "present" : "missing");
    Serial.println("==========================");
}

void printDisplay() {
    const esp_err_t err = AppIdfLvgl::isStarted() ? AppIdfLvgl::requestRefresh() : AppIdfDisplay::drawBootProbe();

    Serial.println();
    Serial.println("=== Display / LVGL ===");
    Serial.printf("Initialized : %s\n", AppIdfDisplay::isInitialized() ? "yes" : "no");
    Serial.printf("Backlight   : %s\n", AppIdfDisplay::isBacklightOn() ? "on" : "off");
    Serial.printf("LVGL        : %s\n", AppIdfLvgl::isStarted() ? "started" : "not started");
    Serial.printf("Resolution  : %dx%d\n", AppIdfDisplay::kScreenWidth, AppIdfDisplay::kScreenHeight);
    Serial.printf("Refresh     : %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    Serial.println("=====================");
}

void printPartitions() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* nextOta = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* model =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    const esp_partition_t* filesystem =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");

    Serial.println();
    Serial.println("=== Key Partitions ===");
    printPartitionLine("running", running);
    printPartitionLine("boot", boot);
    printPartitionLine("next_ota", nextOta);
    printPartitionLine("model", model);
    printPartitionLine("filesystem", filesystem);
    if (model != nullptr) {
        uint8_t header[8] = {};
        const esp_err_t err = esp_partition_read(model, 0, header, sizeof(header));
        if (err == ESP_OK) {
            Serial.printf("model hdr  : %02x %02x %02x %02x %02x %02x %02x %02x\n",
                          header[0],
                          header[1],
                          header[2],
                          header[3],
                          header[4],
                          header[5],
                          header[6],
                          header[7]);
        } else {
            Serial.printf("model hdr  : unavailable (%s)\n", esp_err_to_name(err));
        }
    }
    Serial.println("======================");
}

void printStatus() {
    Serial.println();
    Serial.println("=== IDF Migration Status ===");
    Serial.println("Stage       : pure IDF default firmware with WiFi/4G, MQTT, OTA, audio, WakeWord, BLE and UI shell");
    Serial.println("Business    : BLE control, speaker volume, local cues, manual/wake AI upload and OTA migrated");
    Serial.printf("Log level   : %s (%d)\n", logLevelName(g_LogLevel), g_LogLevel);
    Serial.printf("Flash guard : %s\n", AppFlashGuard::isActive() ? "active" : "idle");
    Serial.printf("LittleFS    : %s\n", AppIdfFilesystem::isResourcePartitionMounted() ? "mounted" : "not mounted");
    Serial.printf("Display     : %s, backlight=%s\n",
                  AppIdfDisplay::isInitialized() ? "initialized" : "not initialized",
                  AppIdfDisplay::isBacklightOn() ? "on" : "off");
    Serial.printf("LVGL        : %s\n", AppIdfLvgl::isStarted() ? "started" : "not started");
    const AppIdfAudio::Snapshot audio = AppIdfAudio::snapshot();
    Serial.printf("Audio       : %s, codec=%s, pa=%s, mic=%s, vol=%u, last=%s\n",
                  audio.started ? "started" : "not started",
                  audio.codecFound ? "found" : "missing",
                  audio.paEnabled ? "on" : "off",
                  audio.micEnabled ? "on" : "off",
                  static_cast<unsigned>(audio.volume),
                  esp_err_to_name(audio.lastError));
    const AppIdfNetwork::Snapshot network = AppIdfNetwork::snapshot();
    Serial.printf("WiFi        : %s%s, ssid=%s, ip=%s, rssi=%d, last=%s\n",
                  network.connected ? "connected" : (network.connecting ? "connecting" : "idle"),
                  network.credentialsLoaded ? "" : " (no creds)",
                  network.ssid[0] ? network.ssid : "-",
                  network.ip,
                  static_cast<int>(network.rssi),
                  esp_err_to_name(network.lastError));
    Serial.printf("WiFi portal : %s, ap=%s, ip=%s, hint=%s\n",
                  network.portalActive ? "active" : "idle",
                  network.portalSsid[0] ? network.portalSsid : "-",
                  network.portalIp,
                  network.portalHint[0] ? network.portalHint : "-");
    const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();
    Serial.printf("4G PPP      : %s, power=%s, ip=%s, csq=%d, imei=%s, last=%s\n",
                  cellular.connected ? "connected" : (cellular.dialing ? "dialing" :
                                                       (cellular.powered ? "powered" : "off")),
                  cellular.powered ? "on" : "off",
                  cellular.ip,
                  cellular.csq,
                  cellular.imei[0] ? cellular.imei : "-",
                  cellular.lastError[0] ? cellular.lastError : "-");
    const AppIdfMqtt::Snapshot mqtt = AppIdfMqtt::snapshot();
    const AppIdfTransport::Snapshot transport = AppIdfTransport::snapshot();
    Serial.printf("Transport   : mode=%s, active=%s, cellular=%s, switches=%u, reason=%s\n",
                  AppIdfTransport::netModeName(transport.mode),
                  AppIdfTransport::activeTransportName(transport.active),
                  AppIdfTransport::cellularStageName(transport.cellularStage),
                  static_cast<unsigned>(transport.switchCount),
                  transport.lastReason);
    Serial.printf("MQTT        : %s, id=%s, rx=%u, ok=%u, err=%u, last=%s\n",
                  mqtt.connected ? "connected" : (mqtt.clientStarted ? "connecting" : "idle"),
                  mqtt.deviceId[0] ? mqtt.deviceId : "-",
                  static_cast<unsigned>(mqtt.receivedCount),
                  static_cast<unsigned>(mqtt.commandOkCount),
                  static_cast<unsigned>(mqtt.commandErrorCount),
                  esp_err_to_name(mqtt.lastError));
    const AppIdfOta::Snapshot ota = AppIdfOta::snapshot();
    Serial.printf("OTA         : %s, progress=%d, version=%s, reason=%s\n",
                  ota.busy ? "busy" : (ota.pending ? "pending" : (ota.rollbackPending ? "verify" : "idle")),
                  ota.progress,
                  ota.version[0] ? ota.version : "-",
                  ota.lastReason[0] ? ota.lastReason : "-");
    const AppIdfServer::Snapshot server = AppIdfServer::snapshot();
    Serial.printf("Server      : %s:%u, probe=%s, last=%s\n",
                  server.host[0] ? server.host : "-",
                  static_cast<unsigned>(server.port),
                  server.lastProbeOk ? "ok" : "idle/fail",
                  server.lastError[0] ? server.lastError : (server.lastSummary[0] ? server.lastSummary : "-"));
    const AppIdfRecorder::Snapshot recorder = AppIdfRecorder::snapshot();
    Serial.printf("Recorder    : %s, bytes=%u, upload=%s, last=%s\n",
                  recorder.recording ? "recording" : (recorder.uploading ? "uploading" : "idle"),
                  static_cast<unsigned>(recorder.recordedBytes),
                  recorder.lastUploadOk ? "ok" : "-",
                  recorder.lastSummary[0] ? recorder.lastSummary : "-");
    const AppIdfWakeWord::Snapshot wake = AppIdfWakeWord::snapshot();
    Serial.printf("WakeWord    : %s, paused=%s, model=%s, wakes=%u, rms=%u, last=%s\n",
                  wake.running ? "running" : (wake.feedTaskResident || wake.fetchTaskResident ? "resident" : "idle"),
                  wake.paused ? "yes" : "no",
                  wake.wakeModelName[0] ? wake.wakeModelName : "-",
                  static_cast<unsigned>(wake.wakeCount),
                  static_cast<unsigned>(wake.lastRms),
                  esp_err_to_name(wake.lastError));
    Serial.printf("UI bridge   : %s, screen=%s, keys=%u\n",
                  AppIdfUi::isStarted() ? "started" : "not started",
                  AppIdfUi::activeScreenName(),
                  static_cast<unsigned>(AppIdfUi::handledKeyCount()));
    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();
    Serial.printf("Sensors     : %s, samples=%u\n",
                  AppIdfSensors::isStarted() ? "started" : "not started",
                  static_cast<unsigned>(sensors.sampleCount));
    Serial.printf("BLE bridge  : %s, stack=%s, target=%s, connected=%s\n",
                  AppIdfBleAircon::isStarted() ? "started" : "not started",
                  AppIdfBleAircon::isStackReady() ? "ready" : "idle",
                  AppIdfBleAircon::getTargetAddressString(),
                  AppIdfBleAircon::hasActiveConnection() ? "yes" : "no");
    Serial.println("============================");
    printHeap();
}

void printInput() {
    const AppIdfInput::KeyEvent last = AppIdfInput::lastEvent();

    Serial.println();
    Serial.println("=== ADC Key Input ===");
    Serial.printf("Started     : %s\n", AppIdfInput::isStarted() ? "yes" : "no");
    Serial.printf("GPIO        : %d\n", PIN_ADC_KEY);
    Serial.printf("Current raw : %d\n", AppIdfInput::lastRaw());
    Serial.printf("Current mV  : %d\n", AppIdfInput::lastMillivolts());
    Serial.printf("Active key  : %s\n", AppIdfInput::keyIdName(AppIdfInput::activeKeyId()));
    Serial.printf("Last action : %s / %s raw=%d mv=%d\n",
                  AppIdfInput::keyIdName(last.keyId),
                  AppIdfInput::keyActionName(last.action),
                  last.raw,
                  last.millivolts);
    Serial.println("=====================");
}

void printTheme() {
    Serial.println();
    Serial.println("=== UI Theme ===");
    Serial.printf("UI bridge   : %s\n", AppIdfUi::isStarted() ? "started" : "not started");
    Serial.printf("Theme       : %s\n", AppIdfUi::currentThemeName());
    Serial.println("Commands    : THEME=LIGHT or THEME=DARK");
    Serial.println("================");
}

void printAppMode() {
    const AppIdfAppMode::Mode mode = AppIdfAppMode::current();
    Serial.println();
    Serial.println("=== App Mode (BLE / IR / RF433 mutex) ===");
    Serial.printf("Current     : %s (%s)\n", AppIdfAppMode::nameAscii(mode), AppIdfAppMode::nameCn(mode));
    Serial.println("Commands    : MODE=BLE | MODE=IR | MODE=RF433  (writes NVS and restarts)");
    Serial.println("==========================================");
}

void printIr() {
    Serial.println();
    Serial.println("=== IDF IR (raw RMT learn/replay) ===");
    Serial.printf("App mode    : %s\n", AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
    Serial.printf("Module      : %s\n", AppIdfIr::isStarted() ? "started" : "not started");
    if (AppIdfIr::isStarted()) {
        Serial.printf("Learning    : %s%s%s\n", AppIdfIr::isLearning() ? "yes" : "no",
                      AppIdfIr::isLearning() && AppIdfIr::currentLearnName()[0] ? ", name=" : "",
                      AppIdfIr::isLearning() ? AppIdfIr::currentLearnName() : "");
        Serial.printf("Stack hwm   : %u bytes\n", static_cast<unsigned>(AppIdfIr::taskStackHighWatermark()));
    } else {
        Serial.println("Hint        : switch app mode via MODE=IR to start the module");
    }
    Serial.printf("RX/TX pins  : %d / %d\n", PIN_IR_RX, PIN_IR_TX);
    Serial.println("Commands    : IRLEARN=<name>, IRLEARN(stop), IRSEND=<name>[,<count>],");
    Serial.println("              IRLIST, IRCLEAR");
    Serial.println("=====================================");
}

void printIrList() {
    Serial.println();
    Serial.println("=== IR learned commands ===");
    if (!AppIdfIr::isStarted()) {
        Serial.println("(IR module not started; switch app mode via MODE=IR)");
        Serial.println("===========================");
        return;
    }
    char names[24][AppIdfIr::kMaxNameLen] = {};
    const size_t count = AppIdfIr::listLearnedNames(names, sizeof(names) / sizeof(names[0]));
    if (count == 0) {
        Serial.println("(no codes learned)");
    } else {
        for (size_t i = 0; i < count; i++) {
            Serial.printf("  %u. %s\n", static_cast<unsigned>(i + 1), names[i]);
        }
    }
    Serial.println("===========================");
}

const char* rawCommandArgument(const char* inputLine, const char* commandName);

void printRf433() {
    Serial.println();
    Serial.println("=== IDF RF433 (CMT2300A bit-bang) ===");
    Serial.printf("App mode    : %s\n", AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
    Serial.printf("Module      : %s\n", AppIdfRf433::isStarted() ? "started" : "not started");
    if (AppIdfRf433::isStarted()) {
        Serial.printf("RF mode     : %s\n", AppIdfRf433::modeNameAscii(AppIdfRf433::currentMode()));
        Serial.printf("Stack hwm   : %u bytes\n", static_cast<unsigned>(AppIdfRf433::taskStackHighWatermark()));
    } else {
        Serial.println("Hint        : switch app mode via MODE=RF433 to start the module");
    }
    Serial.printf("Pins        : FCSB=%d CSB=%d SDIO=%d SCLK=%d GPIO3=%d\n", PIN_RF_FCSB, PIN_RF_CSB, PIN_RF_SDIO,
                  PIN_RF_SCLK, PIN_RF_GPIO3);
    Serial.println("Commands    : RF=IDLE|LEARN|LISTEN|SNIFF, RFSEND=<hex>,<bits>[,<T>], RFTEST, RFDUMP");
    Serial.println("=====================================");
}

bool handleRf433Command(const char* command, const char* inputLine) {
    if (equalsAny(command, "RF", "RF433", "RFSTATUS")) {
        printRf433();
        return true;
    }
    if (equalsAny(command, "RFTEST")) {
        if (!AppIdfRf433::isStarted()) {
            Serial.println("\nRF433 module not started (use MODE=RF433 first)");
            return true;
        }
        const esp_err_t err = AppIdfRf433::sendTest();
        Serial.printf("\nRFTEST: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "RFDUMP")) {
        if (!AppIdfRf433::isStarted()) {
            Serial.println("\nRF433 module not started (use MODE=RF433 first)");
            return true;
        }
        // 选 init 时写过的关键寄存器 + 模式状态相关寄存器，验证 SPI 是否真的把
        // 寄存器表写进了 CMT2300A：若读回值与 expect 全不匹配，基本可判定 SPI
        // bit-bang 没工作，芯片仍处于上电默认 sleep 状态（GPIO3 永远没边沿）。
        const uint8_t addrs[] = {
            0x01, 0x06, 0x18, 0x22, 0x35, 0x43, 0x4F, 0x55,  // init 表里的 anchor 字节
            0x60, 0x65, 0x68, 0x69,                            // 模式/IO/中断控制
            0x61,                                              // TS3260 扩展寄存器（CMT2300A 上读回值无意义）
        };
        const uint8_t expects[] = {
            0x66, 0x14, 0x42, 0x80, 0x44, 0xD4, 0x60, 0x55,
            0x08, 0x10, 0x00, 0x00,
            0x10,
        };
        constexpr uint8_t kCount = sizeof(addrs);
        uint8_t values[kCount] = {};
        const esp_err_t err = AppIdfRf433::debugReadRegs(addrs, values, kCount);
        if (err != ESP_OK) {
            Serial.printf("\nRFDUMP failed: %s\n", esp_err_to_name(err));
            return true;
        }
        Serial.println();
        Serial.println("=== CMT2300A Register Dump ===");
        Serial.println("addr  read  expect  status   note");
        const char* notes[kCount] = {
            "CMT bank cfg",
            "CMT bank cfg",
            "Frequency bank",
            "Data Rate bank (TS3260 tweak)",
            "Data Rate bank",
            "Baseband bank",
            "Baseband bank",
            "TX bank",
            "MODE_CTL  (0x08=RX active)",
            "IO_SEL    (0x10=GPIO3 DOUT)",
            "INT_EN    (disabled)",
            "FIFO_CTL  (read mode)",
            "TS3260 extra reg",
        };
        uint8_t mismatches = 0;
        uint8_t allFF = 0;
        uint8_t all00 = 0;
        for (uint8_t i = 0; i < kCount; ++i) {
            const bool match = values[i] == expects[i];
            if (!match) ++mismatches;
            if (values[i] == 0xFF) ++allFF;
            if (values[i] == 0x00) ++all00;
            Serial.printf("0x%02X  0x%02X  0x%02X    %s  %s\n", addrs[i], values[i], expects[i],
                          match ? "OK   " : "MISS ", notes[i]);
        }
        Serial.println("------------------------------");
        if (mismatches == 0) {
            Serial.println("verdict: all match - SPI write/read OK, chip in expected state");
        } else if (allFF == kCount) {
            Serial.println("verdict: all 0xFF - SPI MISO floating (CSB/SCLK/SDIO wiring or chip not powered)");
        } else if (all00 == kCount) {
            Serial.println("verdict: all 0x00 - SPI MISO stuck low (chip not responding, check power & reset)");
        } else {
            Serial.printf("verdict: %u/%u mismatched - partial SPI failure or chip in wrong mode\n", mismatches,
                          kCount);
        }
        Serial.println("==============================");
        return true;
    }
    if (startsWith(command, "RF=")) {
        if (!AppIdfRf433::isStarted()) {
            Serial.println("\nRF433 module not started (use MODE=RF433 first)");
            return true;
        }
        const char* arg = command + 3;
        AppIdfRf433::Mode target;
        if (strcmp(arg, "IDLE") == 0) target = AppIdfRf433::Mode::Idle;
        else if (strcmp(arg, "LEARN") == 0) target = AppIdfRf433::Mode::LearnCloud;
        else if (strcmp(arg, "LISTEN") == 0) target = AppIdfRf433::Mode::ListenNormal;
        else if (strcmp(arg, "SNIFF") == 0) target = AppIdfRf433::Mode::SniffRaw;
        else {
            Serial.println("\nUsage: RF=IDLE|LEARN|LISTEN|SNIFF");
            return true;
        }
        const esp_err_t err = AppIdfRf433::setMode(target);
        Serial.printf("\nRF mode -> %s: %s\n", AppIdfRf433::modeNameAscii(target),
                      err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "RFSEND=") || startsWith(command, "RFSEND ")) {
        if (!AppIdfRf433::isStarted()) {
            Serial.println("\nRF433 module not started (use MODE=RF433 first)");
            return true;
        }
        const char* arg = rawCommandArgument(inputLine, "RFSEND");
        if (arg == nullptr || arg[0] == '\0') {
            Serial.println("\nUsage: RFSEND=<hex_code>,<bit_len>[,<T_us>]");
            return true;
        }
        // 解析 <hex>,<bitlen>[,<T>]
        char buf[80] = {};
        strncpy(buf, arg, sizeof(buf) - 1);
        char* hexStr = buf;
        char* bitStr = strchr(buf, ',');
        if (bitStr == nullptr) {
            Serial.println("\nUsage: RFSEND=<hex_code>,<bit_len>[,<T_us>]");
            return true;
        }
        *bitStr++ = '\0';
        char* tStr = strchr(bitStr, ',');
        if (tStr != nullptr) {
            *tStr++ = '\0';
        }
        const uint64_t code = strtoull(hexStr, nullptr, 16);
        const int bits = atoi(bitStr);
        const int tUs = (tStr != nullptr) ? atoi(tStr) : 0;
        if (bits <= 0 || bits > 64) {
            Serial.println("\nbit_len must be 1..64");
            return true;
        }
        const esp_err_t err = AppIdfRf433::sendCode(code, static_cast<uint8_t>(bits), static_cast<uint16_t>(tUs));
        Serial.printf("\nRFSEND 0x%llX %d-bits T=%dus: %s\n", static_cast<unsigned long long>(code), bits, tUs,
                      err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    return false;
}

void printScene() {
    Serial.println();
    Serial.println("=== IDF Scene (IR + RF433 unified) ===");
    Serial.printf("App mode    : %s\n", AppIdfAppMode::nameAscii(AppIdfAppMode::current()));
    Serial.printf("Module      : %s\n", AppIdfScene::isStarted() ? "started" : "not started");
    Serial.printf("Count       : %u (max %u)\n", static_cast<unsigned>(AppIdfScene::count()),
                  static_cast<unsigned>(AppIdfScene::kMaxScenes));
    Serial.println("Commands    : SCENELIST, SCENERUN=<id>, SCENEDEL=<id>, SCENECLEAR");
    Serial.println("              SCENEADDIR=<desc>,<ir_name>");
    Serial.println("              SCENEADD433=<desc>,<hex>,<bits>,<T>");
    Serial.println("======================================");
}

void printSceneList() {
    Serial.println();
    Serial.println("=== Scenes ===");
    if (!AppIdfScene::isStarted()) {
        Serial.println("(scene module not started; available in MODE=IR or MODE=RF433)");
        Serial.println("==============");
        return;
    }
    const size_t total = AppIdfScene::count();
    if (total == 0) {
        Serial.println("(empty)");
    } else {
        for (size_t i = 0; i < total; i++) {
            AppIdfScene::SceneItem item;
            if (!AppIdfScene::getByIndex(i, &item)) continue;
            if (item.type == AppIdfScene::SignalType::IR) {
                Serial.printf("  [%u] IR    %s -> %s\n", static_cast<unsigned>(item.id), item.label, item.irName);
            } else if (item.type == AppIdfScene::SignalType::RF433) {
                const uint32_t codeHigh = static_cast<uint32_t>(item.code433 >> 32);
                const uint32_t codeLow = static_cast<uint32_t>(item.code433);
                Serial.printf("  [%u] RF433 %s -> 0x%08lX%08lX %u-bits T=%uus\n",
                              static_cast<unsigned>(item.id), item.label,
                              static_cast<unsigned long>(codeHigh), static_cast<unsigned long>(codeLow),
                              item.len433, item.T433);
            } else {
                Serial.printf("  [%u] BLE   %s\n", static_cast<unsigned>(item.id), item.label);
            }
        }
    }
    Serial.println("==============");
}

bool handleSceneCommand(const char* command, const char* inputLine) {
    if (equalsAny(command, "SCENE", "SCENESTATUS")) {
        printScene();
        return true;
    }
    if (equalsAny(command, "SCENELIST")) {
        printSceneList();
        return true;
    }
    const bool isSceneWriteCommand = equalsAny(command, "SCENECLEAR") || startsWith(command, "SCENEADDIR=") ||
                                     startsWith(command, "SCENEADD433=") || startsWith(command, "SCENEDEL=") ||
                                     startsWith(command, "SCENERUN=");
    if (!isSceneWriteCommand) {
        return false;
    }
    if (!AppIdfScene::isStarted()) {
        Serial.println("\nScene module not started (available in MODE=IR or MODE=RF433)");
        return true;
    }
    if (equalsAny(command, "SCENECLEAR")) {
        const esp_err_t err = AppIdfScene::clearAll();
        Serial.printf("\nSCENECLEAR: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "SCENEDEL=")) {
        const char* arg = rawCommandArgument(inputLine, "SCENEDEL");
        const int id = (arg != nullptr) ? atoi(arg) : 0;
        if (id <= 0 || id > 255) {
            Serial.println("\nUsage: SCENEDEL=<id>");
            return true;
        }
        const esp_err_t err = AppIdfScene::removeById(static_cast<uint8_t>(id));
        Serial.printf("\nSCENEDEL %d: %s\n", id, err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "SCENERUN=")) {
        const char* arg = rawCommandArgument(inputLine, "SCENERUN");
        const int id = (arg != nullptr) ? atoi(arg) : 0;
        if (id <= 0 || id > 255) {
            Serial.println("\nUsage: SCENERUN=<id>");
            return true;
        }
        const esp_err_t err = AppIdfScene::executeById(static_cast<uint8_t>(id));
        Serial.printf("\nSCENERUN %d: %s\n", id, err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "SCENEADDIR=")) {
        const char* arg = rawCommandArgument(inputLine, "SCENEADDIR");
        if (arg == nullptr || arg[0] == '\0') {
            Serial.println("\nUsage: SCENEADDIR=<desc>,<ir_name>");
            return true;
        }
        char buf[128] = {};
        strncpy(buf, arg, sizeof(buf) - 1);
        char* comma = strchr(buf, ',');
        if (comma == nullptr) {
            Serial.println("\nUsage: SCENEADDIR=<desc>,<ir_name>");
            return true;
        }
        *comma++ = '\0';
        const esp_err_t err = AppIdfScene::addSceneIr(buf, comma);
        Serial.printf("\nSCENEADDIR \"%s\" -> %s: %s\n", buf, comma, err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "SCENEADD433=")) {
        const char* arg = rawCommandArgument(inputLine, "SCENEADD433");
        if (arg == nullptr || arg[0] == '\0') {
            Serial.println("\nUsage: SCENEADD433=<desc>,<hex>,<bits>,<T_us>");
            return true;
        }
        char buf[128] = {};
        strncpy(buf, arg, sizeof(buf) - 1);
        char* p1 = strchr(buf, ',');
        if (p1 == nullptr) {
            Serial.println("\nUsage: SCENEADD433=<desc>,<hex>,<bits>,<T_us>");
            return true;
        }
        *p1++ = '\0';
        char* p2 = strchr(p1, ',');
        if (p2 == nullptr) {
            Serial.println("\nUsage: SCENEADD433=<desc>,<hex>,<bits>,<T_us>");
            return true;
        }
        *p2++ = '\0';
        char* p3 = strchr(p2, ',');
        if (p3 == nullptr) {
            Serial.println("\nUsage: SCENEADD433=<desc>,<hex>,<bits>,<T_us>");
            return true;
        }
        *p3++ = '\0';
        const uint64_t code = strtoull(p1, nullptr, 16);
        const int bits = atoi(p2);
        const int tUs = atoi(p3);
        if (bits <= 0 || bits > 64) {
            Serial.println("\nbit_len must be 1..64");
            return true;
        }
        const esp_err_t err =
            AppIdfScene::addSceneRf433(buf, code, static_cast<uint8_t>(bits), static_cast<uint16_t>(tUs));
        Serial.printf("\nSCENEADD433 \"%s\" 0x%llX %d-bits T=%dus: %s\n", buf,
                      static_cast<unsigned long long>(code), bits, tUs,
                      err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    return false;
}

bool handleIrCommand(const char* command, const char* inputLine) {
    if (equalsAny(command, "IR", "IRSTATUS")) {
        printIr();
        return true;
    }
    if (equalsAny(command, "IRLIST")) {
        printIrList();
        return true;
    }
    if (equalsAny(command, "IRCLEAR")) {
        if (!AppIdfIr::isStarted()) {
            Serial.println("\nIR module not started (use MODE=IR first)");
            return true;
        }
        const esp_err_t err = AppIdfIr::clearAllLearned();
        Serial.printf("\nIRCLEAR: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "IRLEARN", "IRSTOP")) {
        if (!AppIdfIr::isStarted()) {
            Serial.println("\nIR module not started (use MODE=IR first)");
            return true;
        }
        AppIdfIr::stopLearning();
        Serial.println("\nIR learn stopped");
        return true;
    }
    if (startsWith(command, "IRLEARN=") || startsWith(command, "IRLEARN ")) {
        if (!AppIdfIr::isStarted()) {
            Serial.println("\nIR module not started (use MODE=IR first)");
            return true;
        }
        const char* name = rawCommandArgument(inputLine, "IRLEARN");
        if (name == nullptr || name[0] == '\0') {
            Serial.println("\nUsage: IRLEARN=<name>");
            return true;
        }
        const esp_err_t err = AppIdfIr::startLearning(name);
        Serial.printf("\nIRLEARN %s: %s (press the IR remote button twice to confirm)\n", name,
                      err == ESP_OK ? "armed" : esp_err_to_name(err));
        return true;
    }
    if (startsWith(command, "IRSEND=") || startsWith(command, "IRSEND ")) {
        if (!AppIdfIr::isStarted()) {
            Serial.println("\nIR module not started (use MODE=IR first)");
            return true;
        }
        const char* arg = rawCommandArgument(inputLine, "IRSEND");
        if (arg == nullptr || arg[0] == '\0') {
            Serial.println("\nUsage: IRSEND=<name>[,<count>]");
            return true;
        }
        char nameBuf[AppIdfIr::kMaxNameLen] = {};
        int repeat = 1;
        const char* comma = strchr(arg, ',');
        if (comma != nullptr) {
            const size_t nameLen = static_cast<size_t>(comma - arg);
            if (nameLen == 0 || nameLen >= sizeof(nameBuf)) {
                Serial.println("\nUsage: IRSEND=<name>[,<count>]");
                return true;
            }
            memcpy(nameBuf, arg, nameLen);
            nameBuf[nameLen] = '\0';
            repeat = atoi(comma + 1);
            if (repeat <= 0) {
                repeat = 1;
            }
        } else {
            strncpy(nameBuf, arg, sizeof(nameBuf) - 1);
        }
        const bool ok = AppIdfIr::sendLearned(nameBuf, repeat);
        Serial.printf("\nIRSEND %s x%d: %s\n", nameBuf, repeat, ok ? "ok" : "failed");
        return true;
    }
    return false;
}

bool setAppModeFromCommand(const char* command) {
    const char* arg = command + strlen("MODE=");
    AppIdfAppMode::Mode target;
    if (strcmp(arg, "BLE") == 0) {
        target = AppIdfAppMode::Mode::BLE;
    } else if (strcmp(arg, "IR") == 0) {
        target = AppIdfAppMode::Mode::IR;
    } else if (strcmp(arg, "RF433") == 0 || strcmp(arg, "433") == 0) {
        target = AppIdfAppMode::Mode::RF433;
    } else {
        return false;
    }

    Serial.printf("\nSwitching app mode to %s and restarting...\n", AppIdfAppMode::nameAscii(target));
    fflush(stdout);
    const esp_err_t err = AppIdfAppMode::switchAndRestart(target, 500);
    // switchAndRestart 不会返回（除非 NVS 写入失败）
    if (err != ESP_OK) {
        Serial.printf("MODE switch failed: %s\n", esp_err_to_name(err));
    }
    return true;
}

void printSensors() {
    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();

    Serial.println();
    Serial.println("=== IDF Sensors ===");
    Serial.printf("Started     : %s\n", AppIdfSensors::isStarted() ? "yes" : "no");
    Serial.printf("Samples     : %u\n", static_cast<unsigned>(sensors.sampleCount));
    Serial.printf("Updated(ms) : %u\n", static_cast<unsigned>(sensors.updatedMs));
    Serial.printf("Battery GPIO: %d\n", PIN_ADC_BAT);
    Serial.printf("BAT raw     : %d\n", sensors.battery.raw);
    Serial.printf("BAT adc(mV) : %d measured, %d calibrated\n",
                  sensors.battery.adcMillivolts,
                  sensors.battery.calibratedAdcMillivolts);
    Serial.printf("BAT voltage : %.2f V\n", sensors.battery.voltage);
    Serial.printf("BAT percent : %d%%\n", sensors.battery.percent);
    Serial.printf("Charging    : %s\n", sensors.battery.charging ? "yes" : "no");
    Serial.printf("BAT status  : %s\n", sensors.battery.lastError == ESP_OK ? "ok" : esp_err_to_name(sensors.battery.lastError));
    Serial.printf("Temp GPIO   : %d\n", PIN_ADC_TEMP);
    Serial.printf("TEMP raw    : %d\n", sensors.temperature.raw);
    Serial.printf("TEMP mV     : %d\n", sensors.temperature.millivolts);
    if (sensors.temperature.valid) {
        Serial.printf("TEMP C      : %.1f\n", sensors.temperature.celsius);
    } else {
        Serial.println("TEMP C      : invalid");
    }
    Serial.printf("TEMP status : %s\n",
                  sensors.temperature.lastError == ESP_OK ? "ok" : esp_err_to_name(sensors.temperature.lastError));
    Serial.println("===================");
}

void printBle() {
    Serial.println();
    Serial.println("=== IDF BLE Aircon ===");
    Serial.printf("Bridge      : %s\n", AppIdfBleAircon::isStarted() ? "started" : "not started");
    Serial.printf("Stack       : %s\n", AppIdfBleAircon::isStackReady() ? "ready" : "idle");
    Serial.printf("Target      : %s type=%u%s\n",
                  AppIdfBleAircon::getTargetAddressString(),
                  static_cast<unsigned>(AppIdfBleAircon::getTargetAddressType()),
                  AppIdfBleAircon::hasStoredTarget() ? " stored" : " default");
    Serial.printf("Connected   : %s\n", AppIdfBleAircon::hasActiveConnection() ? "yes" : "no");
    Serial.printf("Keep conn   : %s\n", AppIdfBleAircon::isDisconnectAfterCommandEnabled() ? "no" : "yes");
    Serial.printf("ACK verify  : %s (notify subscribed: %s)\n",
                  AppIdfBleAircon::isAckVerificationEnabled() ? "ON" : "OFF",
                  AppIdfBleAircon::isNotifySubscribed() ? "yes" : "no");
    const AppIdfBleAircon::PairingState state = AppIdfBleAircon::getPairingState();
    const size_t count = AppIdfBleAircon::getPairingResultCount();
    Serial.printf("Pairing     : %s, results=%u\n",
                  AppIdfBleAircon::pairingStateName(state),
                  static_cast<unsigned>(count));
    for (size_t i = 0; i < count; ++i) {
        AppIdfBleAircon::PairScanEntry entry;
        if (AppIdfBleAircon::getPairingResult(i, &entry)) {
            Serial.printf("%2u. %-23s %s rssi=%d type=%u%s\n",
                          static_cast<unsigned>(i + 1),
                          entry.name[0] ? entry.name : "BLE device",
                          entry.address,
                          static_cast<int>(entry.rssi),
                          static_cast<unsigned>(entry.addressType),
                          entry.hasService ? " FFE0" : "");
        }
    }
    Serial.printf("Last error  : %s\n", AppIdfBleAircon::getLastError());
    Serial.println("Commands    : BLESCAN, BLEPAIR=<index>, BLEACON, BLEACOFF, BLECTEMP, BLEMODE, BLEFAN");
    Serial.println("              BLEDISP, BLELIGHT, BLESWING, BLEKEEP=1|0, BLEACK=1|0, BLEDROP, BLECANCEL");
    Serial.println("======================");
}

void printAudio() {
    const AppIdfAudio::Snapshot audio = AppIdfAudio::snapshot();

    Serial.println();
    Serial.println("=== IDF Audio ===");
    Serial.printf("Started     : %s\n", audio.started ? "yes" : "no");
    Serial.printf("Codec       : %s @ 0x%02x\n", audio.codecFound ? "found" : "missing", AppIdfAudio::kEs8311Address);
    Serial.printf("Chip ID     : %02x %02x ver=%02x\n", audio.chipId1, audio.chipId2, audio.chipVersion);
    Serial.printf("I2S         : %u Hz, 16-bit mono left slot\n", static_cast<unsigned>(audio.sampleRateHz));
    Serial.printf("Pins        : MCLK=%d BCLK=%d LRCK=%d DOUT=%d DIN=%d PA=%d\n",
                  PIN_I2S_MCLK,
                  PIN_I2S_BCLK,
                  PIN_I2S_LRCK,
                  PIN_I2S_DOUT,
                  PIN_I2S_DIN,
                  PIN_PA_EN);
    Serial.printf("Volume      : %u\n", static_cast<unsigned>(audio.volume));
    Serial.printf("Mic gain    : %u\n", static_cast<unsigned>(audio.micGain));
    Serial.printf("Mic         : %s\n", audio.micEnabled ? "enabled" : "disabled");
    Serial.printf("PA          : %s\n", audio.paEnabled ? "enabled" : "disabled");
    Serial.printf("Last error  : %s\n", esp_err_to_name(audio.lastError));
    Serial.println("Commands    : AUDIOSTART, SINETEST, AUDIOTEST, AUDIORESET, AUDIOCUE=<name>");
    Serial.println("              AUDIOGEN=boot|low_battery|record_stop");
    Serial.println("              VOLUME=<0..100>, MICGAIN=<0..100>, MIC=ON|OFF");
    Serial.println("=================");
}

void printI2cScan() {
    uint8_t addresses[16] = {};
    size_t count = 0;
    const esp_err_t err = AppIdfAudio::scanI2c(addresses, sizeof(addresses), &count);

    Serial.println();
    Serial.println("=== I2C Scan ===");
    if (err != ESP_OK) {
        Serial.printf("Scan failed : %s\n", esp_err_to_name(err));
        Serial.println("================");
        return;
    }
    Serial.printf("Bus pins    : SDA=%d SCL=%d\n", PIN_I2C_SDA, PIN_I2C_SCL);
    Serial.printf("Found       : %u device(s)\n", static_cast<unsigned>(count));
    const size_t printed = count < sizeof(addresses) ? count : sizeof(addresses);
    for (size_t i = 0; i < printed; ++i) {
        Serial.printf(" - 0x%02x%s\n", addresses[i], addresses[i] == AppIdfAudio::kEs8311Address ? " ES8311" : "");
    }
    if (count > printed) {
        Serial.printf(" - ... %u more not printed\n", static_cast<unsigned>(count - printed));
    }
    Serial.println("================");
}

void printWifi() {
    const AppIdfNetwork::Snapshot network = AppIdfNetwork::snapshot();

    Serial.println();
    Serial.println("=== IDF WiFi ===");
    Serial.printf("Initialized : %s\n", network.initialized ? "yes" : "no");
    Serial.printf("Started     : %s\n", network.wifiStarted ? "yes" : "no");
    Serial.printf("Credentials : %s\n", network.credentialsLoaded ? "loaded" : "missing");
    Serial.printf("Portal      : %s\n", network.portalActive ? "active" : "idle");
    Serial.printf("Portal SSID : %s\n", network.portalSsid[0] ? network.portalSsid : "-");
    Serial.printf("Portal IP   : %s\n", network.portalIp);
    Serial.printf("Portal hint : %s\n", network.portalHint[0] ? network.portalHint : "-");
    Serial.printf("State       : %s\n", network.connected ? "connected" : (network.connecting ? "connecting" : "idle"));
    Serial.printf("SSID        : %s\n", network.ssid[0] ? network.ssid : "-");
    Serial.printf("IP          : %s\n", network.ip);
    Serial.printf("RSSI        : %d\n", static_cast<int>(network.rssi));
    Serial.printf("Channel     : %u\n", static_cast<unsigned>(network.channel));
    Serial.printf("Disc reason : %u\n", static_cast<unsigned>(network.disconnectReason));
    Serial.printf("Last error  : %s\n", esp_err_to_name(network.lastError));
    Serial.println("Commands    : WIFISCAN, WIFICONNECT=<ssid>,<password>, WIFICLEAR");
    Serial.println("              WIFIPORTAL, WIFIPORTALSTOP");
    Serial.println("================");
}

void printWifiScan() {
    AppIdfNetwork::ScanResult results[AppIdfNetwork::kMaxScanResults] = {};
    size_t count = 0;
    const esp_err_t err = AppIdfNetwork::scan(results, sizeof(results) / sizeof(results[0]), &count);

    Serial.println();
    Serial.println("=== IDF WiFi Scan ===");
    if (err != ESP_OK) {
        Serial.printf("Scan failed : %s\n", esp_err_to_name(err));
        Serial.println("=====================");
        return;
    }

    Serial.printf("Found       : %u network(s)\n", static_cast<unsigned>(count));
    for (size_t i = 0; i < count; ++i) {
        Serial.printf("%2u. %-32s rssi=%d ch=%u auth=%s\n",
                      static_cast<unsigned>(i + 1),
                      results[i].ssid[0] ? results[i].ssid : "<hidden>",
                      static_cast<int>(results[i].rssi),
                      static_cast<unsigned>(results[i].channel),
                      AppIdfNetwork::authModeName(results[i].authMode));
    }
    Serial.println("=====================");
}

void printCellular() {
    const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();

    Serial.println();
    Serial.println("=== IDF 4G PPP ===");
    Serial.printf("Started     : %s\n", cellular.started ? "yes" : "no");
    Serial.printf("Power       : %s\n", cellular.powered ? "on" : "off");
    Serial.printf("UART        : %s\n", cellular.uartReady ? "ready" : "idle");
    Serial.printf("Dialing     : %s\n", cellular.dialing ? "yes" : "no");
    Serial.printf("PPP         : %s\n", cellular.pppRunning ? "running" : "idle");
    Serial.printf("Connected   : %s\n", cellular.connected ? "yes" : "no");
    Serial.printf("APN         : %s\n", cellular.apn);
    Serial.printf("IP          : %s\n", cellular.ip);
    Serial.printf("IMEI        : %s\n", cellular.imei[0] ? cellular.imei : "-");
    Serial.printf("CSQ         : %d\n", cellular.csq);
    Serial.printf("PPP phase   : %u\n", static_cast<unsigned>(cellular.pppPhase));
    Serial.printf("PPP error   : %d\n", cellular.pppError);
    Serial.printf("Dial tries  : %u\n", static_cast<unsigned>(cellular.dialAttempts));
    Serial.printf("RX/TX bytes : %u/%u\n",
                  static_cast<unsigned>(cellular.rxBytes),
                  static_cast<unsigned>(cellular.txBytes));
    Serial.printf("Last error  : %s (%s)\n", cellular.lastError, esp_err_to_name(cellular.lastEspError));
    Serial.println("Commands    : 4GON, 4GDIAL[=<apn>], 4GHANGUP, 4GOFF, 4GCSQ");
    Serial.println("===================");
}

void printTransport() {
    const AppIdfTransport::Snapshot transport = AppIdfTransport::snapshot();

    Serial.println();
    Serial.println("=== IDF Active Transport ===");
    Serial.printf("Started     : %s\n", transport.started ? "yes" : "no");
    Serial.printf("Mode        : %s\n", AppIdfTransport::netModeName(transport.mode));
    Serial.printf("Active      : %s\n", AppIdfTransport::activeTransportName(transport.active));
    Serial.printf("Pending     : %s\n", AppIdfTransport::activeTransportName(transport.pending));
    Serial.printf("4G stage    : %s\n", AppIdfTransport::cellularStageName(transport.cellularStage));
    Serial.printf("Suppressed  : %s\n", transport.autoCellularSuppressed ? "yes" : "no");
    Serial.printf("Blocked     : %s\n", transport.switchingBlocked ? "yes" : "no");
    Serial.printf("WiFi lost   : %u ms\n", static_cast<unsigned>(transport.wifiLostMs));
    Serial.printf("4G attempt  : %u ms\n", static_cast<unsigned>(transport.cellularAttemptMs));
    Serial.printf("MQTT wait   : %u ms\n", static_cast<unsigned>(transport.mqttAttemptMs));
    Serial.printf("Switches    : %u\n", static_cast<unsigned>(transport.switchCount));
    Serial.printf("Last reason : %s\n", transport.lastReason);
    Serial.printf("Last error  : %s\n", esp_err_to_name(transport.lastError));
    Serial.println("Commands    : NET=AUTO, NET=WIFI, NET=4G, NETCANCEL");
    Serial.println("============================");
}

void printMqtt() {
    const AppIdfMqtt::Snapshot mqtt = AppIdfMqtt::snapshot();

    Serial.println();
    Serial.println("=== IDF MQTT ===");
    Serial.printf("Started     : %s\n", mqtt.started ? "yes" : "no");
    Serial.printf("Client task : %s\n", mqtt.clientStarted ? "started" : "not started");
    Serial.printf("Connected   : %s\n", mqtt.connected ? "yes" : "no");
    Serial.printf("Subscribed  : %s\n", mqtt.subscribed ? "yes" : "no");
    Serial.printf("Device ID   : %s\n", mqtt.deviceId[0] ? mqtt.deviceId : "-");
    Serial.printf("Client ID   : %s\n", mqtt.clientId[0] ? mqtt.clientId : "-");
    Serial.printf("Cmd topic   : %s\n", mqtt.commandTopic);
    Serial.printf("OTA topic   : %s\n", mqtt.otaTopic);
    Serial.printf("Status topic: %s\n", mqtt.statusTopic);
    Serial.printf("Connects    : %u\n", static_cast<unsigned>(mqtt.connectCount));
    Serial.printf("Disconnects : %u\n", static_cast<unsigned>(mqtt.disconnectCount));
    Serial.printf("Received    : %u\n", static_cast<unsigned>(mqtt.receivedCount));
    Serial.printf("Cmd ok/err  : %u/%u\n",
                  static_cast<unsigned>(mqtt.commandOkCount),
                  static_cast<unsigned>(mqtt.commandErrorCount));
    Serial.printf("Last msg id : %d\n", mqtt.lastMessageId);
    Serial.printf("Last socket : %d\n", mqtt.lastSocketErrno);
    Serial.printf("Last error  : %s\n", esp_err_to_name(mqtt.lastError));
    Serial.println("Commands    : MQTTPUBSTATUS, MQTTINFO");
    Serial.println("==============");
}

void printOta() {
    const AppIdfOta::Snapshot ota = AppIdfOta::snapshot();

    Serial.println();
    Serial.println("=== IDF OTA ===");
    Serial.printf("Started     : %s\n", ota.started ? "yes" : "no");
    Serial.printf("Busy        : %s\n", ota.busy ? "yes" : "no");
    Serial.printf("Pending     : %s\n", ota.pending ? "yes" : "no");
    Serial.printf("Rollback    : %s\n", ota.rollbackPending ? "pending verify" : "idle");
    Serial.printf("Progress    : %d\n", ota.progress);
    Serial.printf("Version     : %s\n", ota.version[0] ? ota.version : "-");
    Serial.printf("Request ID  : %s\n", ota.requestId[0] ? ota.requestId : "-");
    Serial.printf("Size        : %u/%u\n",
                  static_cast<unsigned>(ota.writtenSize),
                  static_cast<unsigned>(ota.expectedSize));
    Serial.printf("Reason      : %s\n", ota.lastReason[0] ? ota.lastReason : "-");
    Serial.printf("Last error  : %s\n", esp_err_to_name(ota.lastError));
    Serial.println("==============");
}

void printServer() {
    const AppIdfServer::Snapshot server = AppIdfServer::snapshot();

    Serial.println();
    Serial.println("=== IDF Server TCP ===");
    Serial.printf("Started     : %s\n", server.started ? "yes" : "no");
    Serial.printf("Endpoint    : %s:%u\n", server.host[0] ? server.host : "-", static_cast<unsigned>(server.port));
    Serial.printf("Last probe  : %s\n", server.lastProbeOk ? "ok" : "not ok");
    Serial.printf("JSON ok     : %s\n", server.lastResponseJsonOk ? "yes" : "no");
    Serial.printf("Connects    : %u\n", static_cast<unsigned>(server.connectCount));
    Serial.printf("Responses   : %u\n", static_cast<unsigned>(server.responseCount));
    Serial.printf("Summary     : %s\n", server.lastSummary[0] ? server.lastSummary : "-");
    Serial.printf("Last error  : %s (%s)\n",
                  server.lastError[0] ? server.lastError : "-",
                  esp_err_to_name(server.lastEspError));
    Serial.println("Commands    : SERVERPROBE");
    Serial.println("======================");
}

void printRecorder() {
    const AppIdfRecorder::Snapshot recorder = AppIdfRecorder::snapshot();

    Serial.println();
    Serial.println("=== IDF AI Recorder ===");
    Serial.printf("Started     : %s\n", recorder.started ? "yes" : "no");
    Serial.printf("State       : %s\n",
                  recorder.startPending ? "start pending" : (recorder.recording ? "recording" :
                                                              (recorder.uploading ? "uploading" : "idle")));
    Serial.printf("Recorded    : %u/%u bytes\n",
                  static_cast<unsigned>(recorder.recordedBytes),
                  static_cast<unsigned>(recorder.maxRecordBytes));
    Serial.printf("Duration    : %u ms\n", static_cast<unsigned>(recorder.durationMs));
    Serial.printf("Dropped     : %u bytes\n", static_cast<unsigned>(recorder.droppedBytes));
    Serial.printf("Starts      : %u\n", static_cast<unsigned>(recorder.startCount));
    Serial.printf("Uploads     : %u, last=%s\n",
                  static_cast<unsigned>(recorder.uploadCount),
                  recorder.lastUploadOk ? "ok" : "not ok");
    Serial.printf("Summary     : %s\n", recorder.lastSummary[0] ? recorder.lastSummary : "-");
    Serial.printf("Last error  : %s\n", esp_err_to_name(recorder.lastError));
    Serial.println("Commands    : AIRECSTART, AIRECSTOP, AIRECCANCEL, AIRECUPLOAD");
    Serial.println("=======================");
}

void printWakeWord() {
    const AppIdfWakeWord::Snapshot wake = AppIdfWakeWord::snapshot();

    Serial.println();
    Serial.println("=== IDF WakeWord / VADNet ===");
    Serial.printf("Initialized : %s\n", wake.initialized ? "yes" : "no");
    Serial.printf("State       : %s%s\n",
                  wake.running ? "running" : (wake.feedTaskResident || wake.fetchTaskResident ? "resident" : "idle"),
                  wake.paused ? " (paused)" : "");
    Serial.printf("Tasks       : feed=%s fetch=%s\n",
                  wake.feedTaskResident ? "yes" : "no",
                  wake.fetchTaskResident ? "yes" : "no");
    Serial.printf("Model       : %s\n", wake.wakeModelName[0] ? wake.wakeModelName : "-");
    Serial.printf("Wake words  : %s\n", wake.wakeWords[0] ? wake.wakeWords : "-");
    Serial.printf("Models      : %d\n", wake.modelCount);
    Serial.printf("Chunks      : feed=%d samples/%d ch, fetch=%d samples/%d ch\n",
                  wake.feedChunkSamples,
                  wake.feedChannels,
                  wake.fetchChunkSamples,
                  wake.fetchChannels);
    Serial.printf("Counters    : wake=%u feed=%u fetch=%u rms=%u\n",
                  static_cast<unsigned>(wake.wakeCount),
                  static_cast<unsigned>(wake.feedCount),
                  static_cast<unsigned>(wake.fetchCount),
                  static_cast<unsigned>(wake.lastRms));
    Serial.printf("Mic         : %s\n", wake.micEnabled ? "enabled" : "disabled");
    Serial.printf("Last error  : %s\n", esp_err_to_name(wake.lastError));
    Serial.println("Commands    : WWSTART, WWPAUSE, WWRESUME, WWSTOP, AIWAKE");
    Serial.println("=============================");
}

void printTasks() {
    Serial.println();
    Serial.println("=== Migrated IDF Tasks ===");
    Serial.printf("IDF_Console watermark: %u bytes\n",
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_consoleTaskMemory.handle)));
    if (AppIdfLvgl::isStarted()) {
        Serial.printf("IDF_LVGL watermark   : %u bytes\n", static_cast<unsigned>(AppIdfLvgl::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_LVGL             : not started");
    }
    if (AppIdfInput::isStarted()) {
        Serial.printf("IDF_Input watermark  : %u bytes\n", static_cast<unsigned>(AppIdfInput::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_Input            : not started");
    }
    if (AppIdfSensors::isStarted()) {
        Serial.printf("IDF_Sensors watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfSensors::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_Sensors          : not started");
    }
    if (AppIdfBleAircon::workerTaskStackHighWatermark() > 0) {
        Serial.printf("IDF_BLE watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfBleAircon::workerTaskStackHighWatermark()));
    } else {
        Serial.println("IDF_BLE              : not started");
    }
    if (AppIdfOta::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_OTA watermark    : %u bytes\n", static_cast<unsigned>(AppIdfOta::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_OTA              : not started");
    }
    if (AppIdfNetwork::dnsTaskStackHighWatermark() > 0) {
        Serial.printf("IDF_DNS watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfNetwork::dnsTaskStackHighWatermark()));
    } else {
        Serial.println("IDF_DNS              : not started");
    }
    if (AppIdfCellular::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_4G_RX watermark  : %u bytes\n",
                      static_cast<unsigned>(AppIdfCellular::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_4G_RX            : not started");
    }
    if (AppIdfTransport::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_Transport watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfTransport::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_Transport        : not started");
    }
    if (AppIdfRecorder::taskStackHighWatermark() > 0) {
        Serial.printf("IDF_Recorder watermark: %u bytes\n",
                      static_cast<unsigned>(AppIdfRecorder::taskStackHighWatermark()));
    } else {
        Serial.println("IDF_Recorder         : not started");
    }
    if (AppIdfWakeWord::feedTaskStackHighWatermark() > 0) {
        Serial.printf("WW_Feed watermark    : %u bytes\n",
                      static_cast<unsigned>(AppIdfWakeWord::feedTaskStackHighWatermark()));
    } else {
        Serial.println("WW_Feed              : not started");
    }
    if (AppIdfWakeWord::fetchTaskStackHighWatermark() > 0) {
        Serial.printf("WW_Fetch watermark   : %u bytes\n",
                      static_cast<unsigned>(AppIdfWakeWord::fetchTaskStackHighWatermark()));
    } else {
        Serial.println("WW_Fetch             : not started");
    }
    Serial.println("==========================");
}

bool setLogLevelFromCommand(const char* command) {
    const char* level = command + 4;
    if (strcmp(level, "OFF") == 0) {
        g_LogLevel = LOG_LEVEL_OFF;
    } else if (strcmp(level, "0") == 0 || strcmp(level, "ERROR") == 0) {
        g_LogLevel = LOG_LEVEL_ERROR;
    } else if (strcmp(level, "1") == 0 || strcmp(level, "WARN") == 0) {
        g_LogLevel = LOG_LEVEL_WARN;
    } else if (strcmp(level, "2") == 0 || strcmp(level, "INFO") == 0) {
        g_LogLevel = LOG_LEVEL_INFO;
    } else if (strcmp(level, "3") == 0 || strcmp(level, "DEBUG") == 0) {
        g_LogLevel = LOG_LEVEL_DEBUG;
    } else {
        return false;
    }

    Serial.printf("\nLog level: %s (%d)\n", logLevelName(g_LogLevel), g_LogLevel);
    return true;
}

bool setThemeFromCommand(const char* command) {
    const char* value = command + 6;
    bool light = false;
    if (strcmp(value, "LIGHT") == 0 || strcmp(value, "L") == 0 || strcmp(value, "1") == 0) {
        light = true;
    } else if (strcmp(value, "DARK") == 0 || strcmp(value, "D") == 0 || strcmp(value, "0") == 0) {
        light = false;
    } else {
        return false;
    }

    const esp_err_t err = AppIdfUi::setThemeLight(light, true);
    if (err == ESP_OK) {
        Serial.printf("\nTheme: %s\n", AppIdfUi::currentThemeName());
    } else {
        Serial.printf("\nTheme change failed: %s\n", esp_err_to_name(err));
    }
    return true;
}

bool pairBleFromCommand(const char* command) {
    const char* value = command + 8;
    if (*value == '\0') {
        return false;
    }

    char* end = nullptr;
    const long index = strtol(value, &end, 10);
    if (end == value || *end != '\0' || index < 1 || index > static_cast<long>(AppIdfBleAircon::kMaxPairScanResults)) {
        return false;
    }

    const esp_err_t err = AppIdfBleAircon::startPairing(static_cast<size_t>(index - 1));
    if (err == ESP_OK) {
        Serial.printf("\nBLE pairing started for result %ld.\n", index);
    } else {
        Serial.printf("\nBLE pairing start failed: %s\n", esp_err_to_name(err));
    }
    return true;
}

const char* commandArgument(const char* command, const char* prefix) {
    const char* value = command + strlen(prefix);
    while (*value == ' ' || *value == '=') {
        ++value;
    }
    return value;
}

bool parseIntArgument(const char* command, const char* prefix, int minValue, int maxValue, int* out) {
    if (out == nullptr) {
        return false;
    }

    const char* value = commandArgument(command, prefix);
    if (*value == '\0') {
        return false;
    }

    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    while (end != nullptr && *end == ' ') {
        ++end;
    }
    if (end == value || end == nullptr || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }

    *out = static_cast<int>(parsed);
    return true;
}

bool parseOnOffArgument(const char* command, const char* prefix, bool* out) {
    if (out == nullptr) {
        return false;
    }

    const char* value = commandArgument(command, prefix);
    if (strcmp(value, "ON") == 0 || strcmp(value, "1") == 0 || strcmp(value, "TRUE") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "OFF") == 0 || strcmp(value, "0") == 0 || strcmp(value, "FALSE") == 0) {
        *out = false;
        return true;
    }
    return false;
}

void printBleCommandResult(esp_err_t err) {
    if (err == ESP_OK) {
        Serial.println("\nOK");
        return;
    }

    const char* lastError = AppIdfBleAircon::getLastError();
    Serial.printf("\nERR: %s (%s)\n",
                  (lastError != nullptr && strcmp(lastError, "none") != 0) ? lastError : esp_err_to_name(err),
                  esp_err_to_name(err));
}

bool handleBleKeepCommand(const char* command) {
    if (strcmp(command, "BLEKEEP") == 0) {
        Serial.printf("\nBLE keep connection after command: %s\n",
                      AppIdfBleAircon::isDisconnectAfterCommandEnabled() ? "OFF" : "ON");
        Serial.println("Default is ON. Use BLEKEEP=0 to auto-disconnect after commands.");
        return true;
    }

    if (!startsWith(command, "BLEKEEP=")) {
        return false;
    }

    bool keep = false;
    if (!parseOnOffArgument(command, "BLEKEEP", &keep)) {
        Serial.println("\nUsage: BLEKEEP=1|0  (1=keep connected, 0=auto-disconnect)");
        return true;
    }

    AppIdfBleAircon::setDisconnectAfterCommand(!keep);
    Serial.printf("\nOK: BLE command connection will %s after successful writes\n",
                  keep ? "be kept open" : "auto-disconnect");
    return true;
}

bool handleBleAckCommand(const char* command) {
    if (strcmp(command, "BLEACK") == 0) {
        Serial.printf("\nBLE FFE1 ACK verification: %s (notify subscribed: %s)\n",
                      AppIdfBleAircon::isAckVerificationEnabled() ? "ON" : "OFF",
                      AppIdfBleAircon::isNotifySubscribed() ? "yes" : "no");
        Serial.println("Default is ON. Use BLEACK=0 to fall back to fire-and-forget writes.");
        return true;
    }

    if (!startsWith(command, "BLEACK=")) {
        return false;
    }

    bool enabled = false;
    if (!parseOnOffArgument(command, "BLEACK", &enabled)) {
        Serial.println("\nUsage: BLEACK=1|0  (1=verify FFE1 echo, 0=legacy)");
        return true;
    }

    AppIdfBleAircon::setAckVerificationEnabled(enabled);
    Serial.printf("\nOK: BLE FFE1 ACK verification %s\n", enabled ? "enabled" : "disabled");
    return true;
}

bool handleBleControlCommand(const char* command) {
    if (strcmp(command, "BLEACON") == 0) {
        printBleCommandResult(AppIdfBleAircon::powerOn());
        return true;
    }
    if (strcmp(command, "BLEACOFF") == 0) {
        printBleCommandResult(AppIdfBleAircon::powerOff());
        return true;
    }
    if (startsWith(command, "BLECTEMP")) {
        int tempC = 0;
        if (!parseIntArgument(command, "BLECTEMP", 18, 31, &tempC)) {
            Serial.println("\nUsage: BLECTEMP <18~31>");
            return true;
        }
        printBleCommandResult(AppIdfBleAircon::setTemperature(static_cast<uint8_t>(tempC)));
        return true;
    }
    if (startsWith(command, "BLEFAN")) {
        int level = 0;
        if (!parseIntArgument(command, "BLEFAN", 1, 5, &level)) {
            Serial.println("\nUsage: BLEFAN <1~5>");
            return true;
        }
        printBleCommandResult(AppIdfBleAircon::setFanSpeed(static_cast<uint8_t>(level)));
        return true;
    }
    if (startsWith(command, "BLEDISP")) {
        bool on = false;
        if (!parseOnOffArgument(command, "BLEDISP", &on)) {
            Serial.println("\nUsage: BLEDISP ON|OFF");
            return true;
        }
        printBleCommandResult(AppIdfBleAircon::setDisplayOn(on));
        return true;
    }
    if (startsWith(command, "BLELIGHT")) {
        bool on = false;
        if (!parseOnOffArgument(command, "BLELIGHT", &on)) {
            Serial.println("\nUsage: BLELIGHT ON|OFF");
            return true;
        }
        printBleCommandResult(AppIdfBleAircon::setLightOn(on));
        return true;
    }
    if (startsWith(command, "BLEMODE")) {
        const char* mode = commandArgument(command, "BLEMODE");
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(mode, "COOL") == 0) {
            err = AppIdfBleAircon::setCoolingMode();
        } else if (strcmp(mode, "VENT") == 0 || strcmp(mode, "FAN") == 0) {
            err = AppIdfBleAircon::setVentMode();
        } else if (strcmp(mode, "ECO") == 0) {
            err = AppIdfBleAircon::setEcoMode();
        } else if (strcmp(mode, "SLEEP") == 0) {
            err = AppIdfBleAircon::setSleepMode();
        } else {
            Serial.println("\nUsage: BLEMODE COOL|VENT|ECO|SLEEP");
            return true;
        }
        printBleCommandResult(err);
        return true;
    }
    if (startsWith(command, "BLESWING")) {
        const char* direction = commandArgument(command, "BLESWING");
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (strcmp(direction, "H") == 0 || strcmp(direction, "HOR") == 0 || strcmp(direction, "HORIZONTAL") == 0 ||
            strcmp(direction, "LR") == 0) {
            err = AppIdfBleAircon::setSwingHorizontal();
        } else if (strcmp(direction, "V") == 0 || strcmp(direction, "VER") == 0 ||
                   strcmp(direction, "VERTICAL") == 0 || strcmp(direction, "UD") == 0) {
            err = AppIdfBleAircon::setSwingVertical();
        } else {
            Serial.println("\nUsage: BLESWING H|V");
            return true;
        }
        printBleCommandResult(err);
        return true;
    }

    return false;
}

void printAudioCommandResult(const char* label, esp_err_t err) {
    Serial.printf("\n%s: %s\n", label, err == ESP_OK ? "OK" : esp_err_to_name(err));
}

bool handleAudioCommand(const char* command) {
    if (equalsAny(command, "AUDIO", "AUDIOSTATUS")) {
        printAudio();
        return true;
    }
    if (equalsAny(command, "AUDIOSTART")) {
        printAudioCommandResult("AUDIOSTART", AppIdfAudio::start());
        return true;
    }
    if (equalsAny(command, "AUDIORESET", "AUDIORECOVER")) {
        printAudioCommandResult("AUDIORESET", AppIdfAudio::restart("console"));
        return true;
    }
    if (equalsAny(command, "AUDIOTEST")) {
        esp_err_t err = AppIdfAudio::start();
        if (err == ESP_OK) {
            err = AppIdfAudio::setPaEnabled(true);
        }
        if (err == ESP_OK) {
            err = AppIdfAudio::writeSilence(5000);
        }
        AppIdfAudio::setPaEnabled(false);
        printAudioCommandResult("AUDIOTEST silence", err);
        return true;
    }
    if (startsWith(command, "SINETEST")) {
        int frequency = 1000;
        const char* value = commandArgument(command, "SINETEST");
        if (*value != '\0') {
            char* end = nullptr;
            const long parsed = strtol(value, &end, 10);
            while (end != nullptr && *end == ' ') {
                ++end;
            }
            if (end == value || end == nullptr || *end != '\0' || parsed < 100 || parsed > 4000) {
                Serial.println("\nUsage: SINETEST [100..4000]");
                return true;
            }
            frequency = static_cast<int>(parsed);
        }
        printAudioCommandResult("SINETEST", AppIdfAudio::playSineTest(3000, static_cast<uint32_t>(frequency)));
        return true;
    }
    if (startsWith(command, "AUDIOCUE=") || startsWith(command, "AUDIOCUE ")) {
        const char* cueName = commandArgument(command, "AUDIOCUE");
        if (*cueName == '\0') {
            Serial.println("\nUsage: AUDIOCUE=wake_ack|settings_done|done");
            return true;
        }
        printAudioCommandResult("AUDIOCUE", AppIdfAudio::playLocalCue(cueName, 8000));
        return true;
    }
    if (startsWith(command, "AUDIOGEN=") || startsWith(command, "AUDIOGEN ")) {
        const char* cueName = commandArgument(command, "AUDIOGEN");
        if (*cueName == '\0') {
            Serial.println("\nUsage: AUDIOGEN=boot|low_battery|record_stop");
            return true;
        }
        printAudioCommandResult("AUDIOGEN", AppIdfAudio::playGeneratedCue(cueName, 3000));
        return true;
    }
    if (startsWith(command, "VOLUME=") || startsWith(command, "VOL=")) {
        const char* prefix = startsWith(command, "VOLUME=") ? "VOLUME" : "VOL";
        int volume = 0;
        if (!parseIntArgument(command, prefix, 0, 100, &volume)) {
            Serial.println("\nUsage: VOLUME=<0..100>");
            return true;
        }
        printAudioCommandResult("VOLUME", AppIdfAudio::setVolume(static_cast<uint8_t>(volume)));
        return true;
    }
    if (startsWith(command, "MICGAIN=")) {
        int gain = 0;
        if (!parseIntArgument(command, "MICGAIN", 0, 100, &gain)) {
            Serial.println("\nUsage: MICGAIN=<0..100>");
            return true;
        }
        printAudioCommandResult("MICGAIN", AppIdfAudio::setMicGain(static_cast<uint8_t>(gain)));
        return true;
    }
    if (startsWith(command, "MIC=")) {
        bool on = false;
        if (!parseOnOffArgument(command, "MIC", &on)) {
            Serial.println("\nUsage: MIC=ON|OFF");
            return true;
        }
        printAudioCommandResult("MIC", on ? AppIdfAudio::enableMicChannel() : AppIdfAudio::disableMicChannel());
        return true;
    }

    return false;
}

const char* rawCommandArgument(const char* inputLine, const char* commandName);

bool parseWifiConnectArgs(const char* args, char* ssid, size_t ssidSize, char* password, size_t passwordSize) {
    if (args == nullptr || ssid == nullptr || ssidSize == 0 || password == nullptr || passwordSize == 0) {
        return false;
    }

    while (*args == ' ' || *args == '=') {
        ++args;
    }
    const char* comma = strchr(args, ',');
    if (comma == nullptr || comma == args) {
        return false;
    }

    const size_t ssidLen = static_cast<size_t>(comma - args);
    if (ssidLen >= ssidSize) {
        return false;
    }
    memcpy(ssid, args, ssidLen);
    ssid[ssidLen] = '\0';
    trimInPlace(ssid);
    if (ssid[0] == '\0') {
        return false;
    }

    const char* passwordStart = comma + 1;
    while (*passwordStart == ' ') {
        ++passwordStart;
    }
    snprintf(password, passwordSize, "%s", passwordStart);
    trimInPlace(password);
    return true;
}

bool handleWifiCommand(const char* command, const char* inputLine) {
    if (equalsAny(command, "WIFI", "W", "WIFISTATUS")) {
        printWifi();
        return true;
    }
    if (equalsAny(command, "WIFISCAN")) {
        printWifiScan();
        return true;
    }
    if (equalsAny(command, "WIFIPORTAL", "WIFIAP", "PORTAL")) {
        const esp_err_t err = AppIdfNetwork::startPortal();
        const AppIdfNetwork::Snapshot network = AppIdfNetwork::snapshot();
        Serial.printf("\nWIFIPORTAL: %s, SSID=%s, IP=%s\n",
                      err == ESP_OK ? "OK" : esp_err_to_name(err),
                      network.portalSsid[0] ? network.portalSsid : "-",
                      network.portalIp);
        return true;
    }
    if (equalsAny(command, "WIFIPORTALSTOP", "WIFIAPSTOP", "PORTALSTOP")) {
        const esp_err_t err = AppIdfNetwork::stopPortal();
        Serial.printf("\nWIFIPORTALSTOP: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "WIFICLEAR")) {
        const esp_err_t err = AppIdfNetwork::clearCredentials();
        esp_err_t portalErr = ESP_OK;
        if (err == ESP_OK) {
            portalErr = AppIdfNetwork::startPortal();
        }
        const AppIdfNetwork::Snapshot network = AppIdfNetwork::snapshot();
        Serial.printf("\nWIFICLEAR: %s, portal=%s, SSID=%s, IP=%s\n",
                      err == ESP_OK ? "OK" : esp_err_to_name(err),
                      portalErr == ESP_OK ? "OK" : esp_err_to_name(portalErr),
                      network.portalSsid[0] ? network.portalSsid : "-",
                      network.portalIp);
        return true;
    }
    if (startsWith(command, "WIFICONNECT=") || startsWith(command, "WIFICONNECT ")) {
        char ssid[AppIdfNetwork::kMaxSsidLen + 1] = {};
        char password[AppIdfNetwork::kMaxPasswordLen + 1] = {};
        if (!parseWifiConnectArgs(rawCommandArgument(inputLine, "WIFICONNECT"), ssid, sizeof(ssid), password,
                                  sizeof(password))) {
            Serial.println("\nUsage: WIFICONNECT=<ssid>,<password>");
            return true;
        }

        const esp_err_t err = AppIdfNetwork::connect(ssid, password, true);
        Serial.printf("\nWIFICONNECT %s: %s\n", ssid, err == ESP_OK ? "requested" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "WIFIDROP", "WIFIDISCONNECT")) {
        const esp_err_t err = AppIdfNetwork::disconnect(false);
        Serial.printf("\nWIFIDROP: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }

    return false;
}

bool handleCellularCommand(const char* command) {
    if (equalsAny(command, "4G", "CSQ", "CELL")) {
        printCellular();
        return true;
    }
    if (equalsAny(command, "4GON", "CELLON")) {
        const esp_err_t err = AppIdfCellular::powerOn();
        Serial.printf("\n4GON: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "4GOFF", "CELLOFF")) {
        const esp_err_t err = AppIdfCellular::powerOff();
        Serial.printf("\n4GOFF: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "4GHANGUP", "CELLHANGUP")) {
        const esp_err_t err = AppIdfCellular::hangup();
        Serial.printf("\n4GHANGUP: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "4GCSQ", "CELLCSQ")) {
        const esp_err_t err = AppIdfCellular::refreshSignal();
        const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();
        Serial.printf("\n4GCSQ: %s, CSQ=%d\n", err == ESP_OK ? "OK" : esp_err_to_name(err), cellular.csq);
        return true;
    }
    if (startsWith(command, "4GDIAL") || startsWith(command, "CELLDIAL")) {
        const char* prefix = startsWith(command, "4GDIAL") ? "4GDIAL" : "CELLDIAL";
        const char* apn = commandArgument(command, prefix);
        if (apn == nullptr || apn[0] == '\0') {
            apn = "cmnet";
        }
        const esp_err_t err = AppIdfCellular::dial(apn, 90000);
        const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();
        Serial.printf("\n4GDIAL: %s, IP=%s, IMEI=%s, CSQ=%d\n",
                      err == ESP_OK ? "OK" : esp_err_to_name(err),
                      cellular.ip,
                      cellular.imei[0] ? cellular.imei : "-",
                      cellular.csq);
        return true;
    }
    if (startsWith(command, "4GAT=") || startsWith(command, "4GAT ")) {
        const char* at = rawCommandArgument(command, "4GAT");
        if (at == nullptr || at[0] == '\0') {
            Serial.println("\nUsage: 4GAT=<command>  (run NET=WIFI first if PPP is up)");
            return true;
        }
        char response[512] = {};
        const esp_err_t err = AppIdfCellular::sendRawAt(at, response, sizeof(response), 3000);
        Serial.printf("\n4GAT [%s]: %s\n--- response ---\n%s\n--- end ---\n",
                      at,
                      err == ESP_OK ? "OK" : esp_err_to_name(err),
                      response[0] ? response : "(empty)");
        return true;
    }
    if (equalsAny(command, "4GUART", "CELLUART")) {
        char buffer[512] = {};
        const esp_err_t err = AppIdfCellular::dumpUartRx(buffer, sizeof(buffer), 3000);
        Serial.printf("\n4GUART: %s\n--- bytes ---\n%s\n--- end ---\n",
                      err == ESP_OK ? "OK" : esp_err_to_name(err),
                      buffer[0] ? buffer : "(no data)");
        return true;
    }
    if (startsWith(command, "4GPWRKEY")) {
        const char* arg = commandArgument(command, "4GPWRKEY");
        if (arg == nullptr || arg[0] == '\0' || equalsAny(arg, "PULSE", "LOW_PULSE")) {
            const esp_err_t err = AppIdfCellular::pulsePowerKey(0, 600);
            Serial.printf("\n4GPWRKEY PULSE_LOW(600ms): %s, level=%d\n",
                          err == ESP_OK ? "OK" : esp_err_to_name(err),
                          AppIdfCellular::readPowerKeyLevel());
        } else if (equalsAny(arg, "PULSE_HIGH", "HIGH_PULSE")) {
            const esp_err_t err = AppIdfCellular::pulsePowerKey(1, 600);
            Serial.printf("\n4GPWRKEY PULSE_HIGH(600ms): %s, level=%d\n",
                          err == ESP_OK ? "OK" : esp_err_to_name(err),
                          AppIdfCellular::readPowerKeyLevel());
        } else if (equalsAny(arg, "HIGH", "1")) {
            const esp_err_t err = AppIdfCellular::setPowerKeyLevel(1);
            Serial.printf("\n4GPWRKEY=HIGH: %s, level=%d\n",
                          err == ESP_OK ? "OK" : esp_err_to_name(err),
                          AppIdfCellular::readPowerKeyLevel());
        } else if (equalsAny(arg, "LOW", "0")) {
            const esp_err_t err = AppIdfCellular::setPowerKeyLevel(0);
            Serial.printf("\n4GPWRKEY=LOW: %s, level=%d\n",
                          err == ESP_OK ? "OK" : esp_err_to_name(err),
                          AppIdfCellular::readPowerKeyLevel());
        } else if (equalsAny(arg, "READ", "?")) {
            Serial.printf("\n4GPWRKEY level=%d\n", AppIdfCellular::readPowerKeyLevel());
        } else {
            Serial.println("\nUsage: 4GPWRKEY[=PULSE|PULSE_HIGH|HIGH|LOW|READ]");
        }
        return true;
    }
    if (equalsAny(command, "4GDIAG", "CELLDIAG")) {
        const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();
        Serial.printf("\n4GDIAG:\n");
        Serial.printf("  GPIO%d (PWR)    level=%d\n", PIN_4G_PWR, AppIdfCellular::readPowerLevel());
        Serial.printf("  GPIO%d (PWRKEY) level=%d\n", PIN_4G_PWRKEY, AppIdfCellular::readPowerKeyLevel());
        Serial.printf("  UART2 TX=GPIO%d RX=GPIO%d baud=%d ready=%d\n",
                      PIN_4G_TX, PIN_4G_RX, BAUD_4G, cellular.uartReady ? 1 : 0);
        Serial.printf("  started=%d powered=%d dialing=%d ppp=%d connected=%d\n",
                      cellular.started ? 1 : 0,
                      cellular.powered ? 1 : 0,
                      cellular.dialing ? 1 : 0,
                      cellular.pppRunning ? 1 : 0,
                      cellular.connected ? 1 : 0);
        Serial.printf("  imei=%s csq=%d apn=%s last_err=%s\n",
                      cellular.imei[0] ? cellular.imei : "-",
                      cellular.csq,
                      cellular.apn,
                      cellular.lastError);
        Serial.println("Hint: NET=WIFI first to stop dial loop, then 4GON; 4GAT=AT.");
        return true;
    }

    return false;
}

bool handleTransportCommand(const char* command) {
    if (equalsAny(command, "NET", "TRANSPORT")) {
        printTransport();
        return true;
    }
    if (equalsAny(command, "NET=AUTO", "NET AUTO")) {
        const esp_err_t err = AppIdfTransport::requestMode(AppIdfTransport::NetMode::AUTO);
        Serial.printf("\nNET=AUTO: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "NET=WIFI", "NET WIFI")) {
        const esp_err_t err = AppIdfTransport::requestMode(AppIdfTransport::NetMode::WIFI_ONLY);
        Serial.printf("\nNET=WIFI: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "NET=4G", "NET 4G", "NET=CELL")) {
        const esp_err_t err = AppIdfTransport::requestMode(AppIdfTransport::NetMode::CELLULAR_ONLY);
        Serial.printf("\nNET=4G: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    if (equalsAny(command, "NETCANCEL", "CELLCANCEL", "4GCANCEL")) {
        const esp_err_t err = AppIdfTransport::cancelCellularAttempt();
        Serial.printf("\nNETCANCEL: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
        return true;
    }
    return false;
}

const char* rawCommandArgument(const char* inputLine, const char* commandName) {
    if (inputLine == nullptr || commandName == nullptr) {
        return nullptr;
    }

    const size_t commandLen = strlen(commandName);
    const char* value = inputLine + commandLen;
    while (*value == ' ' || *value == '=') {
        ++value;
    }
    return *value != '\0' ? value : nullptr;
}

void executeAiControlJsonFromCommand(const char* json) {
    if (json == nullptr || json[0] == '\0') {
        Serial.println("\nUsage: AICMD=<server-json-with-control>");
        return;
    }

    AppIdfCommandExecutor::Result result;
    const esp_err_t err = AppIdfCommandExecutor::executeControlJson(json, &result);
    if (err == ESP_OK && !result.handled) {
        Serial.printf("\nAICMD: %s\n", result.summary[0] ? result.summary : "no command");
        return;
    }
    if (err == ESP_OK && result.success) {
        Serial.printf("\nAICMD OK: %s\n", result.summary[0] ? result.summary : "done");
        return;
    }

    Serial.printf("\nAICMD ERR: %s (%s)\n",
                  result.error[0] ? result.error : esp_err_to_name(err),
                  esp_err_to_name(err));
}

void injectOtaPreflightFromCommand(const char* json) {
    if (json == nullptr || json[0] == '\0') {
        Serial.println("\nUsage: OTAPREFLIGHT=<ota-preflight-json>");
        return;
    }

    const esp_err_t err = AppIdfOta::handlePreflightRequest(json);
    Serial.printf("\nOTAPREFLIGHT: %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
}

void injectOtaNotifyFromCommand(const char* json) {
    if (json == nullptr || json[0] == '\0') {
        Serial.println("\nUsage: OTANOTIFY=<ota-notify-json>");
        return;
    }

    const esp_err_t err = AppIdfOta::handleNotify(json);
    Serial.printf("\nOTANOTIFY: %s\n", err == ESP_OK ? "scheduled" : esp_err_to_name(err));
}

void dispatchUiKey(AppIdfInput::KeyId keyId, AppIdfInput::KeyAction action) {
    AppIdfInput::KeyEvent event = {};
    event.keyId = keyId;
    event.action = action;
    AppIdfUi::handleKeyEvent(event);
    Serial.printf("\nUIKEY %s/%s: screen=%s keys=%u\n",
                  AppIdfInput::keyIdName(keyId),
                  AppIdfInput::keyActionName(action),
                  AppIdfUi::activeScreenName(),
                  static_cast<unsigned>(AppIdfUi::handledKeyCount()));
}

bool handleUiKeyCommand(const char* command) {
    if (!startsWith(command, "UIKEY=") && !startsWith(command, "UIKEY ")) {
        return false;
    }

    const char* arg = rawCommandArgument(command, "UIKEY");
    if (arg == nullptr) {
        Serial.println("\nUsage: UIKEY=KEY1|KEY2|KEY1_LONG");
        return true;
    }

    if (equalsAny(arg, "KEY1", "K1", "1") || equalsAny(arg, "KEY1_SHORT", "K1_SHORT")) {
        dispatchUiKey(AppIdfInput::KeyId::Key1, AppIdfInput::KeyAction::ShortPress);
        return true;
    }
    if (equalsAny(arg, "KEY2", "K2", "2") || equalsAny(arg, "KEY2_SHORT", "K2_SHORT")) {
        dispatchUiKey(AppIdfInput::KeyId::Key2, AppIdfInput::KeyAction::ShortPress);
        return true;
    }
    if (equalsAny(arg, "KEY1_LONG", "K1_LONG", "BACK")) {
        dispatchUiKey(AppIdfInput::KeyId::Key1, AppIdfInput::KeyAction::LongPressStart);
        return true;
    }

    Serial.println("\nUsage: UIKEY=KEY1|KEY2|KEY1_LONG");
    return true;
}

void printNotMigrated(const char* command) {
    Serial.printf("\n%s is not available in the current IDF firmware.\n", command);
    Serial.println("Use HELP to see the migrated IDF command set.");
}

void processCompleteLine(char* line) {
    trimInPlace(line);
    if (line[0] == '\0') {
        return;
    }

    handleCommand(line);
    fflush(stdout);
}

void consoleTask(void*) {
    configureMainConsoleInput();
    configureNonBlockingStdin();
    configureUsbSecondaryConsoleInput();
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Console");

    Serial.println();
    Serial.println("[IDF_CONSOLE] Ready. Type HELP for commands.");

    char line[kCommandBufferSize] = {};
    size_t lineLen = 0;

    while (true) {
        char ch = '\0';
        const ssize_t readLen = readConsoleChar(&ch);
        if (readLen == 1) {
            if (ch == '\r' || ch == '\n') {
                line[lineLen] = '\0';
                processCompleteLine(line);
                lineLen = 0;
                line[0] = '\0';
            } else if (ch == '\b' || ch == 0x7f) {
                if (lineLen > 0) {
                    --lineLen;
                    line[lineLen] = '\0';
                }
            } else if (isprint(static_cast<unsigned char>(ch))) {
                if (lineLen + 1 < sizeof(line)) {
                    line[lineLen++] = ch;
                } else {
                    lineLen = 0;
                    line[0] = '\0';
                    Serial.println("\nERR: command line too long");
                }
            }
        }

        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

}  // namespace

esp_err_t start() {
    if (g_consoleTaskMemory.handle != nullptr) {
        return ESP_OK;
    }

    return AppIdfTasks::createPinnedToCoreInternal(consoleTask, "IDF_Console", kConsoleTaskStackWords, nullptr, 2, 0,
                                                   &g_consoleTaskMemory);
}

void printHelp() {
    Serial.println();
    Serial.println("=== IDF Serial Command Help ===");
    Serial.println("STATUS or ?     - Show IDF firmware status");
    Serial.println("DIAG or FREE    - Show SRAM/PSRAM and console task stack");
    Serial.println("FS or LITTLEFS  - Show LittleFS resource partition status");
    Serial.println("DISPLAY         - Refresh LVGL UI or redraw ST7789P3 boot probe fallback");
    Serial.println("KEY or ADCKEY   - Show ADC key raw/mV and last IDF key event");
    Serial.println("SENSORS/BAT/TEMP - Show IDF battery, charge and temperature readings");
    Serial.println("BLE/BLESCAN/BLEPAIR=n - Scan and pair BLE aircon target");
    Serial.println("BLEACON/BLEACOFF - Turn BLE aircon on or off");
    Serial.println("BLECTEMP/BLEMODE/BLEFAN - Set BLE aircon temp, mode or fan");
    Serial.println("BLEDISP/BLELIGHT/BLESWING - Set BLE display, light or swing");
    Serial.println("BLEKEEP=1|0/BLEDROP - Keep or release the BLE command connection");
    Serial.println("BLEACK=1|0      - Toggle FFE1 echo ACK verification (default ON)");
    Serial.println("AICMD=<json>    - Execute server-style control JSON through IDF executor");
    Serial.println("AUDIO/AUDIOSTART - Show or initialize IDF ES8311 + I2S audio");
    Serial.println("SINETEST/AUDIOTEST/AUDIORESET/AUDIOCUE=<name> - Verify or recover IDF audio");
    Serial.println("I2CSCAN         - Scan the ES8311 I2C bus");
    Serial.println("VOLUME=<0..100>/MICGAIN=<0..100>/MIC=ON|OFF - Audio controls");
    Serial.println("WIFI/WIFISCAN/WIFICONNECT=<ssid>,<password>/WIFICLEAR - IDF WiFi STA");
    Serial.println("WIFIPORTAL/WIFIPORTALSTOP - Start or stop IDF WiFi setup portal");
    Serial.println("NET/NET=AUTO/NET=WIFI/NET=4G/NETCANCEL - IDF active transport mode");
    Serial.println("4G/4GON/4GDIAL[=<apn>]/4GHANGUP/4GOFF/4GCSQ - IDF 4G PPP");
    Serial.println("4GAT=<at>/4GUART/4GPWRKEY[=PULSE|PULSE_HIGH|HIGH|LOW|READ]/4GDIAG - LE270 bring-up debug");
    Serial.println("MQTT/MQTTPUBSTATUS/MQTTINFO - IDF MQTT status and telemetry publish");
    Serial.println("OTA            - Show IDF OTA download / rollback state");
    Serial.println("OTAPREFLIGHT/OTANOTIFY=<json> - Inject OTA test JSON through IDF OTA path");
    Serial.println("SERVER/SERVERPROBE - Show or probe IDF TCP server identity path");
    Serial.println("AIREC/AIRECSTART/AIRECSTOP - Record PCM and upload through IDF TCP server");
    Serial.println("AIRECCANCEL/AIRECUPLOAD - Cancel recording or upload the last recorded PCM");
    Serial.println("WW/WWSTART/WWPAUSE/WWRESUME/WWSTOP - IDF ESP-SR WakeWord controls");
    Serial.println("AIWAKE          - Simulate a wake event: wake_ack, VADNet record, upload");
    Serial.println("UIKEY=KEY1|KEY2|KEY1_LONG - Inject IDF UI navigation keys");
    Serial.println("THEME[=LIGHT|DARK] - Show or set UI theme");
    Serial.println("MODE[=BLE|IR|RF433] - Show or switch app mode (BLE/IR/RF433 mutex, restarts)");
    Serial.println("IR/IRSTATUS/IRLIST/IRCLEAR - IR module status, learned list, clear all");
    Serial.println("IRLEARN=<name>/IRLEARN(stop)/IRSEND=<name>[,<count>] - IR learn/replay");
    Serial.println("RF/RFSTATUS, RF=IDLE|LEARN|LISTEN|SNIFF, RFTEST - RF433 (CMT2300A) mode/test");
    Serial.println("RFSEND=<hex_code>,<bits>[,<T_us>] - RF433 transmit a learned code");
    Serial.println("SCENE/SCENELIST/SCENERUN=<id>/SCENEDEL=<id>/SCENECLEAR - scene table");
    Serial.println("SCENEADDIR=<desc>,<ir_name>/SCENEADD433=<desc>,<hex>,<bits>,<T> - add scene");
    Serial.println("PART            - Show key partition offsets and model header");
    Serial.println("VER or VERSION  - Show app version and OTA slot");
    Serial.println("CHIP            - Show ESP32-S3 chip and flash info");
    Serial.println("TASKS           - Show migrated IDF task diagnostics");
    Serial.println("LOG=OFF|0|1|2|3 - Set log level");
    Serial.println("POWER/POWER L1/POWER L0/POWER ON|OFF - PowerSave state and control");
    Serial.println("RESTART         - Restart the device");
    Serial.println("BAT-INJECT=<V>  - Override battery voltage (0 to clear) for low-bat state-machine test");
    Serial.println("BAT-CHARGING=<0|1|-1> - Override charging flag (-1 clears)");
    Serial.println("HELP            - Show this help");
    Serial.println("===============================");
}

void handleCommand(const char* inputLine) {
    char command[kCommandBufferSize] = {};
    uppercaseCopy(inputLine, command, sizeof(command));
    trimInPlace(command);

    if (equalsAny(command, "HELP", "H")) {
        printHelp();
    } else if (equalsAny(command, "STATUS", "?", "S")) {
        printStatus();
    } else if (equalsAny(command, "FREE", "DIAG", "HEAP") || strcmp(command, "MEM") == 0) {
        printHeap();
    } else if (equalsAny(command, "FS", "LITTLEFS")) {
        printFilesystem();
    } else if (equalsAny(command, "DISPLAY", "LCD", "SCREEN")) {
        printDisplay();
    } else if (equalsAny(command, "KEY", "KEYS", "ADCKEY")) {
        printInput();
    } else if (equalsAny(command, "SENSORS", "SENSOR") || equalsAny(command, "BAT", "BATTERY", "VOLTAGE") ||
               equalsAny(command, "TEMP", "T")) {
        printSensors();
    } else if (equalsAny(command, "BLE", "BLESTATUS")) {
        printBle();
    } else if (equalsAny(command, "BLESCAN", "BLEPAIRSCAN")) {
        const esp_err_t err = AppIdfBleAircon::startPairingScan();
        Serial.printf("\nBLE scan: %s\n", err == ESP_OK ? "started" : esp_err_to_name(err));
    } else if (startsWith(command, "BLEPAIR=")) {
        if (!pairBleFromCommand(command)) {
            Serial.println("\nUsage: BLEPAIR=<1..10>");
        }
    } else if (equalsAny(command, "BLECANCEL")) {
        AppIdfBleAircon::cancelPairing();
        Serial.println("\nBLE pairing canceled.");
    } else if (equalsAny(command, "BLEDROP")) {
        printBleCommandResult(AppIdfBleAircon::releaseConnection());
    } else if (handleBleKeepCommand(command)) {
    } else if (handleBleAckCommand(command)) {
    } else if (handleBleControlCommand(command)) {
    } else if (handleAudioCommand(command)) {
    } else if (equalsAny(command, "I2CSCAN", "I2C")) {
        printI2cScan();
    } else if (handleWifiCommand(command, inputLine)) {
    } else if (handleTransportCommand(command)) {
    } else if (handleCellularCommand(command)) {
    } else if (equalsAny(command, "MQTT", "MQTTSTATUS")) {
        printMqtt();
    } else if (equalsAny(command, "MQTTPUBSTATUS")) {
        const esp_err_t err = AppIdfMqtt::publishStatus("online");
        Serial.printf("\nMQTT status publish: %s\n", err == ESP_OK ? "queued" : esp_err_to_name(err));
    } else if (equalsAny(command, "MQTTINFO")) {
        const esp_err_t err = AppIdfMqtt::publishDeviceInfo();
        Serial.printf("\nMQTT device info publish: %s\n", err == ESP_OK ? "queued" : esp_err_to_name(err));
    } else if (equalsAny(command, "OTA", "OTASTATUS")) {
        printOta();
    } else if (startsWith(command, "OTAPREFLIGHT=") || startsWith(command, "OTAPREFLIGHT ")) {
        injectOtaPreflightFromCommand(rawCommandArgument(inputLine, "OTAPREFLIGHT"));
    } else if (startsWith(command, "OTANOTIFY=") || startsWith(command, "OTANOTIFY ")) {
        injectOtaNotifyFromCommand(rawCommandArgument(inputLine, "OTANOTIFY"));
    } else if (equalsAny(command, "SERVER", "SERVERSTATUS")) {
        printServer();
    } else if (equalsAny(command, "SERVERPROBE")) {
        const esp_err_t err = AppIdfServer::probe();
        Serial.printf("\nServer probe: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    } else if (equalsAny(command, "AIREC", "AIRECSTATUS")) {
        printRecorder();
    } else if (equalsAny(command, "AIRECSTART")) {
        const esp_err_t err = AppIdfRecorder::startRecording();
        Serial.printf("\nAIRECSTART: %s\n", err == ESP_OK ? "recording requested" : esp_err_to_name(err));
    } else if (equalsAny(command, "AIRECSTOP")) {
        const esp_err_t err = AppIdfRecorder::stopRecordingAndUpload();
        Serial.printf("\nAIRECSTOP: %s\n", err == ESP_OK ? "stop/upload requested" : esp_err_to_name(err));
    } else if (equalsAny(command, "AIRECCANCEL")) {
        const esp_err_t err = AppIdfRecorder::cancelRecording();
        Serial.printf("\nAIRECCANCEL: %s\n", err == ESP_OK ? "cancel requested" : esp_err_to_name(err));
    } else if (equalsAny(command, "AIRECUPLOAD")) {
        const esp_err_t err = AppIdfRecorder::uploadLastRecording();
        Serial.printf("\nAIRECUPLOAD: %s\n", err == ESP_OK ? "upload requested" : esp_err_to_name(err));
    } else if (equalsAny(command, "WW", "WAKE", "WAKEWORD")) {
        printWakeWord();
    } else if (equalsAny(command, "WWSTART", "WAKESTART")) {
        const esp_err_t err = AppIdfWakeWord::start();
        Serial.printf("\nWWSTART: %s\n", err == ESP_OK ? "started" : esp_err_to_name(err));
    } else if (equalsAny(command, "WWPAUSE", "WAKEPAUSE")) {
        AppIdfWakeWord::pause();
        Serial.println("\nWWPAUSE: OK");
    } else if (equalsAny(command, "WWRESUME", "WAKERESUME")) {
        const esp_err_t err = AppIdfWakeWord::resume();
        Serial.printf("\nWWRESUME: %s\n", err == ESP_OK ? "resumed" : esp_err_to_name(err));
    } else if (equalsAny(command, "WWSTOP", "WAKESTOP")) {
        AppIdfWakeWord::stop();
        Serial.println("\nWWSTOP: OK");
    } else if (equalsAny(command, "AIWAKE", "WAKEONCE")) {
        AppIdfWakeWord::pause();
        const esp_err_t err = AppIdfRecorder::startWakeInteraction();
        if (err != ESP_OK) {
            (void)AppIdfWakeWord::resume();
        }
        Serial.printf("\nAIWAKE: %s\n", err == ESP_OK ? "wake interaction requested" : esp_err_to_name(err));
    } else if (startsWith(command, "AICMD=") || startsWith(command, "AICMD ")) {
        executeAiControlJsonFromCommand(rawCommandArgument(inputLine, "AICMD"));
    } else if (startsWith(command, "CTRL=") || startsWith(command, "CTRL ")) {
        executeAiControlJsonFromCommand(rawCommandArgument(inputLine, "CTRL"));
    } else if (handleUiKeyCommand(command)) {
    } else if (equalsAny(command, "THEME", "UI")) {
        printTheme();
    } else if (handleIrCommand(command, inputLine)) {
    } else if (handleRf433Command(command, inputLine)) {
    } else if (handleSceneCommand(command, inputLine)) {
    } else if (equalsAny(command, "MODE", "APPMODE")) {
        printAppMode();
    } else if (startsWith(command, "MODE=")) {
        if (!setAppModeFromCommand(command)) {
            Serial.println("\nUsage: MODE=BLE|IR|RF433");
        }
    } else if (equalsAny(command, "PART", "PARTITIONS")) {
        printPartitions();
    } else if (equalsAny(command, "VER", "VERSION", "V")) {
        printAppInfo();
    } else if (equalsAny(command, "CHIP", "CHIPINFO")) {
        printChipInfo();
    } else if (equalsAny(command, "TASKS", "TASK")) {
        printTasks();
    } else if (startsWith(command, "LOG=")) {
        if (!setLogLevelFromCommand(command)) {
            Serial.println("\nUsage: LOG=OFF|0|1|2|3");
        }
    } else if (startsWith(command, "THEME=")) {
        if (!setThemeFromCommand(command)) {
            Serial.println("\nUsage: THEME=LIGHT|DARK");
        }
    } else if (equalsAny(command, "POWER", "POWERSAVE", "POWERSTATUS")) {
        Serial.printf("\nPower state=%s enabled=%d idle=%ums (timeout=60000ms)\n",
                      AppIdfPowerSave::isL1() ? "L1_STANDBY" : "L0_ACTIVE",
                      AppIdfPowerSave::isEnabled() ? 1 : 0,
                      static_cast<unsigned>(AppIdfPowerSave::inactivityMs()));
    } else if (equalsAny(command, "POWER L1", "POWERL1")) {
        const esp_err_t err = AppIdfPowerSave::enterL1();
        Serial.printf("\nPOWER L1: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    } else if (equalsAny(command, "POWER L0", "POWERL0", "POWERWAKE") || strcmp(command, "POWER WAKE") == 0) {
        const esp_err_t err = AppIdfPowerSave::exitL1();
        Serial.printf("\nPOWER L0: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    } else if (equalsAny(command, "POWER ON", "POWERON")) {
        AppIdfPowerSave::setEnabled(true);
        Serial.println("\nPOWER: enabled");
    } else if (equalsAny(command, "POWER OFF", "POWEROFF")) {
        AppIdfPowerSave::setEnabled(false);
        Serial.println("\nPOWER: disabled (state forced to L0)");
    } else if (equalsAny(command, "RESTART", "RESET", "REBOOT")) {
        Serial.println("\nRestarting...");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    } else if (startsWith(command, "BAT-INJECT=") || startsWith(command, "BAT-INJECT ") ||
               startsWith(command, "BATINJECT=") || startsWith(command, "BATINJECT ")) {
        const char* arg = command;
        while (*arg != '\0' && *arg != '=' && *arg != ' ') ++arg;
        if (*arg != '\0') ++arg;
        const float voltage = strtof(arg, nullptr);
        AppIdfSensors::debugInjectVoltage(voltage);
        Serial.printf("\nBAT-INJECT: voltage override=%.3fV (0 to clear)\n",
                      static_cast<double>(voltage));
    } else if (startsWith(command, "BAT-CHARGING=") || startsWith(command, "BAT-CHARGING ") ||
               startsWith(command, "BATCHARGING=") || startsWith(command, "BATCHARGING ")) {
        const char* arg = command;
        while (*arg != '\0' && *arg != '=' && *arg != ' ') ++arg;
        if (*arg != '\0') ++arg;
        int value = -1;
        if (*arg != '\0') {
            value = static_cast<int>(strtol(arg, nullptr, 10));
        }
        AppIdfSensors::debugInjectCharging(value);
        Serial.printf("\nBAT-CHARGING: override=%d (-1 to clear)\n", value);
    } else if (startsWith(command, "AI") || startsWith(command, "BAT")) {
        printNotMigrated(command);
    } else {
        Serial.printf("\nUnknown command: %s\n", command);
        Serial.println("Type HELP for available IDF commands.");
    }
}

}  // namespace AppIdfConsole
