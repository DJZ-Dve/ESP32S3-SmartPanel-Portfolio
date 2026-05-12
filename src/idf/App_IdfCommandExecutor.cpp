#include "App_IdfCommandExecutor.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfBleAircon.h"
#include "App_IdfLearnFlow.h"
#include "App_IdfScene.h"
#include "App_IdfUi.h"
#include "App_Log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace AppIdfCommandExecutor {
namespace {

constexpr const char* TAG_EXEC = "IDF_EXEC";
constexpr int kAirconProtocolVersion = 1;
constexpr size_t kMaxAirconSteps = 8;
constexpr size_t kMaxSpeakerSteps = 2;
constexpr uint32_t kStepGapMs = 120;

void copyText(char* dst, size_t dstSize, const char* src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstSize, "%s", src);
}

void setResult(Result* result, bool handled, bool success, const char* summary, const char* error) {
    if (result == nullptr) {
        return;
    }

    result->handled = handled;
    result->success = success;
    copyText(result->summary, sizeof(result->summary), summary);
    copyText(result->error, sizeof(result->error), error);
}

void appendSummary(Result* result, const char* text) {
    if (result == nullptr || text == nullptr || text[0] == '\0') {
        return;
    }

    const size_t used = strlen(result->summary);
    if (used >= sizeof(result->summary) - 1) {
        return;
    }

    snprintf(result->summary + used,
             sizeof(result->summary) - used,
             "%s%s",
             used > 0 ? " | " : "",
             text);
}

void toLowerAscii(char* text) {
    if (text == nullptr) {
        return;
    }
    while (*text != '\0') {
        *text = static_cast<char>(tolower(static_cast<unsigned char>(*text)));
        ++text;
    }
}

bool getTextField(const cJSON* object, const char* key, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }
    out[0] = '\0';

    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (value == nullptr || cJSON_IsNull(value)) {
        return false;
    }

    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        copyText(out, outSize, value->valuestring);
        return out[0] != '\0';
    }
    if (cJSON_IsBool(value)) {
        copyText(out, outSize, cJSON_IsTrue(value) ? "true" : "false");
        return true;
    }
    if (cJSON_IsNumber(value)) {
        if (fabs(value->valuedouble - static_cast<double>(value->valueint)) < 0.0001) {
            snprintf(out, outSize, "%d", value->valueint);
        } else {
            snprintf(out, outSize, "%.2f", value->valuedouble);
        }
        return true;
    }

    return false;
}

bool getIntField(const cJSON* object, const char* key, int minValue, int maxValue, int* out) {
    if (out == nullptr) {
        return false;
    }

    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (value == nullptr || cJSON_IsNull(value)) {
        return false;
    }

    int parsed = 0;
    if (cJSON_IsNumber(value)) {
        if (fabs(value->valuedouble - static_cast<double>(value->valueint)) >= 0.0001) {
            return false;
        }
        parsed = value->valueint;
    } else if (cJSON_IsString(value) && value->valuestring != nullptr) {
        char* end = nullptr;
        const long parsedLong = strtol(value->valuestring, &end, 10);
        while (end != nullptr && *end != '\0' && isspace(static_cast<unsigned char>(*end))) {
            ++end;
        }
        if (end == value->valuestring || end == nullptr || *end != '\0') {
            return false;
        }
        parsed = static_cast<int>(parsedLong);
    } else {
        return false;
    }

    if (parsed < minValue || parsed > maxValue) {
        return false;
    }

    *out = parsed;
    return true;
}

bool controlHasCommand(const cJSON* control) {
    const cJSON* hasCommand = cJSON_GetObjectItemCaseSensitive(control, "has_command");
    if (hasCommand == nullptr) {
        return false;
    }
    if (cJSON_IsBool(hasCommand)) {
        return cJSON_IsTrue(hasCommand);
    }
    if (cJSON_IsNumber(hasCommand)) {
        return hasCommand->valueint != 0;
    }
    if (cJSON_IsString(hasCommand) && hasCommand->valuestring != nullptr) {
        char value[8] = {};
        copyText(value, sizeof(value), hasCommand->valuestring);
        toLowerAscii(value);
        return strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0;
    }
    return false;
}

esp_err_t applyBleResult(esp_err_t err, Result* result, const char* successSummary) {
    if (err == ESP_OK) {
        appendSummary(result, successSummary);
        return ESP_OK;
    }

    const char* lastError = AppIdfBleAircon::getLastError();
    copyText(result->error, sizeof(result->error), lastError != nullptr ? lastError : esp_err_to_name(err));
    return err;
}

esp_err_t executeMode(const char* mode, Result* result) {
    char token[16] = {};
    copyText(token, sizeof(token), mode);
    toLowerAscii(token);

    if (token[0] == '\0' || strcmp(token, "cool") == 0) {
        return applyBleResult(AppIdfBleAircon::setCoolingMode(), result, "aircon mode cool");
    }
    if (strcmp(token, "vent") == 0 || strcmp(token, "fan") == 0) {
        return applyBleResult(AppIdfBleAircon::setVentMode(), result, "aircon mode vent");
    }
    if (strcmp(token, "eco") == 0 || strcmp(token, "energy") == 0) {
        return applyBleResult(AppIdfBleAircon::setEcoMode(), result, "aircon mode eco");
    }
    if (strcmp(token, "sleep") == 0) {
        return applyBleResult(AppIdfBleAircon::setSleepMode(), result, "aircon mode sleep");
    }
    if (strcmp(token, "heat") == 0) {
        copyText(result->error, sizeof(result->error), "aircon heat mode is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    copyText(result->error, sizeof(result->error), "aircon mode is not supported");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t executeFan(const cJSON* step, Result* result) {
    int level = 0;
    if (!getIntField(step, "fan", 1, 5, &level)) {
        copyText(result->error, sizeof(result->error), "aircon fan is only 1..5");
        return ESP_ERR_INVALID_ARG;
    }

    char summary[32] = {};
    snprintf(summary, sizeof(summary), "aircon fan %d", level);
    return applyBleResult(AppIdfBleAircon::setFanSpeed(static_cast<uint8_t>(level)), result, summary);
}

esp_err_t executeOnOffField(const cJSON* step, const char* key, bool* out) {
    char token[16] = {};
    if (!getTextField(step, key, token, sizeof(token))) {
        return ESP_ERR_INVALID_ARG;
    }
    toLowerAscii(token);
    if (strcmp(token, "on") == 0 || strcmp(token, "true") == 0 || strcmp(token, "1") == 0) {
        *out = true;
        return ESP_OK;
    }
    if (strcmp(token, "off") == 0 || strcmp(token, "false") == 0 || strcmp(token, "0") == 0) {
        *out = false;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t executeAirconStep(const cJSON* step, Result* result) {
    if (!cJSON_IsObject(step)) {
        copyText(result->error, sizeof(result->error), "aircon step is not an object");
        return ESP_ERR_INVALID_ARG;
    }

    char stepType[32] = {};
    if (!getTextField(step, "type", stepType, sizeof(stepType))) {
        copyText(result->error, sizeof(result->error), "aircon step type is empty");
        return ESP_ERR_INVALID_ARG;
    }
    toLowerAscii(stepType);

    if (strcmp(stepType, "power_state") == 0) {
        bool on = false;
        if (executeOnOffField(step, "power", &on) != ESP_OK) {
            copyText(result->error, sizeof(result->error), "aircon power parameter is invalid");
            return ESP_ERR_INVALID_ARG;
        }
        return applyBleResult(on ? AppIdfBleAircon::powerOn() : AppIdfBleAircon::powerOff(),
                              result,
                              on ? "aircon power on" : "aircon power off");
    }

    if (strcmp(stepType, "temperature_state") == 0) {
        int tempC = 0;
        if (!getIntField(step, "temp", 18, 31, &tempC)) {
            copyText(result->error, sizeof(result->error), "aircon temperature is only 18..31");
            return ESP_ERR_INVALID_ARG;
        }

        char summary[32] = {};
        snprintf(summary, sizeof(summary), "aircon temp %d", tempC);
        return applyBleResult(AppIdfBleAircon::setTemperature(static_cast<uint8_t>(tempC)), result, summary);
    }

    if (strcmp(stepType, "mode_state") == 0) {
        char mode[16] = {};
        if (!getTextField(step, "mode", mode, sizeof(mode))) {
            copyText(result->error, sizeof(result->error), "aircon mode parameter is empty");
            return ESP_ERR_INVALID_ARG;
        }
        return executeMode(mode, result);
    }

    if (strcmp(stepType, "fan_state") == 0) {
        return executeFan(step, result);
    }

    if (strcmp(stepType, "main_state") == 0) {
        esp_err_t err = applyBleResult(AppIdfBleAircon::powerOn(), result, "aircon power on");
        if (err != ESP_OK) {
            return err;
        }

        char mode[16] = {};
        if (getTextField(step, "mode", mode, sizeof(mode))) {
            err = executeMode(mode, result);
            if (err != ESP_OK) {
                return err;
            }
        }

        const cJSON* temp = cJSON_GetObjectItemCaseSensitive(step, "temp");
        if (temp != nullptr && !cJSON_IsNull(temp)) {
            int tempC = 0;
            if (!getIntField(step, "temp", 18, 31, &tempC)) {
                copyText(result->error, sizeof(result->error), "aircon temperature is only 18..31");
                return ESP_ERR_INVALID_ARG;
            }
            char summary[32] = {};
            snprintf(summary, sizeof(summary), "aircon temp %d", tempC);
            err = applyBleResult(AppIdfBleAircon::setTemperature(static_cast<uint8_t>(tempC)), result, summary);
            if (err != ESP_OK) {
                return err;
            }
        }

        const cJSON* fan = cJSON_GetObjectItemCaseSensitive(step, "fan");
        if (fan != nullptr && !cJSON_IsNull(fan)) {
            err = executeFan(step, result);
        }
        return err;
    }

    if (strcmp(stepType, "vent_state") == 0) {
        esp_err_t err = applyBleResult(AppIdfBleAircon::powerOn(), result, "aircon power on");
        if (err != ESP_OK) {
            return err;
        }
        err = executeMode("vent", result);
        if (err != ESP_OK) {
            return err;
        }
        return executeFan(step, result);
    }

    if (strcmp(stepType, "swing_state") == 0) {
        char swing[20] = {};
        if (!getTextField(step, "swing", swing, sizeof(swing))) {
            copyText(result->error, sizeof(result->error), "aircon swing parameter is empty");
            return ESP_ERR_INVALID_ARG;
        }
        toLowerAscii(swing);
        if (strcmp(swing, "horizontal") == 0 || strcmp(swing, "left_right") == 0 ||
            strcmp(swing, "lr") == 0) {
            return applyBleResult(AppIdfBleAircon::setSwingHorizontal(), result, "aircon swing horizontal");
        }
        if (strcmp(swing, "vertical") == 0 || strcmp(swing, "up_down") == 0 || strcmp(swing, "ud") == 0) {
            return applyBleResult(AppIdfBleAircon::setSwingVertical(), result, "aircon swing vertical");
        }
        copyText(result->error, sizeof(result->error), "aircon swing is only horizontal or vertical");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(stepType, "display_state") == 0) {
        bool on = false;
        if (executeOnOffField(step, "display", &on) != ESP_OK) {
            copyText(result->error, sizeof(result->error), "aircon display parameter is invalid");
            return ESP_ERR_INVALID_ARG;
        }
        return applyBleResult(AppIdfBleAircon::setDisplayOn(on), result, on ? "aircon display on" : "aircon display off");
    }

    if (strcmp(stepType, "light_state") == 0) {
        bool on = false;
        if (executeOnOffField(step, "light", &on) != ESP_OK) {
            copyText(result->error, sizeof(result->error), "aircon light parameter is invalid");
            return ESP_ERR_INVALID_ARG;
        }
        return applyBleResult(AppIdfBleAircon::setLightOn(on), result, on ? "aircon light on" : "aircon light off");
    }

    if (strcmp(stepType, "display_toggle") == 0 || strcmp(stepType, "timer_state") == 0) {
        copyText(result->error, sizeof(result->error), "aircon step is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    copyText(result->error, sizeof(result->error), "aircon step type is unknown");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t executeAirconControl(const cJSON* control, Result* result) {
    // 防御：IR/433 模式下 NimBLE 协议栈未常驻，临时 init 会因内部 SRAM 碎片化触发 BLE_INIT
    // Malloc failed → emi.c assert → panic。必须在 dispatch 入口拒掉，避免走到 BLE worker。
    if (!AppIdfAppMode::isBle()) {
        copyText(result->error, sizeof(result->error),
                 "aircon_ble_v1 only supported in BLE mode");
        return ESP_ERR_INVALID_STATE;
    }

    int protocolVersion = 0;
    if (!getIntField(control, "protocol_version", 1, 1, &protocolVersion) ||
        protocolVersion != kAirconProtocolVersion) {
        copyText(result->error, sizeof(result->error), "aircon protocol version is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON* steps = cJSON_GetObjectItemCaseSensitive(control, "steps");
    if (!cJSON_IsArray(steps)) {
        copyText(result->error, sizeof(result->error), "aircon steps are empty");
        return ESP_ERR_INVALID_ARG;
    }

    const int stepCount = cJSON_GetArraySize(steps);
    if (stepCount <= 0) {
        copyText(result->error, sizeof(result->error), "aircon steps are empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (stepCount > static_cast<int>(kMaxAirconSteps)) {
        copyText(result->error, sizeof(result->error), "aircon steps are too many");
        return ESP_ERR_INVALID_SIZE;
    }

    for (int i = 0; i < stepCount; ++i) {
        const cJSON* step = cJSON_GetArrayItem(steps, i);
        const esp_err_t err = executeAirconStep(step, result);
        if (err != ESP_OK) {
            return err;
        }
        if (i + 1 < stepCount) {
            vTaskDelay(pdMS_TO_TICKS(kStepGapMs));
        }
    }

    if (result->summary[0] == '\0') {
        copyText(result->summary, sizeof(result->summary), "aircon command executed");
    }
    return ESP_OK;
}

esp_err_t executeSpeakerControl(const cJSON* control, Result* result) {
    int protocolVersion = 0;
    if (!getIntField(control, "protocol_version", 1, 1, &protocolVersion)) {
        copyText(result->error, sizeof(result->error), "speaker protocol version is not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON* steps = cJSON_GetObjectItemCaseSensitive(control, "steps");
    const int stepCount = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
    if (stepCount <= 0 || stepCount > static_cast<int>(kMaxSpeakerSteps)) {
        copyText(result->error, sizeof(result->error), "speaker steps are invalid");
        return ESP_ERR_INVALID_ARG;
    }

    int targetVolume = AppIdfAudio::getVolume();
    for (int i = 0; i < stepCount; ++i) {
        const cJSON* step = cJSON_GetArrayItem(steps, i);
        if (!cJSON_IsObject(step)) {
            copyText(result->error, sizeof(result->error), "speaker step is not an object");
            return ESP_ERR_INVALID_ARG;
        }

        char stepType[32] = {};
        if (!getTextField(step, "type", stepType, sizeof(stepType))) {
            copyText(result->error, sizeof(result->error), "speaker step type is empty");
            return ESP_ERR_INVALID_ARG;
        }
        toLowerAscii(stepType);

        if (strcmp(stepType, "volume_set") == 0) {
            if (!getIntField(step, "value", 0, 100, &targetVolume)) {
                copyText(result->error, sizeof(result->error), "speaker volume is only 0..100");
                return ESP_ERR_INVALID_ARG;
            }
        } else if (strcmp(stepType, "volume_delta") == 0) {
            int delta = 0;
            if (!getIntField(step, "delta", -100, 100, &delta) || delta == 0) {
                copyText(result->error, sizeof(result->error), "speaker volume delta is invalid");
                return ESP_ERR_INVALID_ARG;
            }
            targetVolume += delta;
            if (targetVolume < 0) {
                targetVolume = 0;
            }
            if (targetVolume > 100) {
                targetVolume = 100;
            }
        } else {
            copyText(result->error, sizeof(result->error), "speaker step type is unknown");
            return ESP_ERR_INVALID_ARG;
        }

        const esp_err_t startErr = AppIdfAudio::start();
        if (startErr != ESP_OK) {
            copyText(result->error, sizeof(result->error), esp_err_to_name(startErr));
            return startErr;
        }
        const esp_err_t volumeErr = AppIdfAudio::setVolume(static_cast<uint8_t>(targetVolume));
        if (volumeErr != ESP_OK) {
            copyText(result->error, sizeof(result->error), esp_err_to_name(volumeErr));
            return volumeErr;
        }
    }

    char summary[40] = {};
    snprintf(summary, sizeof(summary), "speaker volume %d", targetVolume);
    appendSummary(result, summary);
    return ESP_OK;
}

void playAudioCueIfRequested(const cJSON* root, Result* result) {
    // 协议处理（如 local_learn_v1）已经播过自己的 cue 时，避免被 server 默认 cue 覆盖。
    if (result != nullptr && result->cuePlayed) {
        return;
    }
    char cueName[24] = {};
    if (!getTextField(root, "audio_cue", cueName, sizeof(cueName))) {
        return;
    }
    toLowerAscii(cueName);
    if (cueName[0] == '\0' || strcmp(cueName, "none") == 0) {
        return;
    }

    const esp_err_t err = AppIdfAudio::playLocalCue(cueName, 8000);
    if (err == ESP_OK) {
        char summary[40] = {};
        snprintf(summary, sizeof(summary), "audio cue %s", cueName);
        appendSummary(result, summary);
        if (result != nullptr) {
            result->cuePlayed = true;
            if (!result->handled) {
                result->handled = true;
                result->success = true;
            }
        }
    } else {
        LOG_W(TAG_EXEC, "Audio cue skipped %s: %s", cueName, esp_err_to_name(err));
    }
}

// Play the failure cue when a device command was attempted but did not succeed.
// Overrides whatever audio_cue the server suggested, so TTS reflects real outcome.
void playFailureCue(Result* result) {
    const esp_err_t err = AppIdfAudio::playLocalCue("op_failed", 8000);
    if (err == ESP_OK) {
        appendSummary(result, "audio cue op_failed");
        if (result != nullptr) {
            result->cuePlayed = true;
        }
    } else {
        LOG_W(TAG_EXEC, "Failure cue skipped: %s", esp_err_to_name(err));
    }
}

const cJSON* findControlObject(const cJSON* root) {
    if (!cJSON_IsObject(root)) {
        return nullptr;
    }

    const cJSON* control = cJSON_GetObjectItemCaseSensitive(root, "control");
    if (cJSON_IsObject(control)) {
        return control;
    }

    const cJSON* command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsObject(command)) {
        return command;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "has_command") != nullptr) {
        return root;
    }

    return nullptr;
}

esp_err_t executeControlObject(const cJSON* control, Result* result) {
    if (!cJSON_IsObject(control)) {
        setResult(result, false, false, "", "control object is missing");
        return ESP_ERR_NOT_FOUND;
    }

    if (!controlHasCommand(control)) {
        setResult(result, false, true, "no device command", "");
        return ESP_OK;
    }

    setResult(result, true, false, "", "");

    // 切换 AIScreen 到 EXECUTING：协议派发(BLE/扬声器)整段(含 2 次重试)显示"执行中..."
    // 录音 uploading 期间会被 sticky flag 拦截 THINKING 覆盖；executor 返回后由
    // recorder 收尾翻 SUCCESS/ERROR，sticky flag 在那里清。
    AppIdfUi::showAiStatus(AppIdfUi::AiStatus::Executing, true, 0);

    char protocol[32] = {};
    if (!getTextField(control, "protocol", protocol, sizeof(protocol))) {
        copyText(result->error, sizeof(result->error), "control protocol is empty");
        return ESP_ERR_INVALID_ARG;
    }

    LOG_I(TAG_EXEC, "Executing protocol command: %s", protocol);
    if (strcmp(protocol, "aircon_ble_v1") == 0) {
        const esp_err_t err = executeAirconControl(control, result);
        result->success = err == ESP_OK;
        return err;
    }
    if (strcmp(protocol, "speaker_v1") == 0) {
        const esp_err_t err = executeSpeakerControl(control, result);
        result->success = err == ESP_OK;
        return err;
    }
    if (strcmp(protocol, "local_scene_v1") == 0) {
        const cJSON* steps = cJSON_GetObjectItemCaseSensitive(control, "steps");
        if (!cJSON_IsArray(steps)) {
            copyText(result->error, sizeof(result->error), "local_scene_v1: steps missing");
            return ESP_ERR_INVALID_ARG;
        }
        // 仅取首个 run_scene step；服务器约定每次只发一条本地场景。
        const int total = cJSON_GetArraySize(steps);
        for (int i = 0; i < total; i++) {
            const cJSON* step = cJSON_GetArrayItem(steps, i);
            char stepType[16] = {};
            if (!getTextField(step, "type", stepType, sizeof(stepType))) continue;
            if (strcmp(stepType, "run_scene") != 0) continue;
            const cJSON* idItem = cJSON_GetObjectItemCaseSensitive(step, "scene_id");
            if (!cJSON_IsNumber(idItem)) {
                copyText(result->error, sizeof(result->error), "local_scene_v1: scene_id missing");
                return ESP_ERR_INVALID_ARG;
            }
            const uint8_t sceneId = static_cast<uint8_t>(idItem->valueint);
            const esp_err_t err = AppIdfScene::executeById(sceneId);
            if (err == ESP_OK) {
                snprintf(result->summary, sizeof(result->summary), "scene[%u] ok",
                         static_cast<unsigned>(sceneId));
                result->success = true;
                return ESP_OK;
            }
            snprintf(result->error, sizeof(result->error), "scene[%u]: %s",
                     static_cast<unsigned>(sceneId), esp_err_to_name(err));
            return err;
        }
        copyText(result->error, sizeof(result->error), "local_scene_v1: no run_scene step");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(protocol, "local_label_v1") == 0) {
        char label[64] = {};
        if (!getTextField(control, "label", label, sizeof(label)) || label[0] == '\0') {
            copyText(result->error, sizeof(result->error), "local_label_v1: label missing");
            AppIdfLearnFlow::onUploadFailed();
            return ESP_ERR_INVALID_ARG;
        }
        const esp_err_t err = AppIdfLearnFlow::onLabelArrived(label);
        if (err == ESP_OK) {
            snprintf(result->summary, sizeof(result->summary), "label saved: %s", label);
            result->success = true;
            // LearnFlow 已在 onLabelArrived 内部播 done cue；标记 cuePlayed 防止再覆盖。
            result->cuePlayed = true;
            return ESP_OK;
        }
        snprintf(result->error, sizeof(result->error), "label save failed: %s", esp_err_to_name(err));
        result->cuePlayed = true;  // LearnFlow 内部已播 op_failed
        return err;
    }
    if (strcmp(protocol, "local_learn_v1") == 0) {
        // 语音「进入场景学习」入口。BLE 模式下 startCapture 直接返回 INVALID_STATE。
        const esp_err_t err = AppIdfLearnFlow::startCapture();
        if (err == ESP_OK) {
            AppIdfUi::showLearningScreen();
            copyText(result->summary, sizeof(result->summary), "learn started");
            result->success = true;
            // startCapture 内部已播 learn_press_key cue。
            result->cuePlayed = true;
            return ESP_OK;
        }
        snprintf(result->error, sizeof(result->error), "learn start failed: %s", esp_err_to_name(err));
        return err;
    }

    copyText(result->error, sizeof(result->error), "control protocol is not supported");
    return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace

esp_err_t executeControlJson(const char* json, Result* result) {
    LOG_I(TAG_EXEC, "[LAT] cmd_parse_start json_len=%u",
          static_cast<unsigned>(json != nullptr ? strlen(json) : 0));
    setResult(result, false, false, "", "");
    if (json == nullptr || json[0] == '\0') {
        copyText(result->error, sizeof(result->error), "json is empty");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        copyText(result->error, sizeof(result->error), "json parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON* control = findControlObject(root);
    const esp_err_t err = executeControlObject(control, result);
    if (cJSON_IsObject(root)) {
        // Override the server-suggested cue when a device command was attempted
        // but failed, so the spoken feedback matches the real outcome instead of
        // claiming "已经设置完毕" while BLE actually dropped the frame.
        const bool deviceCommandFailed =
            result != nullptr && result->handled && (err != ESP_OK || !result->success);
        if (deviceCommandFailed) {
            playFailureCue(result);
        } else {
            playAudioCueIfRequested(root, result);
        }
    }
    if (err == ESP_OK && result != nullptr && result->handled && result->success) {
        LOG_I(TAG_EXEC, "Protocol command succeeded: %s", result->summary);
    } else if (err != ESP_OK && result != nullptr) {
        LOG_W(TAG_EXEC, "Protocol command failed: %s", result->error);
    }

    cJSON_Delete(root);
    return err;
}

}  // namespace AppIdfCommandExecutor
