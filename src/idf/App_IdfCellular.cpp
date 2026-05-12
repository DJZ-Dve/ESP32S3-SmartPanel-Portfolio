#include "App_IdfCellular.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "App_IdfMqtt.h"
#include "App_IdfTasks.h"
#include "App_IdfTime.h"
#include "App_Log.h"
#include "Pin_Config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"

namespace AppIdfCellular {
namespace {

constexpr const char* TAG_CELL_IDF = "IDF_4G";
constexpr uart_port_t kModemUart = UART_NUM_2;
constexpr size_t kAtResponseBufferLen = 512;
constexpr uint32_t kReaderTaskStackWords = 3072;
constexpr uint32_t kUartRxBufferSize = 4096;
constexpr uint32_t kUartTxBufferSize = 2048;
constexpr uint32_t kPowerOnDelayMs = 2000;
constexpr uint32_t kAtProbeTimeoutMs = 1200;
constexpr uint32_t kAttachTimeoutMs = 45000;
constexpr uint32_t kPppConnectTimeoutMs = 45000;

SemaphoreHandle_t g_mutex = nullptr;
Snapshot g_snapshot;
AppIdfTasks::StaticTaskMemory g_readerTaskMemory;
ppp_pcb* g_ppp = nullptr;
netif g_pppNetif = {};
bool g_pppInputActive = false;

class MutexLock {
public:
    explicit MutexLock(TickType_t timeoutTicks = portMAX_DELAY) {
        if (g_mutex != nullptr) {
            _locked = xSemaphoreTake(g_mutex, timeoutTicks) == pdTRUE;
        }
    }

    ~MutexLock() {
        if (_locked && g_mutex != nullptr) {
            xSemaphoreGive(g_mutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    bool _locked = false;
};

esp_err_t ensureMutex() {
    if (g_mutex != nullptr) {
        return ESP_OK;
    }
    g_mutex = xSemaphoreCreateMutex();
    return g_mutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

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

esp_err_t setLastError(esp_err_t err, const char* message = nullptr) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.lastEspError = err;
        if (message != nullptr && message[0] != '\0') {
            copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), message);
        } else if (err == ESP_OK) {
            copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), "none");
        } else {
            copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), esp_err_to_name(err));
        }
    }
    return err;
}

void sanitizeDigits(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }
    output[0] = '\0';
    size_t written = 0;
    for (size_t i = 0; input != nullptr && input[i] != '\0' && written + 1 < outputSize; ++i) {
        if (isdigit(static_cast<unsigned char>(input[i]))) {
            output[written++] = input[i];
        }
    }
    output[written] = '\0';
}

esp_err_t ensureUart() {
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked() && g_snapshot.uartReady) {
            return ESP_OK;
        }
    }

    uart_config_t uartConfig = {};
    uartConfig.baud_rate = BAUD_4G;
    uartConfig.data_bits = UART_DATA_8_BITS;
    uartConfig.parity = UART_PARITY_DISABLE;
    uartConfig.stop_bits = UART_STOP_BITS_1;
    uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartConfig.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(kModemUart, kUartRxBufferSize, kUartTxBufferSize, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return setLastError(err, "uart_driver_install");
    }
    err = uart_param_config(kModemUart, &uartConfig);
    if (err != ESP_OK) {
        return setLastError(err, "uart_param_config");
    }
    err = uart_set_pin(kModemUart, PIN_4G_TX, PIN_4G_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return setLastError(err, "uart_set_pin");
    }
    uart_flush_input(kModemUart);

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.uartReady = true;
    }
    return setLastError(ESP_OK);
}

bool responseContains(const char* response, const char* expected) {
    return expected == nullptr || expected[0] == '\0' || (response != nullptr && strstr(response, expected) != nullptr);
}

esp_err_t sendAtCommand(const char* command,
                        const char* expected,
                        const char* failure,
                        uint32_t timeoutMs,
                        char* response,
                        size_t responseSize) {
    if (command == nullptr || command[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensureUart();
    if (err != ESP_OK) {
        return err;
    }

    if (response != nullptr && responseSize > 0) {
        response[0] = '\0';
    }
    uart_flush_input(kModemUart);

    char line[160] = {};
    snprintf(line, sizeof(line), "%s\r\n", command);
    const int written = uart_write_bytes(kModemUart, line, strlen(line));
    if (written < 0) {
        return setLastError(ESP_FAIL, "uart_write");
    }

    char local[kAtResponseBufferLen] = {};
    size_t used = 0;
    const int64_t deadlineUs = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
    while (esp_timer_get_time() < deadlineUs && used + 1 < sizeof(local)) {
        uint8_t ch = 0;
        const int readLen = uart_read_bytes(kModemUart, &ch, 1, pdMS_TO_TICKS(100));
        if (readLen <= 0) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            continue;
        }
        if (ch == '\0') {
            continue;
        }
        local[used++] = static_cast<char>(ch);
        local[used] = '\0';
        if (responseContains(local, expected)) {
            if (response != nullptr && responseSize > 0) {
                copyText(response, responseSize, local);
            }
            return ESP_OK;
        }
        if (failure != nullptr && failure[0] != '\0' && strstr(local, failure) != nullptr) {
            if (response != nullptr && responseSize > 0) {
                copyText(response, responseSize, local);
            }
            return setLastError(ESP_FAIL, failure);
        }
    }

    if (response != nullptr && responseSize > 0) {
        copyText(response, responseSize, local);
    }
    return setLastError(ESP_ERR_TIMEOUT, command);
}

void cacheCsqFromResponse(const char* response) {
    const char* csq = response != nullptr ? strstr(response, "+CSQ:") : nullptr;
    if (csq == nullptr) {
        return;
    }
    int value = 0;
    if (sscanf(csq, "+CSQ: %d", &value) == 1) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.csq = (value >= 0 && value <= 31) ? value : 0;
        }
    }
}

void cacheImeiFromResponse(const char* response) {
    char imei[kMaxImeiLen] = {};
    sanitizeDigits(response, imei, sizeof(imei));
    if (strlen(imei) < 14) {
        return;
    }
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        copyText(g_snapshot.imei, sizeof(g_snapshot.imei), imei);
    }
    AppIdfMqtt::setDeviceIdentity(imei);
}

esp_err_t probeAt(uint32_t attempts) {
    char response[kAtResponseBufferLen] = {};
    for (uint32_t i = 0; i < attempts; ++i) {
        const esp_err_t err = sendAtCommand("AT", "OK", "ERROR", kAtProbeTimeoutMs, response, sizeof(response));
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
    return setLastError(ESP_ERR_TIMEOUT, "modem_no_at");
}

esp_err_t waitForAttach() {
    char response[kAtResponseBufferLen] = {};
    const int64_t deadlineUs = esp_timer_get_time() + static_cast<int64_t>(kAttachTimeoutMs) * 1000;
    while (esp_timer_get_time() < deadlineUs) {
        const esp_err_t err = sendAtCommand("AT+CGATT?", "OK", "ERROR", 2500, response, sizeof(response));
        if (err == ESP_OK && strstr(response, "+CGATT: 1") != nullptr) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
    return setLastError(ESP_ERR_TIMEOUT, "cgatt_timeout");
}

u32_t pppOutput(ppp_pcb*, const void* data, u32_t len, void*) {
    if (data == nullptr || len == 0) {
        return 0;
    }
    const int written = uart_write_bytes(kModemUart, data, len);
    if (written > 0) {
        MutexLock lock(pdMS_TO_TICKS(20));
        if (lock.locked()) {
            g_snapshot.txBytes += static_cast<uint32_t>(written);
        }
        return static_cast<u32_t>(written);
    }
    return 0;
}

void pppStatus(ppp_pcb* pcb, int errCode, void*) {
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }
    g_snapshot.pppError = errCode;
    if (errCode == PPPERR_NONE && pcb != nullptr) {
        const netif* pppif = ppp_netif(pcb);
        g_snapshot.connected = true;
        g_snapshot.pppRunning = true;
        if (pppif != nullptr) {
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), ipaddr_ntoa(&pppif->ip_addr));
        }
        copyText(g_snapshot.lastError, sizeof(g_snapshot.lastError), "none");
        LOG_I(TAG_CELL_IDF, "PPP connected: ip=%s", g_snapshot.ip);
        AppIdfTime::onNetworkUp();
        return;
    }

    g_snapshot.connected = false;
    copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
    if (errCode != PPPERR_USER) {
        snprintf(g_snapshot.lastError, sizeof(g_snapshot.lastError), "ppp_err_%d", errCode);
    }
    LOG_W(TAG_CELL_IDF, "PPP status err=%d", errCode);
}

#if PPP_NOTIFY_PHASE
void pppPhaseNotify(ppp_pcb*, u8_t phase, void*) {
    MutexLock lock(pdMS_TO_TICKS(20));
    if (lock.locked()) {
        g_snapshot.pppPhase = phase;
    }
}
#endif

void readerTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_4G_RX");
    uint8_t buffer[512] = {};

    while (true) {
        if (!g_pppInputActive || g_ppp == nullptr) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const int readLen = uart_read_bytes(kModemUart, buffer, sizeof(buffer), pdMS_TO_TICKS(100));
        if (readLen > 0) {
            pppos_input_tcpip(g_ppp, buffer, readLen);
            MutexLock lock(pdMS_TO_TICKS(20));
            if (lock.locked()) {
                g_snapshot.rxBytes += static_cast<uint32_t>(readLen);
            }
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

esp_err_t ensureReaderTask() {
    if (g_readerTaskMemory.handle != nullptr) {
        return ESP_OK;
    }
    return AppIdfTasks::createPinnedToCoreInternal(readerTask, "IDF_4G_RX", kReaderTaskStackWords, nullptr, 3, 0,
                                                   &g_readerTaskMemory);
}

esp_err_t createPppSession() {
    if (g_ppp != nullptr) {
        return ESP_OK;
    }
    memset(&g_pppNetif, 0, sizeof(g_pppNetif));
    g_ppp = pppos_create(&g_pppNetif, pppOutput, pppStatus, nullptr);
    if (g_ppp == nullptr) {
        return setLastError(ESP_ERR_NO_MEM, "pppos_create");
    }
    ppp_set_default(g_ppp);
#if LWIP_DNS
    ppp_set_usepeerdns(g_ppp, 1);
#endif
#if PPP_NOTIFY_PHASE
    ppp_set_notify_phase_callback(g_ppp, pppPhaseNotify);
#endif
    return ESP_OK;
}

esp_err_t beginPpp(uint32_t timeoutMs) {
    esp_err_t err = createPppSession();
    if (err != ESP_OK) {
        return err;
    }
    err = ensureReaderTask();
    if (err != ESP_OK) {
        return setLastError(err, "reader_task");
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.pppRunning = true;
            g_snapshot.connected = false;
            g_snapshot.pppError = 0;
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
        }
    }
    g_pppInputActive = true;

    const err_t pppErr = ppp_connect(g_ppp, 0);
    if (pppErr != ERR_OK) {
        return setLastError(ESP_FAIL, "ppp_connect");
    }

    const int64_t deadlineUs = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;
    while (esp_timer_get_time() < deadlineUs) {
        if (isConnected()) {
            return setLastError(ESP_OK);
        }
        Snapshot snap = snapshot();
        if (snap.pppError != 0 && snap.pppError != PPPERR_NONE) {
            return setLastError(ESP_FAIL, snap.lastError);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
    return setLastError(ESP_ERR_TIMEOUT, "ppp_timeout");
}

}  // namespace

esp_err_t start() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t powerConfig = {};
    powerConfig.pin_bit_mask = 1ULL << PIN_4G_PWR;
    powerConfig.mode = GPIO_MODE_INPUT_OUTPUT;
    powerConfig.pull_up_en = GPIO_PULLUP_DISABLE;
    powerConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
    powerConfig.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&powerConfig);
    if (err != ESP_OK) {
        return setLastError(err, "gpio_config");
    }
    // PIN_4G_PWR 高电平开启；start() 默认关电源。
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWR), 0);

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.started = true;
        g_snapshot.powered = false;
        copyText(g_snapshot.apn, sizeof(g_snapshot.apn), "cmnet");
    }
    return setLastError(ESP_OK);
}

namespace {

// 老 Arduino App4G 实测能 work 的 PWRKEY 复位时序：H 100ms → L 2500ms → H → 释放回 INPUT → 等 10s 让模组重启。
void runPwrKeyResetSequence() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << PIN_4G_PWRKEY;
    cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), 0);
    vTaskDelay(pdMS_TO_TICKS(2500));
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 释放为输入，让硬件下拉接管。
    cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&cfg);

    // 模组重启需要 ~10 秒；分段 feed 看门狗。
    for (int i = 0; i < 20; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

constexpr uint32_t kFallbackBaud = 115200;  // LE270-EU 出厂默认

esp_err_t setModemUartBaud(uint32_t baud) {
    esp_err_t err = uart_set_baudrate(kModemUart, baud);
    if (err != ESP_OK) {
        return setLastError(err, "uart_set_baudrate");
    }
    uart_flush_input(kModemUart);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t tryProbeAt(uint32_t attempts, uint32_t timeoutMs) {
    char response[kAtResponseBufferLen] = {};
    for (uint32_t i = 0; i < attempts; ++i) {
        if (sendAtCommand("AT", "OK", "ERROR", timeoutMs, response, sizeof(response)) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
    return ESP_ERR_TIMEOUT;
}

// LE270-EU 出厂默认 115200；通过 AT+IPR 升速后若没 AT&W 写盘，掉电会漂回出厂。
// 上电后做自适应：先试目标 BAUD_4G，失败回退 115200 探活并发 IPR 升速 + AT&W 存盘。
esp_err_t syncBaudRate() {
    if (tryProbeAt(3, 500) == ESP_OK) {
        LOG_I(TAG_CELL_IDF, "AT sync OK at %u baud", static_cast<unsigned>(BAUD_4G));
        return ESP_OK;
    }

    LOG_W(TAG_CELL_IDF, "AT no response at %u, trying %u fallback",
          static_cast<unsigned>(BAUD_4G), static_cast<unsigned>(kFallbackBaud));
    if (setModemUartBaud(kFallbackBaud) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tryProbeAt(3, 500) != ESP_OK) {
        LOG_W(TAG_CELL_IDF, "AT no response at fallback %u either",
              static_cast<unsigned>(kFallbackBaud));
        setModemUartBaud(BAUD_4G);  // 还原，让上层走 PWRKEY 复位
        return ESP_FAIL;
    }
    LOG_I(TAG_CELL_IDF, "Module reachable at %u, upgrading to %u",
          static_cast<unsigned>(kFallbackBaud), static_cast<unsigned>(BAUD_4G));

    char cmd[40] = {};
    snprintf(cmd, sizeof(cmd), "AT+IPR=%u", static_cast<unsigned>(BAUD_4G));
    char response[kAtResponseBufferLen] = {};
    if (sendAtCommand(cmd, "OK", "ERROR", 1500, response, sizeof(response)) != ESP_OK) {
        LOG_E(TAG_CELL_IDF, "AT+IPR upgrade failed");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    if (setModemUartBaud(BAUD_4G) != ESP_OK) {
        return ESP_FAIL;
    }
    if (tryProbeAt(5, 800) != ESP_OK) {
        LOG_E(TAG_CELL_IDF, "AT validation failed after upgrade to %u",
              static_cast<unsigned>(BAUD_4G));
        return ESP_FAIL;
    }
    LOG_I(TAG_CELL_IDF, "AT sync OK at %u after IPR upgrade", static_cast<unsigned>(BAUD_4G));

    if (sendAtCommand("AT&W", "OK", "ERROR", 1500, response, sizeof(response)) == ESP_OK) {
        LOG_I(TAG_CELL_IDF, "Module config saved (AT&W); IPR=%u will persist",
              static_cast<unsigned>(BAUD_4G));
    } else {
        LOG_W(TAG_CELL_IDF, "AT&W failed; IPR may not persist across power cycles");
    }
    return ESP_OK;
}

}  // namespace

esp_err_t powerOn() {
    esp_err_t err = start();
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWR), 1);
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.powered = true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(kPowerOnDelayMs));

    // 装上 UART，方便后续 probe / dial 直接用。
    ensureUart();

    // 模块 IPR 不持久会漂回出厂 115200，先做波特率自适应；失败再走 PWRKEY 复位重试一次。
    if (syncBaudRate() != ESP_OK) {
        LOG_W(TAG_CELL_IDF, "Baud sync failed after VBAT on, running PWRKEY reset sequence");
        runPwrKeyResetSequence();
        if (syncBaudRate() != ESP_OK) {
            LOG_E(TAG_CELL_IDF, "Baud sync still failing after PWRKEY reset");
        }
    }
    return setLastError(ESP_OK);
}

esp_err_t powerOff() {
    hangup();
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWR), 0);
    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.powered = false;
    }
    return setLastError(ESP_OK);
}

esp_err_t dial(const char* apn, uint32_t timeoutMs) {
    esp_err_t err = powerOn();
    if (err != ESP_OK) {
        return err;
    }
    err = ensureUart();
    if (err != ESP_OK) {
        return err;
    }

    const char* selectedApn = (apn != nullptr && apn[0] != '\0') ? apn : "cmnet";
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.dialing = true;
            g_snapshot.connected = false;
            g_snapshot.pppRunning = false;
            ++g_snapshot.dialAttempts;
            copyText(g_snapshot.apn, sizeof(g_snapshot.apn), selectedApn);
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
        }
    }

    char response[kAtResponseBufferLen] = {};
    err = probeAt(30);
    if (err == ESP_OK) {
        sendAtCommand("ATE0", "OK", "ERROR", 1500, response, sizeof(response));
        sendAtCommand("AT+CSQ", "OK", "ERROR", 2000, response, sizeof(response));
        cacheCsqFromResponse(response);
        sendAtCommand("AT+CGSN", "OK", "ERROR", 2000, response, sizeof(response));
        cacheImeiFromResponse(response);
        err = waitForAttach();
    }
    if (err == ESP_OK) {
        char cgdcont[96] = {};
        snprintf(cgdcont, sizeof(cgdcont), "AT+CGDCONT=1,\"IP\",\"%s\"", selectedApn);
        err = sendAtCommand(cgdcont, "OK", "ERROR", 5000, response, sizeof(response));
    }
    if (err == ESP_OK) {
        err = sendAtCommand("ATD*99***1#", "CONNECT", "NO CARRIER", 45000, response, sizeof(response));
        if (err != ESP_OK) {
            err = sendAtCommand("ATD*99#", "CONNECT", "NO CARRIER", 45000, response, sizeof(response));
        }
    }
    if (err == ESP_OK) {
        err = beginPpp(timeoutMs > 0 ? timeoutMs : kPppConnectTimeoutMs);
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.dialing = false;
        if (err != ESP_OK) {
            g_snapshot.pppRunning = false;
            g_snapshot.connected = false;
        }
    }
    return err == ESP_OK ? setLastError(ESP_OK) : err;
}

esp_err_t hangup() {
    g_pppInputActive = false;
    if (g_ppp != nullptr) {
        ppp_close(g_ppp, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        ppp_free(g_ppp);
        g_ppp = nullptr;
    }

    if (g_snapshot.uartReady) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uart_write_bytes(kModemUart, "+++", 3);
        vTaskDelay(pdMS_TO_TICKS(1200));
        char response[kAtResponseBufferLen] = {};
        sendAtCommand("ATH", "OK", "ERROR", 3000, response, sizeof(response));
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.dialing = false;
        g_snapshot.pppRunning = false;
        g_snapshot.connected = false;
        g_snapshot.pppPhase = 0;
        g_snapshot.pppError = PPPERR_USER;
        copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
    }
    return setLastError(ESP_OK);
}

esp_err_t setDefaultRoute() {
    if (g_ppp == nullptr || !isConnected()) {
        return setLastError(ESP_ERR_INVALID_STATE, "ppp_not_connected");
    }
    ppp_set_default(g_ppp);
    return setLastError(ESP_OK);
}

esp_err_t refreshSignal() {
    if (!isPowered() || !g_snapshot.uartReady || g_snapshot.pppRunning) {
        return setLastError(ESP_ERR_INVALID_STATE, "at_not_available");
    }
    char response[kAtResponseBufferLen] = {};
    const esp_err_t err = sendAtCommand("AT+CSQ", "OK", "ERROR", 2000, response, sizeof(response));
    if (err == ESP_OK) {
        cacheCsqFromResponse(response);
    }
    return err;
}

bool isStarted() {
    return g_snapshot.started;
}

bool isPowered() {
    return g_snapshot.powered;
}

bool isConnected() {
    return g_snapshot.connected;
}

bool isReadyForMqtt() {
    return g_snapshot.connected && g_snapshot.pppRunning;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return g_snapshot;
}

uint32_t taskStackHighWatermark() {
    return g_readerTaskMemory.handle != nullptr ? uxTaskGetStackHighWaterMark(g_readerTaskMemory.handle) : 0;
}

esp_err_t sendRawAt(const char* command, char* response, size_t responseSize, uint32_t timeoutMs) {
    if (g_pppInputActive || g_snapshot.pppRunning || g_snapshot.dialing) {
        return setLastError(ESP_ERR_INVALID_STATE, "ppp_busy");
    }
    if (!g_snapshot.uartReady) {
        const esp_err_t err = ensureUart();
        if (err != ESP_OK) {
            return err;
        }
    }
    return sendAtCommand(command, "OK", "ERROR", timeoutMs > 0 ? timeoutMs : 2000, response, responseSize);
}

esp_err_t dumpUartRx(char* buffer, size_t bufferSize, uint32_t durationMs) {
    if (g_pppInputActive || g_snapshot.pppRunning) {
        return setLastError(ESP_ERR_INVALID_STATE, "ppp_busy");
    }
    if (!g_snapshot.uartReady) {
        const esp_err_t err = ensureUart();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (buffer != nullptr && bufferSize > 0) {
        buffer[0] = '\0';
    }
    if (bufferSize == 0 || buffer == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t used = 0;
    const int64_t deadlineUs = esp_timer_get_time() + static_cast<int64_t>(durationMs) * 1000;
    while (esp_timer_get_time() < deadlineUs && used + 1 < bufferSize) {
        uint8_t ch = 0;
        const int readLen = uart_read_bytes(kModemUart, &ch, 1, pdMS_TO_TICKS(50));
        if (readLen <= 0) {
            AppIdfTasks::feedCurrentTaskWatchdog();
            continue;
        }
        if (ch == '\0') {
            continue;
        }
        buffer[used++] = static_cast<char>(ch);
        buffer[used] = '\0';
    }
    return ESP_OK;
}

esp_err_t pulsePowerKey(int level, uint32_t durationMs) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << PIN_4G_PWRKEY;
    cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return setLastError(err, "pwrkey_cfg");
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), level ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(durationMs > 0 ? durationMs : 600));
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), level ? 0 : 1);
    // Release back to input so the hardware pull-down can drive the line as before.
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&cfg);
    return ESP_OK;
}

esp_err_t setPowerKeyLevel(int level) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << PIN_4G_PWRKEY;
    cfg.mode = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return setLastError(err, "pwrkey_cfg");
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY), level ? 1 : 0);
    return ESP_OK;
}

int readPowerLevel() {
    return gpio_get_level(static_cast<gpio_num_t>(PIN_4G_PWR));
}

int readPowerKeyLevel() {
    return gpio_get_level(static_cast<gpio_num_t>(PIN_4G_PWRKEY));
}

}  // namespace AppIdfCellular
