#ifndef APP_IDF_INPUT_H
#define APP_IDF_INPUT_H

#include <stdint.h>

#include "esp_err.h"

namespace AppIdfInput {

enum class KeyAction : uint8_t {
    None = 0,
    Down,
    ShortPress,
    LongPressStart,
    LongPressHold,
    LongPressEnd,
};

enum class KeyId : uint8_t {
    None = 0,
    Key1 = 1,
    Key2 = 2,
    Both = 3,
};

struct KeyEvent {
    KeyAction action = KeyAction::None;
    KeyId keyId = KeyId::None;
    int raw = 0;
    int millivolts = 0;
};

using KeyEventCallback = void (*)(const KeyEvent& event);

esp_err_t start(KeyEventCallback callback);
bool isStarted();
KeyId activeKeyId();
KeyEvent lastEvent();
int lastRaw();
int lastMillivolts();
uint32_t taskStackHighWatermark();
const char* keyActionName(KeyAction action);
const char* keyIdName(KeyId keyId);

}  // namespace AppIdfInput

#endif  // APP_IDF_INPUT_H
