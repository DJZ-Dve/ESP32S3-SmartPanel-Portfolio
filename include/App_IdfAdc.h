#ifndef APP_IDF_ADC_H
#define APP_IDF_ADC_H

#include "esp_err.h"

namespace AppIdfAdc {

struct Sample {
    int raw = 0;
    int millivolts = 0;
};

esp_err_t init();
bool isInitialized();
esp_err_t readGpioMillivolts(int gpio, Sample* sample);

}  // namespace AppIdfAdc

#endif  // APP_IDF_ADC_H
