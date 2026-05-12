#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfCommandExecutor {

constexpr size_t kSummaryBufferSize = 160;
constexpr size_t kErrorBufferSize = 128;

struct Result {
    bool handled = false;
    bool success = false;
    bool cuePlayed = false;  // true once any local audio cue actually played
    char summary[kSummaryBufferSize] = {};
    char error[kErrorBufferSize] = {};
};

esp_err_t executeControlJson(const char* json, Result* result);

}  // namespace AppIdfCommandExecutor
