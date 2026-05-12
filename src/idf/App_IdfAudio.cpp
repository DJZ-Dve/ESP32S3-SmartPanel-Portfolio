#include "App_IdfAudio.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "App_FlashGuard.h"
#include "App_IdfFilesystem.h"
#include "App_Log.h"
#include "Pin_Config.h"
#include "audio_cues_data.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace AppIdfAudio {
namespace {

constexpr const char* TAG_AUDIO_IDF = "IDF_AUDIO";
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr uint32_t kI2cFrequencyHz = 100000;
constexpr uint32_t kI2cTimeoutMs = 100;
constexpr uint8_t kDefaultVolume = 70;
constexpr uint32_t kI2sDmaFrameCount = 160;
constexpr uint32_t kI2sDmaDescCount = 6;
constexpr int kSineAmplitude = 16000;
constexpr size_t kCueIoBufferSize = 512;
constexpr size_t kManifestMaxBytes = 4096;
constexpr size_t kGeneratedCueChunkBytes = 512;

constexpr uint8_t ES8311_RESET_REG00 = 0x00;
constexpr uint8_t ES8311_CLK_MANAGER_REG01 = 0x01;
constexpr uint8_t ES8311_CLK_MANAGER_REG02 = 0x02;
constexpr uint8_t ES8311_CLK_MANAGER_REG03 = 0x03;
constexpr uint8_t ES8311_CLK_MANAGER_REG04 = 0x04;
constexpr uint8_t ES8311_CLK_MANAGER_REG05 = 0x05;
constexpr uint8_t ES8311_CLK_MANAGER_REG06 = 0x06;
constexpr uint8_t ES8311_CLK_MANAGER_REG07 = 0x07;
constexpr uint8_t ES8311_CLK_MANAGER_REG08 = 0x08;
constexpr uint8_t ES8311_SDPIN_REG09 = 0x09;
constexpr uint8_t ES8311_SDPOUT_REG0A = 0x0A;
constexpr uint8_t ES8311_SYSTEM_REG0B = 0x0B;
constexpr uint8_t ES8311_SYSTEM_REG0C = 0x0C;
constexpr uint8_t ES8311_SYSTEM_REG0D = 0x0D;
constexpr uint8_t ES8311_SYSTEM_REG0E = 0x0E;
constexpr uint8_t ES8311_SYSTEM_REG10 = 0x10;
constexpr uint8_t ES8311_SYSTEM_REG11 = 0x11;
constexpr uint8_t ES8311_SYSTEM_REG12 = 0x12;
constexpr uint8_t ES8311_SYSTEM_REG13 = 0x13;
constexpr uint8_t ES8311_SYSTEM_REG14 = 0x14;
constexpr uint8_t ES8311_ADC_REG15 = 0x15;
constexpr uint8_t ES8311_ADC_REG16 = 0x16;
constexpr uint8_t ES8311_ADC_REG17 = 0x17;
constexpr uint8_t ES8311_ADC_REG1B = 0x1B;
constexpr uint8_t ES8311_ADC_REG1C = 0x1C;
constexpr uint8_t ES8311_DAC_REG31 = 0x31;
constexpr uint8_t ES8311_DAC_REG32 = 0x32;
constexpr uint8_t ES8311_DAC_REG37 = 0x37;
constexpr uint8_t ES8311_GP_REG45 = 0x45;
constexpr uint8_t ES8311_CHD1_REGFD = 0xFD;
constexpr uint8_t ES8311_CHD2_REGFE = 0xFE;
constexpr uint8_t ES8311_CHVER_REGFF = 0xFF;

i2c_master_bus_handle_t g_i2cBus = nullptr;
i2c_master_dev_handle_t g_codecDevice = nullptr;
i2s_chan_handle_t g_txChannel = nullptr;
i2s_chan_handle_t g_rxChannel = nullptr;
SemaphoreHandle_t g_i2cMutex = nullptr;
SemaphoreHandle_t g_i2sMutex = nullptr;
Snapshot g_snapshot;

class MutexLock {
public:
    explicit MutexLock(SemaphoreHandle_t mutex, TickType_t timeoutTicks = portMAX_DELAY) : _mutex(mutex) {
        if (_mutex != nullptr) {
            _locked = xSemaphoreTake(_mutex, timeoutTicks) == pdTRUE;
        }
    }

    ~MutexLock() {
        if (_locked && _mutex != nullptr) {
            xSemaphoreGive(_mutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    SemaphoreHandle_t _mutex = nullptr;
    bool _locked = false;
};

esp_err_t setLastError(esp_err_t err) {
    g_snapshot.lastError = err;
    return err;
}

esp_err_t ensureMutexes() {
    if (g_i2cMutex == nullptr) {
        g_i2cMutex = xSemaphoreCreateMutex();
        if (g_i2cMutex == nullptr) {
            return setLastError(ESP_ERR_NO_MEM);
        }
    }
    if (g_i2sMutex == nullptr) {
        g_i2sMutex = xSemaphoreCreateMutex();
        if (g_i2sMutex == nullptr) {
            return setLastError(ESP_ERR_NO_MEM);
        }
    }
    return ESP_OK;
}

esp_err_t configurePaGpio() {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << PIN_PA_EN;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    return setPaEnabled(false);
}

esp_err_t initI2cBus() {
    if (g_i2cBus != nullptr && g_codecDevice != nullptr) {
        return ESP_OK;
    }

    if (g_i2cBus == nullptr) {
        i2c_master_bus_config_t busConfig = {};
        busConfig.i2c_port = kI2cPort;
        busConfig.sda_io_num = static_cast<gpio_num_t>(PIN_I2C_SDA);
        busConfig.scl_io_num = static_cast<gpio_num_t>(PIN_I2C_SCL);
        busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
        busConfig.glitch_ignore_cnt = 7;
        busConfig.intr_priority = 0;
        busConfig.trans_queue_depth = 0;
        busConfig.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&busConfig, &g_i2cBus);
        if (err == ESP_ERR_INVALID_STATE) {
            err = i2c_master_get_bus_handle(kI2cPort, &g_i2cBus);
        }
        if (err != ESP_OK) {
            LOG_E(TAG_AUDIO_IDF, "I2C bus init failed: %s", esp_err_to_name(err));
            return setLastError(err);
        }
    }

    if (g_codecDevice == nullptr) {
        i2c_device_config_t deviceConfig = {};
        deviceConfig.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        deviceConfig.device_address = kEs8311Address;
        deviceConfig.scl_speed_hz = kI2cFrequencyHz;
        deviceConfig.scl_wait_us = 0;

        const esp_err_t err = i2c_master_bus_add_device(g_i2cBus, &deviceConfig, &g_codecDevice);
        if (err != ESP_OK) {
            LOG_E(TAG_AUDIO_IDF, "ES8311 I2C device add failed: %s", esp_err_to_name(err));
            return setLastError(err);
        }
    }

    return ESP_OK;
}

esp_err_t probeCodec() {
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < 10; ++attempt) {
        err = i2c_master_probe(g_i2cBus, kEs8311Address, kI2cTimeoutMs);
        if (err == ESP_OK) {
            g_snapshot.codecFound = true;
            LOG_I(TAG_AUDIO_IDF, "ES8311 responded at 0x%02x attempt=%d", kEs8311Address, attempt + 1);
            return ESP_OK;
        }
        LOG_W(TAG_AUDIO_IDF, "ES8311 probe failed attempt=%d/10: %s", attempt + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    g_snapshot.codecFound = false;
    return setLastError(err);
}

esp_err_t writeReg(uint8_t reg, uint8_t value) {
    if (g_codecDevice == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(g_codecDevice, payload, sizeof(payload), kI2cTimeoutMs);
}

esp_err_t readReg(uint8_t reg, uint8_t* value) {
    if (g_codecDevice == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(g_codecDevice, &reg, sizeof(reg), value, sizeof(*value), kI2cTimeoutMs);
}

esp_err_t writeRegChecked(uint8_t reg, uint8_t value, const char* step) {
    const esp_err_t err = writeReg(reg, value);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "ES8311 write %s reg=0x%02x val=0x%02x failed: %s",
              step ? step : "reg",
              reg,
              value,
              esp_err_to_name(err));
    }
    return err;
}

esp_err_t updateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask, const char* step) {
    uint8_t value = 0;
    esp_err_t err = readReg(reg, &value);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "ES8311 read %s reg=0x%02x failed: %s", step ? step : "reg", reg, esp_err_to_name(err));
        return err;
    }

    value = static_cast<uint8_t>((value & ~clearMask) | setMask);
    return writeRegChecked(reg, value, step);
}

esp_err_t codecInit16k() {
    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    esp_err_t err = ESP_OK;
    err = writeRegChecked(ES8311_CLK_MANAGER_REG01, 0x30, "init01");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG02, 0x00, "init02");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG03, 0x10, "init03");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_ADC_REG16, 0x24, "adc-gain");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG04, 0x10, "init04");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG05, 0x00, "init05");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG0B, 0x00, "sys0b");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG0C, 0x00, "sys0c");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG10, 0x1F, "sys10");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG11, 0x7F, "sys11");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_RESET_REG00, 0x80, "reset");
    if (err != ESP_OK) return setLastError(err);

    err = updateReg(ES8311_RESET_REG00, 0x40, 0x00, "slave-mode");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG01, 0x3F, "clk-enable");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_CLK_MANAGER_REG01, 0x80, 0x00, "mclk-source");
    if (err != ESP_OK) return setLastError(err);

    err = writeRegChecked(ES8311_CLK_MANAGER_REG02, 0x00, "clk-coeff02");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG05, 0x00, "clk-coeff05");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG03, 0x10, "clk-coeff03");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG04, 0x10, "clk-coeff04");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG07, 0x00, "lrck-h");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG08, 0xFF, "lrck-l");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_CLK_MANAGER_REG06, 0x03, "bclk-div");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_CLK_MANAGER_REG01, 0x40, 0x00, "mclk-normal");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_CLK_MANAGER_REG06, 0x20, 0x00, "bclk-normal");
    if (err != ESP_OK) return setLastError(err);

    err = updateReg(ES8311_SDPIN_REG09, 0x03, 0x0C, "dac-i2s-16bit");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_SDPOUT_REG0A, 0x03, 0x0C, "adc-i2s-16bit");
    if (err != ESP_OK) return setLastError(err);

    err = writeRegChecked(ES8311_SYSTEM_REG13, 0x10, "sys13");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_ADC_REG1B, 0x0A, "adc-hpf1");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_ADC_REG1C, 0x6A, "adc-hpf2");
    if (err != ESP_OK) return setLastError(err);

    return ESP_OK;
}

esp_err_t initI2s() {
    MutexLock lock(g_i2sMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    if (g_txChannel != nullptr && g_rxChannel != nullptr) {
        return ESP_OK;
    }

    i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
    channelConfig.dma_desc_num = kI2sDmaDescCount;
    channelConfig.dma_frame_num = kI2sDmaFrameCount;
    channelConfig.auto_clear_after_cb = true;
    channelConfig.auto_clear_before_cb = false;

    esp_err_t err = i2s_new_channel(&channelConfig, &g_txChannel, &g_rxChannel);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "I2S channel allocation failed: %s", esp_err_to_name(err));
        g_txChannel = nullptr;
        g_rxChannel = nullptr;
        return setLastError(err);
    }

    i2s_std_clk_config_t clkConfig = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz);
    clkConfig.clk_src = I2S_CLK_SRC_DEFAULT;
    clkConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_slot_config_t slotConfig =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slotConfig.slot_mask = I2S_STD_SLOT_LEFT;

    i2s_std_config_t stdConfig = {};
    stdConfig.clk_cfg = clkConfig;
    stdConfig.slot_cfg = slotConfig;
    stdConfig.gpio_cfg.mclk = static_cast<gpio_num_t>(PIN_I2S_MCLK);
    stdConfig.gpio_cfg.bclk = static_cast<gpio_num_t>(PIN_I2S_BCLK);
    stdConfig.gpio_cfg.ws = static_cast<gpio_num_t>(PIN_I2S_LRCK);
    stdConfig.gpio_cfg.dout = static_cast<gpio_num_t>(PIN_I2S_DOUT);
    stdConfig.gpio_cfg.din = static_cast<gpio_num_t>(PIN_I2S_DIN);

    err = i2s_channel_init_std_mode(g_txChannel, &stdConfig);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "I2S TX std init failed: %s", esp_err_to_name(err));
        return setLastError(err);
    }
    err = i2s_channel_init_std_mode(g_rxChannel, &stdConfig);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "I2S RX std init failed: %s", esp_err_to_name(err));
        return setLastError(err);
    }

    err = i2s_channel_enable(g_txChannel);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "I2S TX enable failed: %s", esp_err_to_name(err));
        return setLastError(err);
    }
    err = i2s_channel_enable(g_rxChannel);
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "I2S RX enable failed: %s", esp_err_to_name(err));
        return setLastError(err);
    }

    g_snapshot.sampleRateHz = kSampleRateHz;
    LOG_I(TAG_AUDIO_IDF,
          "I2S RX/TX ready (%u Hz, 16-bit mono left, dma=%u x %u frames)",
          static_cast<unsigned>(kSampleRateHz),
          static_cast<unsigned>(kI2sDmaDescCount),
          static_cast<unsigned>(kI2sDmaFrameCount));
    return ESP_OK;
}

uint8_t volumeToReg(uint8_t volume) {
    if (volume == 0) {
        return 0;
    }
    const double scaled = 191.0 * log10(9.0 * static_cast<double>(volume) / 100.0 + 1.0) / log10(10.0);
    if (scaled <= 0.0) {
        return 0;
    }
    if (scaled >= 191.0) {
        return 191;
    }
    return static_cast<uint8_t>(scaled + 0.5);
}

uint8_t gainToReg(uint8_t gain) {
    if (gain > 100) {
        gain = 100;
    }
    return static_cast<uint8_t>((static_cast<unsigned>(gain) * 7U + 50U) / 100U);
}

esp_err_t codecStartDecode() {
    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    esp_err_t err = updateReg(ES8311_SDPIN_REG09, 0x40, 0x00, "dac-enable");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_SDPOUT_REG0A, 0x40, 0x40, "adc-initial-disabled");
    if (err != ESP_OK) return setLastError(err);

    err = writeRegChecked(ES8311_ADC_REG17, 0xBF, "adc-digital-0db");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG0E, 0x02, "sys0e-start");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG12, 0x00, "dac-enable-logic");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG14, 0x1A, "analog-path");
    if (err != ESP_OK) return setLastError(err);
    err = updateReg(ES8311_SYSTEM_REG14, 0x40, 0x00, "analog-mic");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_SYSTEM_REG0D, 0x01, "power-up");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_ADC_REG15, 0x40, "adc-ramp");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_DAC_REG37, 0x48, "dac-ramp");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_GP_REG45, 0x00, "gp-control");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_DAC_REG31, 0x00, "dac-unmute");
    if (err != ESP_OK) return setLastError(err);
    err = writeRegChecked(ES8311_DAC_REG32, volumeToReg(g_snapshot.volume), "dac-volume");
    if (err != ESP_OK) return setLastError(err);

    g_snapshot.micEnabled = false;
    return ESP_OK;
}

esp_err_t readChipIds() {
    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    uint8_t value = 0;
    esp_err_t err = readReg(ES8311_CHD1_REGFD, &value);
    if (err == ESP_OK) {
        g_snapshot.chipId1 = value;
    }
    err = readReg(ES8311_CHD2_REGFE, &value);
    if (err == ESP_OK) {
        g_snapshot.chipId2 = value;
    }
    err = readReg(ES8311_CHVER_REGFF, &value);
    if (err == ESP_OK) {
        g_snapshot.chipVersion = value;
    }
    return ESP_OK;
}

esp_err_t disableI2sChannels() {
    esp_err_t result = ESP_OK;
    if (g_txChannel != nullptr) {
        const esp_err_t err = i2s_channel_disable(g_txChannel);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            result = err;
        }
    }
    if (g_rxChannel != nullptr) {
        const esp_err_t err = i2s_channel_disable(g_rxChannel);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            result = err;
        }
    }
    return result;
}

bool isSupportedCueName(const char* cueName) {
    return cueName != nullptr &&
           (strcmp(cueName, "wake_ack") == 0 || strcmp(cueName, "settings_done") == 0 ||
            strcmp(cueName, "done") == 0 || strcmp(cueName, "op_failed") == 0 ||
            strcmp(cueName, "network_failed") == 0 || strcmp(cueName, "not_understood") == 0 ||
            strcmp(cueName, "chitchat_unsupported") == 0 ||
            strcmp(cueName, "device_unsupported") == 0 ||
            strcmp(cueName, "scene_not_found") == 0 ||
            strcmp(cueName, "learn_press_key") == 0 ||
            strcmp(cueName, "learn_press_again") == 0 ||
            strcmp(cueName, "learn_say_name") == 0);
}

const AudioCueAsset* findGeneratedCue(const char* cueName) {
    if (cueName == nullptr || cueName[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < kAudioCueAssetCount; ++i) {
        if (strcmp(g_audio_cue_assets[i].name, cueName) == 0) {
            return &g_audio_cue_assets[i];
        }
    }
    return nullptr;
}

bool isSafeLocalCuePath(const char* resourcePath, const char* cueName) {
    if (resourcePath == nullptr || cueName == nullptr || resourcePath[0] == '\0') {
        return false;
    }

    char expectedPrefix[48] = {};
    snprintf(expectedPrefix, sizeof(expectedPrefix), "/audio_cues/%s/", cueName);
    const size_t pathLen = strlen(resourcePath);
    return strncmp(resourcePath, expectedPrefix, strlen(expectedPrefix)) == 0 &&
           strstr(resourcePath, "..") == nullptr && pathLen > 4 &&
           strcmp(resourcePath + pathLen - 4, ".pcm") == 0;
}

char* readManifestJson() {
    char manifestPath[160] = {};
    if (AppIdfFilesystem::makeResourcePath("/audio_cues/manifest.json", manifestPath, sizeof(manifestPath)) != ESP_OK) {
        return nullptr;
    }

    FILE* file = fopen(manifestPath, "rb");
    if (file == nullptr) {
        return nullptr;
    }

    char* buffer = static_cast<char*>(
        heap_caps_malloc(kManifestMaxBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        buffer = static_cast<char*>(heap_caps_malloc(kManifestMaxBytes + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buffer == nullptr) {
        fclose(file);
        return nullptr;
    }

    const size_t bytesRead = fread(buffer, 1, kManifestMaxBytes, file);
    fclose(file);
    buffer[bytesRead] = '\0';
    if (bytesRead == 0 || bytesRead >= kManifestMaxBytes) {
        heap_caps_free(buffer);
        return nullptr;
    }
    return buffer;
}

esp_err_t selectCueResourcePath(const char* cueName, char* outResourcePath, size_t outResourcePathSize) {
    if (!isSupportedCueName(cueName) || outResourcePath == nullptr || outResourcePathSize == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    outResourcePath[0] = '\0';

    char* manifestJson = readManifestJson();
    if (manifestJson == nullptr) {
        LOG_W(TAG_AUDIO_IDF, "LittleFS audio manifest missing or too large");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON* root = cJSON_Parse(manifestJson);
    heap_caps_free(manifestJson);
    if (root == nullptr) {
        LOG_W(TAG_AUDIO_IDF, "LittleFS audio manifest parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* cues = cJSON_GetObjectItemCaseSensitive(root, "cues");
    const cJSON* variants = cJSON_IsObject(cues) ? cJSON_GetObjectItemCaseSensitive(cues, cueName) : nullptr;
    const int variantCount = cJSON_IsArray(variants) ? cJSON_GetArraySize(variants) : 0;
    if (variantCount <= 0) {
        cJSON_Delete(root);
        LOG_W(TAG_AUDIO_IDF, "LittleFS cue has no variants: %s", cueName);
        return ESP_ERR_NOT_FOUND;
    }

    const int startIndex = static_cast<int>(esp_random() % static_cast<uint32_t>(variantCount));
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (int attempt = 0; attempt < variantCount; ++attempt) {
        const int index = (startIndex + attempt) % variantCount;
        const cJSON* item = cJSON_GetArrayItem(variants, index);
        const cJSON* file = cJSON_IsObject(item) ? cJSON_GetObjectItemCaseSensitive(item, "file") : nullptr;
        const char* fileField = (cJSON_IsString(file) && file->valuestring != nullptr) ? file->valuestring : "";

        char candidate[128] = {};
        if (fileField[0] == '/') {
            snprintf(candidate, sizeof(candidate), "%s", fileField);
        } else if (fileField[0] != '\0') {
            snprintf(candidate, sizeof(candidate), "/audio_cues/%s/%s", cueName, fileField);
        }

        if (!isSafeLocalCuePath(candidate, cueName)) {
            LOG_W(TAG_AUDIO_IDF, "Ignoring unsafe cue path: %s", candidate);
            continue;
        }
        if (!AppIdfFilesystem::resourceExists(candidate)) {
            LOG_W(TAG_AUDIO_IDF, "LittleFS cue file missing: %s", candidate);
            continue;
        }

        const int written = snprintf(outResourcePath, outResourcePathSize, "%s", candidate);
        result = (written > 0 && static_cast<size_t>(written) < outResourcePathSize) ? ESP_OK : ESP_ERR_NO_MEM;
        break;
    }

    cJSON_Delete(root);
    return result;
}

}  // namespace

esp_err_t start() {
    if (g_snapshot.started) {
        return ESP_OK;
    }

    g_snapshot.volume = kDefaultVolume;
    g_snapshot.sampleRateHz = kSampleRateHz;

    esp_err_t err = ensureMutexes();
    if (err != ESP_OK) {
        return err;
    }
    err = configurePaGpio();
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "PA GPIO init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = initI2cBus();
    if (err != ESP_OK) {
        return err;
    }
    err = probeCodec();
    if (err != ESP_OK) {
        LOG_E(TAG_AUDIO_IDF, "ES8311 probe failed, audio disabled: %s", esp_err_to_name(err));
        uint8_t foundAddrs[16] = {};
        size_t foundCount = 0;
        if (scanI2c(foundAddrs, sizeof(foundAddrs), &foundCount) == ESP_OK) {
            if (foundCount == 0) {
                LOG_E(TAG_AUDIO_IDF, "I2C scan: no devices found on SDA=%d SCL=%d", PIN_I2C_SDA, PIN_I2C_SCL);
            } else {
                LOG_W(TAG_AUDIO_IDF, "I2C scan: %u device(s) found:", static_cast<unsigned>(foundCount));
                for (size_t i = 0; i < foundCount && i < sizeof(foundAddrs); ++i) {
                    LOG_W(TAG_AUDIO_IDF, "  addr=0x%02x%s", foundAddrs[i],
                          foundAddrs[i] == kEs8311Address ? " <-- ES8311 expected here" : "");
                }
            }
        }
        return err;
    }
    readChipIds();

    err = codecInit16k();
    if (err != ESP_OK) {
        return err;
    }
    err = initI2s();
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(40));
    err = codecStartDecode();
    if (err != ESP_OK) {
        return err;
    }
    err = disableMicChannel();
    if (err != ESP_OK) {
        return err;
    }

    g_snapshot.started = true;
    g_snapshot.lastError = ESP_OK;
    LOG_I(TAG_AUDIO_IDF,
          "ES8311 IDF audio ready chip=%02x %02x ver=%02x volume=%u",
          g_snapshot.chipId1,
          g_snapshot.chipId2,
          g_snapshot.chipVersion,
          static_cast<unsigned>(g_snapshot.volume));
    return ESP_OK;
}

esp_err_t restart(const char* reason) {
    LOG_W(TAG_AUDIO_IDF, "Restarting IDF audio path (%s)", reason ? reason : "manual");
    setPaEnabled(false);
    MutexLock lock(g_i2sMutex, pdMS_TO_TICKS(2000));
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    const esp_err_t disableErr = disableI2sChannels();
    vTaskDelay(pdMS_TO_TICKS(40));
    if (g_txChannel != nullptr) {
        const esp_err_t err = i2s_channel_enable(g_txChannel);
        if (err != ESP_OK) {
            return setLastError(err);
        }
    }
    if (g_rxChannel != nullptr) {
        const esp_err_t err = i2s_channel_enable(g_rxChannel);
        if (err != ESP_OK) {
            return setLastError(err);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(40));

    esp_err_t err = codecStartDecode();
    if (err == ESP_OK) {
        err = disableMicChannel();
    }
    LOG_I(TAG_AUDIO_IDF,
          "Audio restart result disable=%s codec=%s",
          esp_err_to_name(disableErr),
          esp_err_to_name(err));
    return err;
}

bool isStarted() {
    return g_snapshot.started;
}

bool isCodecFound() {
    return g_snapshot.codecFound;
}

Snapshot snapshot() {
    return g_snapshot;
}

esp_err_t setVolume(uint8_t volume) {
    if (volume > 100) {
        volume = 100;
    }
    g_snapshot.volume = volume;

    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }
    const esp_err_t err = writeRegChecked(ES8311_DAC_REG32, volumeToReg(volume), "set-volume");
    if (err == ESP_OK) {
        LOG_I(TAG_AUDIO_IDF, "Volume set to %u", static_cast<unsigned>(volume));
    }
    return setLastError(err);
}

uint8_t getVolume() {
    return g_snapshot.volume;
}

esp_err_t setMicGain(uint8_t gain) {
    if (gain > 100) {
        gain = 100;
    }
    g_snapshot.micGain = gain;
    const uint8_t regValue = gainToReg(gain);

    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }
    const esp_err_t err = writeRegChecked(ES8311_ADC_REG16, regValue, "set-mic-gain");
    if (err == ESP_OK) {
        LOG_I(TAG_AUDIO_IDF,
              "Mic gain set to %u (reg 0x%02x)",
              static_cast<unsigned>(gain),
              static_cast<unsigned>(regValue));
    }
    return setLastError(err);
}

uint8_t getMicGain() {
    return g_snapshot.micGain;
}

esp_err_t enableMicChannel() {
    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    esp_err_t err = writeRegChecked(ES8311_SDPOUT_REG0A, 0x01, "mic-enable");
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = writeRegChecked(ES8311_ADC_REG17, 0xBF, "adc-digital-0db");
    if (err == ESP_OK) {
        g_snapshot.micEnabled = true;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    return setLastError(err);
}

esp_err_t disableMicChannel() {
    MutexLock lock(g_i2cMutex);
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }

    const esp_err_t err = writeRegChecked(ES8311_SDPOUT_REG0A, 0x41, "mic-disable");
    if (err == ESP_OK) {
        g_snapshot.micEnabled = false;
    }
    return setLastError(err);
}

bool isMicChannelEnabled() {
    return g_snapshot.micEnabled;
}

esp_err_t setPaEnabled(bool enabled) {
    const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(PIN_PA_EN), enabled ? 1 : 0);
    if (err == ESP_OK) {
        g_snapshot.paEnabled = enabled;
    }
    return setLastError(err);
}

bool isPaEnabled() {
    return g_snapshot.paEnabled;
}

esp_err_t writePcm(const void* data, size_t len, size_t* bytesWritten, uint32_t timeoutMs) {
    if (data == nullptr || len == 0 || g_txChannel == nullptr) {
        return setLastError(ESP_ERR_INVALID_ARG);
    }
    MutexLock lock(g_i2sMutex, pdMS_TO_TICKS(timeoutMs));
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }
    const esp_err_t err = i2s_channel_write(g_txChannel, data, len, bytesWritten, timeoutMs);
    return setLastError(err);
}

esp_err_t readPcm(void* data, size_t len, size_t* bytesRead, uint32_t timeoutMs) {
    if (data == nullptr || len == 0 || g_rxChannel == nullptr) {
        return setLastError(ESP_ERR_INVALID_ARG);
    }
    MutexLock lock(g_i2sMutex, pdMS_TO_TICKS(timeoutMs));
    if (!lock.locked()) {
        return setLastError(ESP_ERR_TIMEOUT);
    }
    const esp_err_t err = i2s_channel_read(g_rxChannel, data, len, bytesRead, timeoutMs);
    return setLastError(err);
}

esp_err_t writeSilence(uint32_t durationMs) {
    if (!g_snapshot.started) {
        return setLastError(ESP_ERR_INVALID_STATE);
    }

    int16_t silence[160] = {};
    const uint32_t chunks = (durationMs + 9) / 10;
    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < chunks; ++i) {
        size_t written = 0;
        err = writePcm(silence, sizeof(silence), &written, 500);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t playSineTest(uint32_t durationMs, uint32_t frequencyHz) {
    if (!g_snapshot.started) {
        const esp_err_t err = start();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (frequencyHz == 0 || durationMs == 0) {
        return setLastError(ESP_ERR_INVALID_ARG);
    }

    esp_err_t err = setPaEnabled(true);
    if (err != ESP_OK) {
        return err;
    }
    writeSilence(50);
    setVolume(g_snapshot.volume);

    int16_t samples[160] = {};
    const uint32_t totalSamples = (kSampleRateHz * durationMs) / 1000U;
    uint32_t samplesGenerated = 0;
    while (samplesGenerated < totalSamples) {
        const uint32_t remaining = totalSamples - samplesGenerated;
        const uint32_t chunkSamples = remaining < (sizeof(samples) / sizeof(samples[0]))
                                          ? remaining
                                          : static_cast<uint32_t>(sizeof(samples) / sizeof(samples[0]));
        for (uint32_t i = 0; i < chunkSamples; ++i) {
            const double phase = 2.0 * M_PI * static_cast<double>(frequencyHz) *
                                 static_cast<double>(samplesGenerated + i) / static_cast<double>(kSampleRateHz);
            samples[i] = static_cast<int16_t>(kSineAmplitude * sin(phase));
        }
        if (chunkSamples < sizeof(samples) / sizeof(samples[0])) {
            memset(samples + chunkSamples, 0, (sizeof(samples) / sizeof(samples[0]) - chunkSamples) * sizeof(samples[0]));
        }
        size_t written = 0;
        err = writePcm(samples, chunkSamples * sizeof(samples[0]), &written, 1000);
        if (err != ESP_OK) {
            setPaEnabled(false);
            return err;
        }
        samplesGenerated += chunkSamples;
    }

    writeSilence(100);
    vTaskDelay(pdMS_TO_TICKS(120));
    setPaEnabled(false);
    LOG_I(TAG_AUDIO_IDF,
          "Sine test completed duration=%u ms freq=%u Hz",
          static_cast<unsigned>(durationMs),
          static_cast<unsigned>(frequencyHz));
    return ESP_OK;
}

esp_err_t playGeneratedCue(const char* cueName, uint32_t timeoutMs) {
    if (cueName == nullptr || cueName[0] == '\0') {
        return setLastError(ESP_ERR_INVALID_ARG);
    }
    if (AppFlashGuard::isActive()) {
        LOG_D(TAG_AUDIO_IDF, "Skipping generated cue while flash guard is active");
        return setLastError(ESP_ERR_INVALID_STATE);
    }
    if (!g_snapshot.started) {
        const esp_err_t startErr = start();
        if (startErr != ESP_OK) {
            return startErr;
        }
    }

    const AudioCueAsset* asset = findGeneratedCue(cueName);
    if (asset == nullptr || asset->data == nullptr || asset->len == 0) {
        return setLastError(ESP_ERR_NOT_FOUND);
    }

    esp_err_t err = setPaEnabled(true);
    if (err == ESP_OK) {
        writeSilence(50);
        err = setVolume(g_snapshot.volume);
    }

    const TickType_t startTick = xTaskGetTickCount();
    size_t offset = 0;
    while (err == ESP_OK && offset < asset->len) {
        if (timeoutMs > 0) {
            const uint32_t elapsedMs = static_cast<uint32_t>((xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS);
            if (elapsedMs > timeoutMs) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
        }

        size_t chunkLen = asset->len - offset;
        if (chunkLen > kGeneratedCueChunkBytes) {
            chunkLen = kGeneratedCueChunkBytes;
        }
        chunkLen &= ~static_cast<size_t>(1);
        if (chunkLen == 0) {
            break;
        }

        size_t written = 0;
        err = writePcm(asset->data + offset, chunkLen, &written, 1000);
        if (err != ESP_OK) {
            break;
        }
        offset += chunkLen;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    writeSilence(100);
    vTaskDelay(pdMS_TO_TICKS(120));
    setPaEnabled(false);

    if (err == ESP_OK) {
        LOG_I(TAG_AUDIO_IDF,
              "Played generated cue %s (%u bytes, %u ms)",
              asset->name,
              static_cast<unsigned>(asset->len),
              static_cast<unsigned>(asset->durationMs));
    } else {
        LOG_W(TAG_AUDIO_IDF, "Generated cue playback failed %s: %s", cueName, esp_err_to_name(err));
    }
    return setLastError(err);
}

esp_err_t playLocalCue(const char* cueName, uint32_t timeoutMs) {
    char normalizedCue[24] = {};
    if (cueName != nullptr) {
        size_t i = 0;
        while (cueName[i] != '\0' && i + 1 < sizeof(normalizedCue)) {
            normalizedCue[i] = static_cast<char>(tolower(static_cast<unsigned char>(cueName[i])));
            ++i;
        }
    }

    if (normalizedCue[0] == '\0' || strcmp(normalizedCue, "none") == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (AppFlashGuard::isActive()) {
        LOG_D(TAG_AUDIO_IDF, "Skipping LittleFS cue while flash guard is active");
        return ESP_ERR_INVALID_STATE;
    }
    if (!AppIdfFilesystem::isResourcePartitionMounted()) {
        const esp_err_t mountErr = AppIdfFilesystem::mountResourcePartition(true);
        if (mountErr != ESP_OK) {
            return setLastError(mountErr);
        }
    }
    if (!g_snapshot.started) {
        const esp_err_t startErr = start();
        if (startErr != ESP_OK) {
            return startErr;
        }
    }

    char resourcePath[128] = {};
    esp_err_t err = selectCueResourcePath(normalizedCue, resourcePath, sizeof(resourcePath));
    if (err != ESP_OK) {
        return setLastError(err);
    }

    char fullPath[160] = {};
    err = AppIdfFilesystem::makeResourcePath(resourcePath, fullPath, sizeof(fullPath));
    if (err != ESP_OK) {
        return setLastError(err);
    }

    FILE* pcm = fopen(fullPath, "rb");
    if (pcm == nullptr) {
        return setLastError(ESP_ERR_NOT_FOUND);
    }

    err = setPaEnabled(true);
    if (err == ESP_OK) {
        writeSilence(50);
        err = setVolume(g_snapshot.volume);
    }

    uint8_t ioBuffer[kCueIoBufferSize] = {};
    size_t totalBytes = 0;
    const TickType_t startTick = xTaskGetTickCount();
    while (err == ESP_OK) {
        if (timeoutMs > 0) {
            const uint32_t elapsedMs = static_cast<uint32_t>((xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS);
            if (elapsedMs > timeoutMs) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
        }

        const size_t bytesRead = fread(ioBuffer, 1, sizeof(ioBuffer), pcm);
        if (bytesRead == 0) {
            break;
        }

        const size_t alignedLen = bytesRead & ~static_cast<size_t>(1);
        if (alignedLen > 0) {
            size_t written = 0;
            err = writePcm(ioBuffer, alignedLen, &written, 1000);
            if (err != ESP_OK) {
                break;
            }
            totalBytes += written;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    fclose(pcm);
    writeSilence(100);
    vTaskDelay(pdMS_TO_TICKS(120));
    setPaEnabled(false);

    if (err == ESP_OK && totalBytes == 0) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        LOG_I(TAG_AUDIO_IDF, "Played LittleFS cue %s (%u bytes): %s",
              normalizedCue,
              static_cast<unsigned>(totalBytes),
              resourcePath);
    } else {
        LOG_W(TAG_AUDIO_IDF, "LittleFS cue playback failed %s: %s", normalizedCue, esp_err_to_name(err));
    }
    return setLastError(err);
}

esp_err_t scanI2c(uint8_t* addresses, size_t maxAddresses, size_t* count) {
    if (count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    const esp_err_t err = initI2cBus();
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t addr = 1; addr < 0x7F; ++addr) {
        const esp_err_t probeErr = i2c_master_probe(g_i2cBus, addr, 30);
        if (probeErr == ESP_OK) {
            if (addresses != nullptr && *count < maxAddresses) {
                addresses[*count] = addr;
            }
            ++(*count);
        }
    }
    return ESP_OK;
}

}  // namespace AppIdfAudio
