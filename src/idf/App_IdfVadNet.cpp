#include "App_IdfVadNet.h"

#include <string.h>

#include "App_Log.h"
#include "esp_vadn_models.h"
#include "model_path.h"

namespace AppIdfVadNet {
namespace {

constexpr const char* TAG_VAD_IDF = "IDF_VADNET";

char* findVadNetModel(srmodel_list_t* models) {
    if (models == nullptr) {
        return nullptr;
    }

    char* modelName = esp_srmodel_filter(models, ESP_VADN_PREFIX, nullptr);
    if (modelName != nullptr) {
        return modelName;
    }

    for (int i = 0; i < models->num; ++i) {
        if (models->model_name[i] != nullptr && strstr(models->model_name[i], "vadnet") != nullptr) {
            return models->model_name[i];
        }
    }
    return nullptr;
}

}  // namespace

esp_err_t Detector::begin(const char* partitionLabel) {
    end();

    srmodel_list_t* models = get_static_srmodels();
    if (models == nullptr) {
        models = esp_srmodel_init(partitionLabel);
    }
    if (models == nullptr) {
        LOG_E(TAG_VAD_IDF,
              "VADNet model list load failed from partition=%s",
              partitionLabel != nullptr ? partitionLabel : "NULL");
        return ESP_ERR_NOT_FOUND;
    }

    char* vadName = findVadNetModel(models);
    if (vadName == nullptr) {
        LOG_E(TAG_VAD_IDF, "VADNet model not found in model partition");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_vadn_iface_t* iface = esp_vadn_handle_from_name(vadName);
    if (iface == nullptr) {
        LOG_E(TAG_VAD_IDF, "VADNet interface not found for model=%s", vadName);
        return ESP_ERR_NOT_FOUND;
    }

    model_iface_data_t* model = iface->create(vadName, VAD_MODE_1, 1, 128, 128);
    if (model == nullptr) {
        LOG_E(TAG_VAD_IDF, "VADNet create failed for model=%s", vadName);
        return ESP_ERR_NO_MEM;
    }

    const int frameSamples = iface->get_samp_chunksize(model);
    const int sampleRate = iface->get_samp_rate(model);
    const int channelCount = iface->get_channel_num(model);
    if (frameSamples <= 0 || sampleRate != 16000 || channelCount != 1) {
        LOG_E(TAG_VAD_IDF,
              "VADNet unsupported format: frame=%d samples, sample_rate=%d, channels=%d",
              frameSamples,
              sampleRate,
              channelCount);
        iface->destroy(model);
        return ESP_ERR_NOT_SUPPORTED;
    }

    _iface = iface;
    _model = model;
    _modelName = vadName;
    _frameSamples = static_cast<size_t>(frameSamples);
    _sampleRate = sampleRate;
    _channelCount = channelCount;

    LOG_I(TAG_VAD_IDF,
          "VADNet initialized: model=%s, frame=%u samples, sample_rate=%d, channels=%d",
          _modelName,
          static_cast<unsigned>(_frameSamples),
          _sampleRate,
          _channelCount);
    return ESP_OK;
}

void Detector::end() {
    if (_iface != nullptr && _model != nullptr) {
        const esp_vadn_iface_t* iface = static_cast<const esp_vadn_iface_t*>(_iface);
        model_iface_data_t* model = static_cast<model_iface_data_t*>(_model);
        iface->destroy(model);
    }

    _model = nullptr;
    _iface = nullptr;
    _modelName = nullptr;
    _frameSamples = 0;
    _sampleRate = 0;
    _channelCount = 0;
}

bool Detector::isSpeech(int16_t* samples) {
    if (_iface == nullptr || _model == nullptr || samples == nullptr) {
        return false;
    }

    const esp_vadn_iface_t* iface = static_cast<const esp_vadn_iface_t*>(_iface);
    model_iface_data_t* model = static_cast<model_iface_data_t*>(_model);
    return iface->detect(model, samples) == VAD_SPEECH;
}

}  // namespace AppIdfVadNet
