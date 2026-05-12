#include "App_IdfDisplay.h"

#include <stdint.h>

#include "App_Log.h"
#include "Battery_Config.h"
#include "Pin_Config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfDisplay {
namespace {

constexpr const char* TAG_DISPLAY = "IDF_DISPLAY";
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
constexpr int kMemoryHeight = 320;
constexpr int kPixelClockHz = 80 * 1000 * 1000;
constexpr int kProbeLines = 16;
constexpr int kRotation2YGap = kMemoryHeight - kScreenHeight;

esp_lcd_panel_io_handle_t g_panelIo = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
bool g_initialized = false;
bool g_backlightOn = false;

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

esp_err_t configureBacklightGpio() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << PIN_TFT_BL;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t initSpiBus() {
    spi_bus_config_t busConfig = {};
    busConfig.sclk_io_num = PIN_TFT_SCLK;
    busConfig.mosi_io_num = PIN_TFT_MOSI;
    busConfig.miso_io_num = PIN_TFT_MISO;
    busConfig.quadwp_io_num = -1;
    busConfig.quadhd_io_num = -1;
    busConfig.max_transfer_sz = kScreenWidth * kProbeLines * static_cast<int>(sizeof(uint16_t));

    const esp_err_t err = spi_bus_initialize(kDisplaySpiHost, &busConfig, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

esp_err_t initPanelIo() {
    if (g_panelIo != nullptr) {
        return ESP_OK;
    }

    esp_lcd_panel_io_spi_config_t ioConfig = {};
    ioConfig.dc_gpio_num = PIN_TFT_DC;
    ioConfig.cs_gpio_num = PIN_TFT_CS;
    ioConfig.pclk_hz = kPixelClockHz;
    ioConfig.lcd_cmd_bits = 8;
    ioConfig.lcd_param_bits = 8;
    ioConfig.spi_mode = 0;
    ioConfig.trans_queue_depth = 4;

    return esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &ioConfig, &g_panelIo);
}

esp_err_t initPanel() {
    if (g_panel != nullptr) {
        return ESP_OK;
    }

    esp_lcd_panel_dev_config_t panelConfig = {};
    panelConfig.reset_gpio_num = PIN_TFT_RST;
    panelConfig.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panelConfig.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    panelConfig.bits_per_pixel = 16;

    return esp_lcd_new_panel_st7789(g_panelIo, &panelConfig, &g_panel);
}

esp_err_t applyPanelSetup() {
    esp_err_t err = esp_lcd_panel_reset(g_panel);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_init(g_panel);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t vcom = 0x1D;
    err = esp_lcd_panel_io_tx_param(g_panelIo, 0xBB, &vcom, sizeof(vcom));
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_invert_color(g_panel, true);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_mirror(g_panel, true, true);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_set_gap(g_panel, 0, kRotation2YGap);
    if (err != ESP_OK) {
        return err;
    }

    return esp_lcd_panel_disp_on_off(g_panel, true);
}

esp_err_t drawHorizontalBand(uint16_t* lineBuffer, int yStart, int yEnd, uint16_t color) {
    for (int i = 0; i < kScreenWidth * kProbeLines; ++i) {
        lineBuffer[i] = color;
    }

    for (int y = yStart; y < yEnd; y += kProbeLines) {
        const int h = ((y + kProbeLines) <= yEnd) ? kProbeLines : (yEnd - y);
        esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, y, kScreenWidth, y + h, lineBuffer);
        if (err == ESP_OK) {
            err = waitForPendingTransfers();
        }
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

}  // namespace

esp_err_t init() {
    if (g_initialized) {
        return ESP_OK;
    }

    esp_err_t err = configureBacklightGpio();
    if (err != ESP_OK) {
        LOG_E(TAG_DISPLAY, "Backlight GPIO init failed: %s", esp_err_to_name(err));
        return err;
    }
    setBacklight(false);

    err = initSpiBus();
    if (err != ESP_OK) {
        LOG_E(TAG_DISPLAY, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = initPanelIo();
    if (err != ESP_OK) {
        LOG_E(TAG_DISPLAY, "Panel IO init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = initPanel();
    if (err != ESP_OK) {
        LOG_E(TAG_DISPLAY, "ST7789 panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = applyPanelSetup();
    if (err != ESP_OK) {
        LOG_E(TAG_DISPLAY, "ST7789 setup failed: %s", esp_err_to_name(err));
        return err;
    }

    g_initialized = true;
    LOG_I(TAG_DISPLAY, "ST7789P3 IDF probe initialized (%dx%d, SPI%d @ %d Hz)",
          kScreenWidth,
          kScreenHeight,
          static_cast<int>(kDisplaySpiHost) + 1,
          kPixelClockHz);
    return ESP_OK;
}

bool isInitialized() {
    return g_initialized;
}

bool isBacklightOn() {
    return g_backlightOn;
}

esp_err_t setBacklight(bool enabled) {
    const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(PIN_TFT_BL), enabled ? 1 : 0);
    if (err == ESP_OK) {
        g_backlightOn = enabled;
    }
    return err;
}

esp_err_t drawRgb565Bitmap(int xStart, int yStart, int xEnd, int yEnd, const uint16_t* pixels) {
    if (!g_initialized || g_panel == nullptr || pixels == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xStart < 0 || yStart < 0 || xEnd > kScreenWidth || yEnd > kScreenHeight || xStart >= xEnd ||
        yStart >= yEnd) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_lcd_panel_draw_bitmap(g_panel, xStart, yStart, xEnd, yEnd, pixels);
}

esp_err_t waitForPendingTransfers() {
    if (g_panelIo == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_lcd_panel_io_tx_param(g_panelIo, -1, nullptr, 0);
}

esp_err_t drawBootProbe() {
    esp_err_t err = init();
    if (err != ESP_OK) {
        return err;
    }

    uint16_t* lineBuffer = static_cast<uint16_t*>(
        heap_caps_malloc(kScreenWidth * kProbeLines * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (lineBuffer == nullptr) {
        LOG_E(TAG_DISPLAY, "Failed to allocate LCD DMA probe buffer");
        return ESP_ERR_NO_MEM;
    }

    err = drawHorizontalBand(lineBuffer, 0, 60, rgb565(0, 0, 0));
    if (err == ESP_OK) {
        err = drawHorizontalBand(lineBuffer, 60, 120, rgb565(0, 34, 96));
    }
    if (err == ESP_OK) {
        err = drawHorizontalBand(lineBuffer, 120, 180, rgb565(0, 96, 64));
    }
    if (err == ESP_OK) {
        err = drawHorizontalBand(lineBuffer, 180, kScreenHeight, rgb565(96, 24, 24));
    }

    heap_caps_free(lineBuffer);

    if (err == ESP_OK) {
        setBacklight(true);
        LOG_I(TAG_DISPLAY, "Boot probe drawn");
    } else {
        LOG_E(TAG_DISPLAY, "Boot probe draw failed: %s", esp_err_to_name(err));
    }
    return err;
}

namespace {

// 程序化绘制的低电量电池图标几何参数（240x240 屏幕居中）
constexpr int kBatTipY0 = 56;
constexpr int kBatTipY1 = 68;
constexpr int kBatTipX0 = 105;
constexpr int kBatTipX1 = 135;
constexpr int kBatBodyY0 = 68;
constexpr int kBatBodyY1 = 196;
constexpr int kBatBodyX0 = 80;
constexpr int kBatBodyX1 = 160;
constexpr int kBatBorderThickness = 4;

// 一个像素是否属于"红色前景"
inline bool isLowBatteryFg(int x, int y) {
    // 电池"嘴"
    if (y >= kBatTipY0 && y < kBatTipY1 && x >= kBatTipX0 && x < kBatTipX1) {
        return true;
    }
    // 电池外框（4 条边）
    if (y >= kBatBodyY0 && y < kBatBodyY1 && x >= kBatBodyX0 && x < kBatBodyX1) {
        const bool onTopEdge = y < kBatBodyY0 + kBatBorderThickness;
        const bool onBottomEdge = y >= kBatBodyY1 - kBatBorderThickness;
        const bool onLeftEdge = x < kBatBodyX0 + kBatBorderThickness;
        const bool onRightEdge = x >= kBatBodyX1 - kBatBorderThickness;
        if (onTopEdge || onBottomEdge || onLeftEdge || onRightEdge) {
            return true;
        }
        // 体内一个粗的感叹号（垂直条 + 下方点）
        const int cx = (kBatBodyX0 + kBatBodyX1) / 2;
        const int barX0 = cx - 4;
        const int barX1 = cx + 4;
        if (x >= barX0 && x < barX1) {
            if (y >= 88 && y < 150) {
                return true;
            }
            if (y >= 162 && y < 178) {
                return true;
            }
        }
    }
    return false;
}

void paintLowBatteryRow(uint16_t* rowPixels, int y, bool iconVisible, uint16_t bg, uint16_t fg) {
    for (int x = 0; x < kScreenWidth; ++x) {
        rowPixels[x] = (iconVisible && isLowBatteryFg(x, y)) ? fg : bg;
    }
}

esp_err_t blitLowBatteryFrame(uint16_t* lineBuffer, bool iconVisible) {
    constexpr uint16_t bg = 0x0000;  // black
    const uint16_t fg = rgb565(220, 38, 38);  // red-500

    for (int y = 0; y < kScreenHeight; y += kProbeLines) {
        const int rows = ((y + kProbeLines) <= kScreenHeight) ? kProbeLines : (kScreenHeight - y);
        for (int r = 0; r < rows; ++r) {
            paintLowBatteryRow(lineBuffer + r * kScreenWidth, y + r, iconVisible, bg, fg);
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(g_panel, 0, y, kScreenWidth, y + rows, lineBuffer);
        if (err == ESP_OK) {
            err = waitForPendingTransfers();
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

bool chargeDetectHigh() {
    // 简单读一次 GPIO；早期门控阶段 sensors task 已经配置好引脚（pulldown 输入）。
    return gpio_get_level(static_cast<gpio_num_t>(PIN_CHARGE_DETECT)) != 0;
}

}  // namespace

esp_err_t drawLowBatteryFrame(bool iconVisible) {
    esp_err_t err = init();
    if (err != ESP_OK) {
        return err;
    }

    uint16_t* lineBuffer = static_cast<uint16_t*>(
        heap_caps_malloc(kScreenWidth * kProbeLines * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (lineBuffer == nullptr) {
        LOG_E(TAG_DISPLAY, "Failed to allocate LCD DMA low-battery buffer");
        return ESP_ERR_NO_MEM;
    }

    err = blitLowBatteryFrame(lineBuffer, iconVisible);
    heap_caps_free(lineBuffer);
    return err;
}

esp_err_t runLowBatteryShutdownScreen() {
    esp_err_t err = init();
    if (err != ESP_OK) {
        return err;
    }

    // 第一帧先把图标打到屏上再开背光，避免用户看到上一帧残留或彩条。
    err = drawLowBatteryFrame(true);
    if (err != ESP_OK) {
        return err;
    }
    setBacklight(true);
    LOG_I(TAG_DISPLAY, "Low battery shutdown screen entered");

    const uint64_t startUs = esp_timer_get_time();
    const uint64_t deadlineUs = startUs + static_cast<uint64_t>(BatteryConfig::kEarlyShutdownDisplayMs) * 1000ULL;
    bool iconVisible = true;
    uint64_t nextBlinkUs = startUs + static_cast<uint64_t>(BatteryConfig::kEarlyShutdownBlinkMs) * 1000ULL;

    while (true) {
        const uint64_t nowUs = esp_timer_get_time();
        if (chargeDetectHigh()) {
            LOG_I(TAG_DISPLAY, "Charger detected during low-battery screen, resuming boot");
            // 把图标显示态恢复到 visible 再退出，避免最后一帧黑屏让用户以为没启动。
            (void)drawLowBatteryFrame(true);
            return ESP_OK;
        }
        if (nowUs >= deadlineUs) {
            break;
        }
        if (nowUs >= nextBlinkUs) {
            iconVisible = !iconVisible;
            (void)drawLowBatteryFrame(iconVisible);
            nextBlinkUs = nowUs + static_cast<uint64_t>(BatteryConfig::kEarlyShutdownBlinkMs) * 1000ULL;
        }
        vTaskDelay(pdMS_TO_TICKS(BatteryConfig::kEarlyShutdownPollMs));
    }

    enterDeepSleepForLowBattery("early-gate");
}

[[noreturn]] void enterDeepSleepForLowBattery(const char* reason) {
    LOG_W(TAG_DISPLAY, "Entering deep sleep (reason=%s)", reason != nullptr ? reason : "low_battery");
    setBacklight(false);
    // 给 LOG 输出留点时间打出去
    esp_rom_delay_us(50000);

    // ESP32-S3 ext1 唤醒源只支持 RTC IO（GPIO 0-21），PIN_CHARGE_DETECT=GPIO37 不在此列。
    // 改用 timer wakeup 周期苏醒，重走 app_main 早期门控判断电压 + GPIO37 充电状态。
    (void)esp_sleep_enable_timer_wakeup(BatteryConfig::kDeepSleepTimerUs);
    esp_deep_sleep_start();
    // 不返回。但加 while(true) 给编译器安静和保险。
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace AppIdfDisplay
