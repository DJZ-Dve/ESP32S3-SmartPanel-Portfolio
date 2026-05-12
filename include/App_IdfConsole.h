#pragma once

#include "esp_err.h"

namespace AppIdfConsole {

esp_err_t start();
void printHelp();
void handleCommand(const char* inputLine);

}  // namespace AppIdfConsole
