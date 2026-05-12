#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

namespace AppIdfVadNet {

class Detector {
public:
    esp_err_t begin(const char* partitionLabel = "model");
    void end();

    bool isReady() const {
        return _model != nullptr && _iface != nullptr;
    }

    bool isSpeech(int16_t* samples);
    size_t frameSamples() const {
        return _frameSamples;
    }
    int sampleRate() const {
        return _sampleRate;
    }
    int channelCount() const {
        return _channelCount;
    }
    const char* modelName() const {
        return _modelName;
    }

private:
    void* _model = nullptr;
    const void* _iface = nullptr;
    const char* _modelName = nullptr;
    size_t _frameSamples = 0;
    int _sampleRate = 0;
    int _channelCount = 0;
};

}  // namespace AppIdfVadNet
