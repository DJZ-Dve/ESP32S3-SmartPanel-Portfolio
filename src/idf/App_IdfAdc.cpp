#include "App_IdfAdc.h"

#include "App_Log.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace AppIdfAdc {
namespace {

constexpr const char* TAG_ADC = "IDF_ADC";
constexpr int kMaxAdcUnits = 2;
constexpr int kMaxAdcChannels = 10;
constexpr adc_atten_t kAdcAtten = ADC_ATTEN_DB_12;
constexpr adc_bitwidth_t kAdcBitwidth = ADC_BITWIDTH_12;

adc_oneshot_unit_handle_t g_unitHandles[kMaxAdcUnits] = {};
adc_cali_handle_t g_caliHandles[kMaxAdcUnits][kMaxAdcChannels] = {};
bool g_channelConfigured[kMaxAdcUnits][kMaxAdcChannels] = {};
bool g_caliAttempted[kMaxAdcUnits][kMaxAdcChannels] = {};
SemaphoreHandle_t g_adcMutex = nullptr;
bool g_initialized = false;

class AdcLock {
public:
    explicit AdcLock(TickType_t timeoutTicks) {
        if (g_adcMutex != nullptr) {
            _locked = xSemaphoreTake(g_adcMutex, timeoutTicks) == pdTRUE;
        }
    }

    ~AdcLock() {
        if (_locked && g_adcMutex != nullptr) {
            xSemaphoreGive(g_adcMutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    bool _locked = false;
};

int unitIndex(adc_unit_t unit) {
    const int index = static_cast<int>(unit);
    if (index < 0 || index >= kMaxAdcUnits) {
        return -1;
    }
    return index;
}

int channelIndex(adc_channel_t channel) {
    const int index = static_cast<int>(channel);
    if (index < 0 || index >= kMaxAdcChannels) {
        return -1;
    }
    return index;
}

int approximateRawToMillivolts(int raw) {
    if (raw <= 0) {
        return 0;
    }
    return (raw * 3300 + 2047) / 4095;
}

esp_err_t ensureUnit(adc_unit_t unit) {
    const int index = unitIndex(unit);
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_unitHandles[index] != nullptr) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unitConfig = {
        .unit_id = unit,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    const esp_err_t err = adc_oneshot_new_unit(&unitConfig, &g_unitHandles[index]);
    if (err != ESP_OK) {
        LOG_E(TAG_ADC, "ADC unit %d init failed: %s", index + 1, esp_err_to_name(err));
    }
    return err;
}

esp_err_t createCalibration(adc_unit_t unit, adc_channel_t channel, adc_cali_handle_t* handle) {
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        const adc_cali_curve_fitting_config_t caliConfig = {
            .unit_id = unit,
            .chan = channel,
            .atten = kAdcAtten,
            .bitwidth = kAdcBitwidth,
        };
        const esp_err_t err = adc_cali_create_scheme_curve_fitting(&caliConfig, handle);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        const adc_cali_line_fitting_config_t caliConfig = {
            .unit_id = unit,
            .atten = kAdcAtten,
            .bitwidth = kAdcBitwidth,
        };
        const esp_err_t err = adc_cali_create_scheme_line_fitting(&caliConfig, handle);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
#endif

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ensureChannel(adc_unit_t unit, adc_channel_t channel) {
    const int unitIdx = unitIndex(unit);
    const int chanIdx = channelIndex(channel);
    if (unitIdx < 0 || chanIdx < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensureUnit(unit);
    if (err != ESP_OK) {
        return err;
    }

    if (!g_channelConfigured[unitIdx][chanIdx]) {
        const adc_oneshot_chan_cfg_t channelConfig = {
            .atten = kAdcAtten,
            .bitwidth = kAdcBitwidth,
        };
        err = adc_oneshot_config_channel(g_unitHandles[unitIdx], channel, &channelConfig);
        if (err != ESP_OK) {
            LOG_E(TAG_ADC, "ADC channel config failed unit=%d channel=%d: %s", unitIdx + 1, chanIdx,
                  esp_err_to_name(err));
            return err;
        }
        g_channelConfigured[unitIdx][chanIdx] = true;
    }

    if (!g_caliAttempted[unitIdx][chanIdx]) {
        g_caliAttempted[unitIdx][chanIdx] = true;
        err = createCalibration(unit, channel, &g_caliHandles[unitIdx][chanIdx]);
        if (err == ESP_OK) {
            LOG_I(TAG_ADC, "ADC calibration enabled unit=%d channel=%d", unitIdx + 1, chanIdx);
        } else {
            LOG_W(TAG_ADC,
                  "ADC calibration unavailable unit=%d channel=%d, using raw approximation",
                  unitIdx + 1,
                  chanIdx);
        }
    }

    return ESP_OK;
}

}  // namespace

esp_err_t init() {
    if (g_initialized) {
        return ESP_OK;
    }
    if (g_adcMutex == nullptr) {
        g_adcMutex = xSemaphoreCreateMutex();
        if (g_adcMutex == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    g_initialized = true;
    return ESP_OK;
}

bool isInitialized() {
    return g_initialized;
}

esp_err_t readGpioMillivolts(int gpio, Sample* sample) {
    if (sample == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_initialized) {
        const esp_err_t initErr = init();
        if (initErr != ESP_OK) {
            return initErr;
        }
    }

    AdcLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }

    adc_unit_t unit = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &channel);
    if (err != ESP_OK) {
        LOG_E(TAG_ADC, "GPIO%d is not an ADC pad: %s", gpio, esp_err_to_name(err));
        return err;
    }

    err = ensureChannel(unit, channel);
    if (err != ESP_OK) {
        return err;
    }

    const int unitIdx = unitIndex(unit);
    const int chanIdx = channelIndex(channel);
    int raw = 0;
    err = adc_oneshot_read(g_unitHandles[unitIdx], channel, &raw);
    if (err != ESP_OK) {
        return err;
    }

    int millivolts = approximateRawToMillivolts(raw);
    if (g_caliHandles[unitIdx][chanIdx] != nullptr) {
        int calibratedMv = 0;
        err = adc_cali_raw_to_voltage(g_caliHandles[unitIdx][chanIdx], raw, &calibratedMv);
        if (err == ESP_OK) {
            millivolts = calibratedMv;
        }
    }

    sample->raw = raw;
    sample->millivolts = millivolts;
    return ESP_OK;
}

}  // namespace AppIdfAdc
