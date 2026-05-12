#ifndef APP_FLASH_GUARD_H
#define APP_FLASH_GUARD_H

#include <stdint.h>

class AppFlashGuard {
public:
    static void init();
    static bool begin(const char* reason, uint32_t playbackWaitMs = 5000);
    static void end();
    static bool isActive();
    static bool isRestoringWakeWord();
};

class ScopedFlashGuard {
public:
    ScopedFlashGuard(const char* reason, uint32_t playbackWaitMs = 5000)
        : _active(AppFlashGuard::begin(reason, playbackWaitMs)) {}

    ~ScopedFlashGuard() {
        if (_active) {
            AppFlashGuard::end();
        }
    }

    bool ok() const { return _active; }

private:
    bool _active = false;
};

#endif
