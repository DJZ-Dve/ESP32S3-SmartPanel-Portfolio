#ifndef APP_IDF_LVGL_H
#define APP_IDF_LVGL_H

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfLvgl {

using LockedCallback = void (*)(void* userData);

esp_err_t start();
bool isStarted();
uint32_t taskStackHighWatermark();
esp_err_t requestRefresh();
esp_err_t runLocked(LockedCallback callback, void* userData, uint32_t timeoutMs);
void enableBacklightAfterNextFrame();

}  // namespace AppIdfLvgl

#endif  // APP_IDF_LVGL_H
