#ifndef APP_LOG_H
#define APP_LOG_H

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

class AppLogSerialPort {
public:
    void println() { puts(""); }

    void println(const char* text) { puts(text ? text : ""); }

    int printf(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        const int written = vprintf(fmt, args);
        va_end(args);
        return written;
    }
};

static AppLogSerialPort Serial;
#endif

// ============================================================
// Unified serial logging helpers
// ============================================================

// Log level definitions
#define LOG_LEVEL_OFF     -1  // fully silent
#define LOG_LEVEL_ERROR    0
#define LOG_LEVEL_WARN     1
#define LOG_LEVEL_INFO     2
#define LOG_LEVEL_DEBUG    3

// Runtime log level (change with the serial LOG=x command)
extern int g_LogLevel;

// ============================================================
// Runtime-controlled log macros
// ============================================================

#define LOG_E(tag, fmt, ...) \
    do { if (g_LogLevel >= LOG_LEVEL_ERROR) Serial.printf("[%s][ERROR] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define LOG_W(tag, fmt, ...) \
    do { if (g_LogLevel >= LOG_LEVEL_WARN) Serial.printf("[%s][WARN] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define LOG_I(tag, fmt, ...) \
    do { if (g_LogLevel >= LOG_LEVEL_INFO) Serial.printf("[%s][INFO] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define LOG_D(tag, fmt, ...) \
    do { if (g_LogLevel >= LOG_LEVEL_DEBUG) Serial.printf("[%s][DEBUG] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

// ============================================================
// ASCII-only formatting helpers
// ============================================================

#define LOG_SEPARATOR() Serial.println("------------------------------------------------")

// Startup banner
#define LOG_BANNER(title) do { \
    Serial.println(); \
    Serial.println("================================================"); \
    Serial.printf("%s\n", title); \
    Serial.println("================================================"); \
} while(0)

// Simple info block
#define LOG_BOX_START(title) do { \
    Serial.println(); \
    Serial.println("================================================"); \
    Serial.printf("%s\n", title); \
    Serial.println("------------------------------------------------"); \
} while(0)

#define LOG_BOX_LINE(fmt, ...) \
    Serial.printf(fmt "\n", ##__VA_ARGS__)

#define LOG_BOX_END() \
    Serial.println("================================================")

// ============================================================
// Module tags
// ============================================================

#define TAG_SYS    "SYS"
#define TAG_UI     "UI"
#define TAG_NET    "NET"
#define TAG_4G     "4G"
#define TAG_WIFI   "WIFI"
#define TAG_IR     "IR"
#define TAG_AUDIO  "AUDIO"
#define TAG_RF433  "RF433"
#define TAG_SERVER "SERVER"
#define TAG_AI     "WAKE"

// ============================================================
// Serial command help
// ============================================================

inline void printSerialHelp() {
    Serial.println();
    Serial.println("=== Serial Command Help ===");
    Serial.println("STATUS or ?  - Show full system status");
    Serial.println("TEMP         - Show current temperature");
    Serial.println("CSQ or 4G    - Show 4G signal strength");
    Serial.println("HEAP         - Show free heap");
    Serial.println("WIFI         - Show WiFi status");
    Serial.println("WIFIDIAG     - Show WiFi/AP diagnostics");
    Serial.println("NET=4G       - Force 4G PPP-only network mode");
    Serial.println("NET=AUTO     - Switch to auto mode (WiFi preferred)");
    Serial.println("AT+xxx       - Unavailable in PPP-only firmware path");
    Serial.println("DEBUG4G      - Unavailable in PPP-only firmware path");
    Serial.println("LOG=0~3      - Set log level (0 error / 1 warn / 2 info / 3 debug)");
    Serial.println("LOG=OFF      - Disable all logs");
    Serial.println("AUDIOTEST    - Run amplifier idle-noise test");
    Serial.println("SINETEST     - Run sine playback test");
    Serial.println("AUDIORESET   - Recover I2S + ES8311 DAC playback path");
    Serial.println("AIRECSTART   - Debug: simulate KEY2 long-press recording start");
    Serial.println("AIRECSTOP    - Debug: simulate KEY2 release, play RecordStop, queue upload");
    Serial.println("SLEEP        - Enter deep sleep (wake with key press)");
    Serial.println("BAT          - Show current battery voltage");
    Serial.println("BATMON       - Monitor battery voltage for 10 seconds");
    Serial.println("BLEACON      - Turn the BLE air conditioner on");
    Serial.println("BLEACOFF     - Turn the BLE air conditioner off");
    Serial.println("BLECTEMP 26  - Set BLE air conditioner temperature (18~31)");
    Serial.println("BLEMODE COOL - Set mode: COOL / VENT / ECO / SLEEP");
    Serial.println("BLEFAN 3     - Set BLE air conditioner fan speed (1~5)");
    Serial.println("BLEDISP ON   - Display ON or OFF");
    Serial.println("BLELIGHT ON  - Light ON or OFF");
    Serial.println("BLESWING H   - Swing H(horizontal) or V(vertical)");
    Serial.println("BLEKEEP=1    - Keep BLE connected after commands (default)");
    Serial.println("BLEKEEP=0    - Auto-disconnect after commands and release connection");
    Serial.println("BLEDROP      - Manually release the BLE command connection");
    Serial.println("HELP         - Show this help");
    Serial.println("===========================");
}

// Print a compact system status report
inline void printSystemStatus(
    float temp,
    int csq,
    bool wifiConnected,
    const char* wifiIP,
    bool pppActive,
    bool pppConnected,
    const char* pppIP,
    uint32_t freeHeap,
    const char* activeTransport,
    bool mqttConnected,
    const char* mqttTransport
) {
    int percent = (csq == 99 || csq == 0) ? 0 : (csq * 100) / 31;
    
    Serial.println();
    Serial.println("=== System Status ===");
    Serial.printf("Temperature : %.1f C\n", temp);
    Serial.printf("4G Signal   : %d%% (CSQ=%d)\n", percent, csq);
    if (wifiConnected) {
        Serial.printf("WiFi        : connected, IP=%s\n", wifiIP);
    } else {
        Serial.println("WiFi        : disconnected");
    }
    if (pppActive) {
        Serial.printf("PPP         : %s, IP=%s\n",
            pppConnected ? "connected" : "dialing",
            (pppConnected && pppIP && pppIP[0] != '\0') ? pppIP : "-");
    } else {
        Serial.println("PPP         : disabled");
    }
    Serial.printf("Active Net  : %s\n", activeTransport ? activeTransport : "NONE");
    Serial.printf("MQTT        : %s%s%s\n",
        mqttConnected ? "connected" : "disconnected",
        mqttConnected ? ", " : "",
        mqttConnected && mqttTransport ? mqttTransport : "");
    Serial.printf("Free Heap   : %lu bytes\n", (unsigned long)freeHeap);
    Serial.println("=====================");
}

#endif // APP_LOG_H
