#pragma once

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfPowerSave {

enum class State : uint8_t {
    L0_Active = 0,
    L1_Standby = 1,
};

esp_err_t start();
bool isStarted();

void notifyActivity();

esp_err_t enterL1();
esp_err_t exitL1();

State currentState();
bool isL1();
bool canEnterL1();

uint32_t lastActivityMs();
uint32_t inactivityMs();

void setEnabled(bool enabled);
bool isEnabled();

}  // namespace AppIdfPowerSave
