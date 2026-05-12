#include "App_FlashGuard.h"
#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfBleAircon.h"
#include "App_IdfCellular.h"
#include "App_IdfConsole.h"
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
#include "App_IdfLearnFlow.h"
#include "App_IdfScene.h"
#include "App_IdfServer.h"
#include "App_IdfSensors.h"
#include "App_IdfSystem.h"
#include "App_IdfTasks.h"
#include "App_IdfTime.h"
#include "App_IdfTransport.h"
#include "App_IdfUi.h"
#include "App_IdfWakeWord.h"
#include "App_Log.h"
#include "Battery_Config.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG_BOOT = "IDF_BOOT";
constexpr const char* kServerHost = "<your-server-host>";
constexpr uint16_t kServerPort = 9090;
constexpr uint32_t kHealthTaskStackWords = 4096;

AppIdfTasks::StaticTaskMemory g_healthTaskMemory;

void logFlashGuardSelfCheck() {
    ScopedFlashGuard guard("bootstrap self-check", 1000);
    if (!guard.ok()) {
        LOG_W(TAG_BOOT, "Flash guard self-check failed");
        return;
    }

    LOG_I(TAG_BOOT, "Flash guard self-check passed");
}

void healthTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_Health");
    uint32_t heartbeatTicks = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        AppIdfSystem::logHeapSnapshot("periodic");
        AppIdfTasks::logTaskMemory(g_healthTaskMemory, "IDF_Health");
        ++heartbeatTicks;
        if (heartbeatTicks >= 30) {
            heartbeatTicks = 0;
            if (AppIdfMqtt::isConnected()) {
                AppIdfMqtt::publishDeviceInfo();
            }
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

}  // namespace

extern "C" void app_main(void) {
    LOG_BANNER("ESP-IDF migration bootstrap");

    const esp_err_t nvsErr = AppIdfSystem::initNvs();
    if (nvsErr != ESP_OK) {
        LOG_E(TAG_BOOT, "NVS init failed: %s", esp_err_to_name(nvsErr));
    }

    AppFlashGuard::init();
    logFlashGuardSelfCheck();
    AppIdfTasks::initTaskWatchdog(60000, true);

    AppIdfAppMode::init();
    LOG_I(TAG_BOOT, "App mode at boot: %s", AppIdfAppMode::nameAscii(AppIdfAppMode::current()));

    AppIdfSystem::logAppDescription();
    AppIdfSystem::logChipInfo();
    AppIdfSystem::logKeyPartitions();
    // /littlefs (spiffs 分区) 只读静态资源 (audio_cues 等)；由 littlefs_create_partition_image
    // FLASH_IN_PROJECT 在每次 idf.py flash 时刷新。
    // 注：保留可写挂载是因为 esp_littlefs 在 read_only=1 模式下 lookup 比较激进；这里 read_only=0
    // 但运行期不主动写资源类文件。
    AppIdfFilesystem::mountResourcePartition(false);

    // /userdata (userdata 分区) 存运行期用户数据：scenes.json / ir_codes.json。
    // 该分区不绑定 image，idf.py flash 不会覆盖。首次烧录后分区为 0xFF，挂载时自动 format。
    const esp_err_t userFsErr = AppIdfFilesystem::mountUserDataPartition();
    if (userFsErr != ESP_OK) {
        LOG_E(TAG_BOOT, "userdata partition mount failed: %s", esp_err_to_name(userFsErr));
    }

    const esp_err_t networkErr = AppIdfNetwork::start();
    if (networkErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF network start failed: %s", esp_err_to_name(networkErr));
    }

    AppIdfTime::init();

    const esp_err_t cellularErr = AppIdfCellular::start();
    if (cellularErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF cellular init failed: %s", esp_err_to_name(cellularErr));
    }

    const esp_err_t audioErr = AppIdfAudio::start();
    if (audioErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF audio start failed: %s", esp_err_to_name(audioErr));
    } else {
        const esp_err_t bootCueErr = AppIdfAudio::playGeneratedCue("boot", 2500);
        if (bootCueErr != ESP_OK) {
            LOG_W(TAG_BOOT, "IDF boot cue skipped: %s", esp_err_to_name(bootCueErr));
        }
    }

    if (AppIdfAppMode::isBle()) {
        const esp_err_t bleErr = AppIdfBleAircon::start();
        if (bleErr != ESP_OK) {
            LOG_E(TAG_BOOT, "IDF BLE bridge start failed: %s", esp_err_to_name(bleErr));
        }
    } else if (AppIdfAppMode::isIr()) {
        const esp_err_t irErr = AppIdfIr::start();
        if (irErr != ESP_OK) {
            LOG_E(TAG_BOOT, "IDF IR module start failed: %s", esp_err_to_name(irErr));
        }
    } else if (AppIdfAppMode::isRf433()) {
        const esp_err_t rf433Err = AppIdfRf433::start();
        if (rf433Err != ESP_OK) {
            LOG_E(TAG_BOOT, "IDF RF433 module start failed: %s", esp_err_to_name(rf433Err));
        }
    }

    // 三种模式都启动 Scene：BLE 用 ROM 预制（不写盘），IR/RF433 加载 scenes.json。
    const esp_err_t sceneErr = AppIdfScene::start();
    if (sceneErr != ESP_OK) {
        LOG_W(TAG_BOOT, "IDF scene module start failed: %s", esp_err_to_name(sceneErr));
    }

    // LearnFlow 仅在 IR/RF433 模式有用，但 task 常驻便于按需启动；BLE 模式下也会处于 Idle 不耗事件。
    const esp_err_t learnErr = AppIdfLearnFlow::start();
    if (learnErr != ESP_OK) {
        LOG_W(TAG_BOOT, "IDF learn flow start failed: %s", esp_err_to_name(learnErr));
    }

    const esp_err_t sensorsErr = AppIdfSensors::start();
    if (sensorsErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF sensors start failed: %s", esp_err_to_name(sensorsErr));
    }

    const esp_err_t otaErr = AppIdfOta::start();
    if (otaErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF OTA start failed: %s", esp_err_to_name(otaErr));
    }

    const esp_err_t serverErr = AppIdfServer::start(kServerHost, kServerPort);
    if (serverErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF server client init failed: %s", esp_err_to_name(serverErr));
    }

    const esp_err_t recorderErr = AppIdfRecorder::start();
    if (recorderErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF recorder init failed: %s", esp_err_to_name(recorderErr));
    }

    // MQTT 由 AppIdfTransport 在 WiFi 拿到 IP 或 4G PPP 拨号成功后通过
    // restartMqttForRoute() 拉起，避免在 DHCP/DNS 就绪前触发 esp-tls/mqtt_client
    // 的 getaddrinfo 失败红字。
    const esp_err_t transportErr = AppIdfTransport::start();
    if (transportErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF transport manager start failed: %s", esp_err_to_name(transportErr));
    }

    AppIdfUi::preloadThemePreference();

    // 早期低电量门控：电压过低 & 未充电时直接进入裸屏闪烁画面 → deep sleep，
    // 不启动 LVGL/WiFi/4G/MQTT/Audio/WakeWord 等高耗电模块。
    // sensors::start() 已在前面跑了一次 sampleAndPublish()，battery snapshot 可读。
    {
        const auto snapshot = AppIdfSensors::latest();
        const bool batteryReady = snapshot.battery.valid && snapshot.battery.voltage > 0.0f;
        if (batteryReady && !snapshot.battery.charging &&
            snapshot.battery.voltage <= BatteryConfig::kEarlyShutdownVoltage) {
            LOG_W(TAG_BOOT,
                  "Early low-battery gate triggered: voltage=%.3fV (<=%.2fV, not charging)",
                  static_cast<double>(snapshot.battery.voltage),
                  static_cast<double>(BatteryConfig::kEarlyShutdownVoltage));
            const esp_err_t earlyErr = AppIdfDisplay::runLowBatteryShutdownScreen();
            if (earlyErr != ESP_OK) {
                // 显示初始化失败也要进入 deep sleep 保护电池
                LOG_E(TAG_BOOT, "Low-battery screen init failed (%s), sleeping anyway",
                      esp_err_to_name(earlyErr));
                AppIdfDisplay::enterDeepSleepForLowBattery("early-gate-no-display");
            }
            // 函数返回意味着检测到充电插入，正常 boot 继续
            LOG_I(TAG_BOOT, "Charger detected during early gate, resuming boot");
        }
    }

    const esp_err_t lvglErr = AppIdfLvgl::start();
    if (lvglErr != ESP_OK) {
        LOG_E(TAG_BOOT, "LVGL startup failed: %s", esp_err_to_name(lvglErr));
        const esp_err_t displayErr = AppIdfDisplay::drawBootProbe();
        if (displayErr != ESP_OK) {
            LOG_E(TAG_BOOT, "Display boot probe fallback failed: %s", esp_err_to_name(displayErr));
        }
    } else {
        const esp_err_t uiErr = AppIdfUi::start();
        if (uiErr != ESP_OK) {
            LOG_E(TAG_BOOT, "IDF UI bridge start failed: %s", esp_err_to_name(uiErr));
        }
        // Turn on backlight after theme loading and screen setup are complete,
        // so the user never sees an intermediate theme-switch render.
        AppIdfLvgl::enableBacklightAfterNextFrame();
    }

    const esp_err_t inputErr = AppIdfInput::start(AppIdfUi::handleKeyEvent);
    if (inputErr != ESP_OK) {
        LOG_E(TAG_BOOT, "ADC key input start failed: %s", esp_err_to_name(inputErr));
    }

    const esp_err_t wakeErr = AppIdfWakeWord::start();
    if (wakeErr != ESP_OK) {
        LOG_E(TAG_BOOT, "IDF WakeWord start failed: %s", esp_err_to_name(wakeErr));
    }

    AppIdfSystem::logHeapSnapshot("boot");

    const esp_err_t healthTaskErr =
        AppIdfTasks::createPinnedToCoreInternal(healthTask, "IDF_Health", kHealthTaskStackWords, nullptr, 1, 1,
                                                &g_healthTaskMemory);
    if (healthTaskErr != ESP_OK) {
        LOG_E(TAG_BOOT, "Health task creation failed: %s", esp_err_to_name(healthTaskErr));
    }

    const esp_err_t consoleErr = AppIdfConsole::start();
    if (consoleErr != ESP_OK) {
        LOG_E(TAG_BOOT, "Console task creation failed: %s", esp_err_to_name(consoleErr));
    }

    const esp_err_t powerErr = AppIdfPowerSave::start();
    if (powerErr != ESP_OK) {
        LOG_E(TAG_BOOT, "PowerSave start failed: %s", esp_err_to_name(powerErr));
    }

    LOG_I(TAG_BOOT, "ESP-IDF firmware is alive.");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
