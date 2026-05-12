#include "App_IdfNetwork.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "App_IdfTime.h"
#include "App_Log.h"
#include "App_IdfTasks.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace AppIdfNetwork {
namespace {

constexpr const char* TAG_NET_IDF = "IDF_NET";
constexpr const char* kWifiPrefsNamespace = "wifi_cfg";
constexpr const char* kWifiSsidKey = "ssid";
constexpr const char* kWifiPasswordKey = "password";
constexpr uint32_t kConnectWaitMs = 50;
constexpr uint32_t kScanActiveMinMs = 80;
constexpr uint32_t kScanActiveMaxMs = 260;
constexpr uint8_t kPortalChannel = 1;
constexpr uint8_t kPortalMaxConnections = 4;
constexpr uint32_t kPortalApReadyDelayMs = 150;
constexpr uint32_t kPortalMinInternalFree = 20 * 1024;
constexpr uint32_t kPortalMinInternalLargest = 12 * 1024;
constexpr uint32_t kPortalHttpStackBytes = 8192;
constexpr uint16_t kPortalDnsPort = 53;
constexpr size_t kPortalScanJsonSize = 1536;
constexpr uint32_t kDnsTaskStackWords = 4096;
constexpr uint32_t kDnsPollDelayMs = 250;
constexpr uint32_t kDnsRetryDelayMs = 1000;
constexpr uint32_t kDnsTtlSeconds = 30;

constexpr uint8_t kDnsPrimary[4] = {223, 5, 5, 5};
constexpr uint8_t kDnsSecondary[4] = {114, 114, 114, 114};

constexpr const char* kPortalHtml = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>HC-A1000 配网</title>
  <style>
    body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei","Segoe UI",sans-serif;background:#f5f7fb;color:#172033}
    .wrap{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:22px}
    .card{width:100%;max-width:390px;background:#fff;border:1px solid #d9e1ee;border-radius:14px;padding:22px;box-shadow:0 14px 34px rgba(24,39,75,.12)}
    h1{font-size:22px;margin:0 0 6px}p{margin:0 0 18px;color:#5b687a;font-size:14px}
    label{display:block;font-size:13px;font-weight:600;margin:14px 0 7px}
    select,input,button{width:100%;box-sizing:border-box;border-radius:10px;font-size:15px}
    select,input{border:1px solid #c9d3e2;padding:11px;background:#fff;color:#172033}
    button{border:0;padding:12px;background:#0b8f8a;color:white;font-weight:700;margin-top:16px}
    .row{display:flex;gap:10px}.row button{width:auto;min-width:96px;margin-top:0;background:#34445a}
    .hint{margin-top:14px;min-height:20px;color:#0b695f;font-size:14px}.foot{margin-top:12px;color:#7a8798;font-size:12px}
  </style>
</head>
<body>
  <div class="wrap"><div class="card">
    <h1>HC-A1000 配网</h1><p>选择 WiFi 网络,输入密码后点击保存。</p>
    <label>附近的 WiFi</label><div class="row"><select id="ssid"></select><button id="refresh" type="button">扫描</button></div>
    <label>密码</label><input id="password" type="password" placeholder="开放网络可留空">
    <button id="save" type="button">保存并连接</button>
    <div class="hint" id="hint">正在扫描...</div><div class="foot" id="foot"></div>
  </div></div>
  <script>
    const HINTS={portal_starting:'热点启动中',portal_ready:'热点已就绪',client_connected:'手机已连接,请输入 WiFi',connecting:'正在连接 WiFi',saved_connecting:'已保存,正在连接',low_memory:'内存不足',portal_http_error:'服务异常',invalid_ssid:'WiFi 名称无效'};
    function trHint(t){return t?(HINTS[t]||t):'';}
    const ssidEl=document.getElementById('ssid'),passwordEl=document.getElementById('password'),hintEl=document.getElementById('hint'),footEl=document.getElementById('foot');
    let pollTimer=null;function hint(t){hintEl.textContent=t;}
    async function scan(){hint('正在扫描...');ssidEl.innerHTML='';try{const r=await fetch('/scan',{cache:'no-store'});const d=await r.json();const n=d.networks||[];if(!n.length){ssidEl.add(new Option('未发现网络',''));hint('未发现可用网络');return;}n.forEach(i=>ssidEl.add(new Option(`${i.ssid} (${i.rssi}dBm${i.secure?' / 加密':' / 开放'})`,i.ssid)));hint('请选择网络。');}catch(e){hint('扫描失败。');}}
    async function poll(){try{const r=await fetch('/status',{cache:'no-store'});const d=await r.json();if(d.connected){hint(`已连接:${d.ip||'--'}`);clearInterval(pollTimer);pollTimer=null;}else if(d.hint){hint(trHint(d.hint));}if(d.ap_ssid){footEl.textContent=`热点:${d.ap_ssid}`;}}catch(e){}}
    document.getElementById('refresh').onclick=scan;
    document.getElementById('save').onclick=async()=>{const ssid=ssidEl.value.trim();if(!ssid){hint('请选择 WiFi。');return;}hint('正在保存并连接...');try{const body=new URLSearchParams({ssid,password:passwordEl.value});const r=await fetch('/save',{method:'POST',body});const d=await r.json();hint(trHint(d.message)||'已提交。');if(!pollTimer)pollTimer=setInterval(poll,1200);}catch(e){hint('保存失败。');}};
    window.addEventListener('load',()=>{poll();scan();});
  </script>
</body>
</html>
)HTML";

esp_netif_t* g_staNetif = nullptr;
esp_netif_t* g_apNetif = nullptr;
httpd_handle_t g_httpServer = nullptr;
esp_event_handler_instance_t g_wifiEventHandler = nullptr;
esp_event_handler_instance_t g_ipEventHandler = nullptr;
SemaphoreHandle_t g_networkMutex = nullptr;
AppIdfTasks::StaticTaskMemory g_dnsTaskMemory;
Snapshot g_snapshot;
Credentials g_credentials;

void ensurePortalSsidLocked();

class MutexLock {
public:
    explicit MutexLock(TickType_t timeoutTicks = portMAX_DELAY) {
        if (g_networkMutex != nullptr) {
            _locked = xSemaphoreTake(g_networkMutex, timeoutTicks) == pdTRUE;
        }
    }

    ~MutexLock() {
        if (_locked && g_networkMutex != nullptr) {
            xSemaphoreGive(g_networkMutex);
        }
    }

    bool locked() const {
        return _locked;
    }

private:
    bool _locked = false;
};

esp_err_t setLastError(esp_err_t err) {
    g_snapshot.lastError = err;
    return err;
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

void trimInPlace(char* text) {
    if (text == nullptr) {
        return;
    }

    char* start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }

    char* end = start + strlen(start);
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\r' || *(end - 1) == '\n')) {
        --end;
    }
    *end = '\0';

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

esp_err_t ensureMutex() {
    if (g_networkMutex != nullptr) {
        return ESP_OK;
    }
    g_networkMutex = xSemaphoreCreateMutex();
    return g_networkMutex != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

void applyDnsConfig() {
    if (g_staNetif == nullptr) {
        return;
    }

    esp_netif_dns_info_t dns = {};
    IP4_ADDR(&dns.ip.u_addr.ip4, kDnsPrimary[0], kDnsPrimary[1], kDnsPrimary[2], kDnsPrimary[3]);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(g_staNetif, ESP_NETIF_DNS_MAIN, &dns);

    memset(&dns, 0, sizeof(dns));
    IP4_ADDR(&dns.ip.u_addr.ip4, kDnsSecondary[0], kDnsSecondary[1], kDnsSecondary[2], kDnsSecondary[3]);
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(g_staNetif, ESP_NETIF_DNS_BACKUP, &dns);
}

bool hasPortalMemoryHeadroom() {
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internalFree >= kPortalMinInternalFree && internalLargest >= kPortalMinInternalLargest) {
        return true;
    }

    LOG_W(TAG_NET_IDF,
          "Internal SRAM is low; skip WiFi portal start (free=%u largest=%u)",
          static_cast<unsigned>(internalFree),
          static_cast<unsigned>(internalLargest));
    return false;
}

esp_err_t ensureApNetif() {
    if (g_apNetif == nullptr) {
        g_apNetif = esp_netif_create_default_wifi_ap();
        if (g_apNetif == nullptr) {
            return ESP_FAIL;
        }
    }

    esp_err_t err = esp_netif_dhcps_stop(g_apNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    esp_netif_ip_info_t ipInfo = {};
    IP4_ADDR(&ipInfo.ip, 192, 168, 4, 1);
    IP4_ADDR(&ipInfo.gw, 192, 168, 4, 1);
    IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
    err = esp_netif_set_ip_info(g_apNetif, &ipInfo);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_start(g_apNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return err;
    }
    return ESP_OK;
}

bool portalActiveForDnsTask() {
    MutexLock lock(pdMS_TO_TICKS(100));
    return lock.locked() && g_snapshot.portalActive;
}

bool buildDnsPortalResponse(const uint8_t* request, size_t requestLen, uint8_t* response, size_t responseSize,
                            size_t* responseLen) {
    if (request == nullptr || response == nullptr || responseLen == nullptr || requestLen < 12 || responseSize < 32) {
        return false;
    }

    const uint16_t questionCount = (static_cast<uint16_t>(request[4]) << 8) | request[5];
    if (questionCount == 0) {
        return false;
    }

    size_t cursor = 12;
    while (cursor < requestLen && request[cursor] != 0) {
        const uint8_t labelLen = request[cursor];
        if ((labelLen & 0xC0) != 0 || labelLen > 63 || cursor + 1 + labelLen >= requestLen) {
            return false;
        }
        cursor += 1 + labelLen;
    }
    const size_t questionEnd = cursor + 5;
    if (cursor >= requestLen || questionEnd > requestLen || questionEnd + 16 > responseSize) {
        return false;
    }

    memcpy(response, request, questionEnd);
    response[2] = 0x81;
    response[3] = 0x80;
    response[4] = 0x00;
    response[5] = 0x01;
    response[6] = 0x00;
    response[7] = 0x01;
    response[8] = 0x00;
    response[9] = 0x00;
    response[10] = 0x00;
    response[11] = 0x00;

    size_t out = questionEnd;
    response[out++] = 0xC0;
    response[out++] = 0x0C;
    response[out++] = 0x00;
    response[out++] = 0x01;
    response[out++] = 0x00;
    response[out++] = 0x01;
    response[out++] = static_cast<uint8_t>((kDnsTtlSeconds >> 24) & 0xFF);
    response[out++] = static_cast<uint8_t>((kDnsTtlSeconds >> 16) & 0xFF);
    response[out++] = static_cast<uint8_t>((kDnsTtlSeconds >> 8) & 0xFF);
    response[out++] = static_cast<uint8_t>(kDnsTtlSeconds & 0xFF);
    response[out++] = 0x00;
    response[out++] = 0x04;
    response[out++] = 192;
    response[out++] = 168;
    response[out++] = 4;
    response[out++] = 1;

    *responseLen = out;
    return true;
}

int openDnsSocket() {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        LOG_W(TAG_NET_IDF, "DNS portal socket create failed: errno=%d", errno);
        return -1;
    }

    const int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    timeval timeout = {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPortalDnsPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_W(TAG_NET_IDF, "DNS portal bind failed: errno=%d", errno);
        close(sock);
        return -1;
    }
    LOG_I(TAG_NET_IDF, "DNS captive portal started on UDP %u", static_cast<unsigned>(kPortalDnsPort));
    return sock;
}

void dnsTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_DNS");

    int sock = -1;
    uint8_t request[512] = {};
    uint8_t response[512] = {};

    while (true) {
        if (!portalActiveForDnsTask()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
                LOG_I(TAG_NET_IDF, "DNS captive portal stopped");
            }
            AppIdfTasks::feedCurrentTaskWatchdog();
            vTaskDelay(pdMS_TO_TICKS(kDnsPollDelayMs));
            continue;
        }

        if (sock < 0) {
            sock = openDnsSocket();
            if (sock < 0) {
                AppIdfTasks::feedCurrentTaskWatchdog();
                vTaskDelay(pdMS_TO_TICKS(kDnsRetryDelayMs));
                continue;
            }
        }

        sockaddr_storage source = {};
        socklen_t sourceLen = sizeof(source);
        const int requestLen = recvfrom(sock, request, sizeof(request), 0, reinterpret_cast<sockaddr*>(&source),
                                        &sourceLen);
        if (requestLen < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_W(TAG_NET_IDF, "DNS portal recv failed: errno=%d", errno);
                close(sock);
                sock = -1;
            }
            AppIdfTasks::feedCurrentTaskWatchdog();
            continue;
        }

        size_t responseLen = 0;
        if (buildDnsPortalResponse(request, static_cast<size_t>(requestLen), response, sizeof(response), &responseLen)) {
            sendto(sock, response, responseLen, 0, reinterpret_cast<sockaddr*>(&source), sourceLen);
        }
        AppIdfTasks::feedCurrentTaskWatchdog();
    }
}

esp_err_t ensureDnsTask() {
    if (g_dnsTaskMemory.handle != nullptr) {
        return ESP_OK;
    }
    return AppIdfTasks::createPinnedToCoreInternal(dnsTask, "IDF_DNS", kDnsTaskStackWords, nullptr, 2, 0,
                                                   &g_dnsTaskMemory);
}

void updateRssiLocked() {
    if (!g_snapshot.wifiStarted || !g_snapshot.connected) {
        g_snapshot.rssi = -127;
        g_snapshot.channel = 0;
        return;
    }

    wifi_ap_record_t apInfo = {};
    if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
        g_snapshot.rssi = apInfo.rssi;
        g_snapshot.channel = apInfo.primary;
        copyText(g_snapshot.ssid, sizeof(g_snapshot.ssid), reinterpret_cast<const char*>(apInfo.ssid));
    } else {
        g_snapshot.rssi = -127;
        g_snapshot.channel = 0;
    }
}

bool wifiStartedSnapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    return lock.locked() && g_snapshot.wifiStarted;
}

void handleWifiEvent(void*, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    if (eventBase != WIFI_EVENT) {
        return;
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }

    switch (eventId) {
        case WIFI_EVENT_STA_START:
            g_snapshot.wifiStarted = true;
            LOG_I(TAG_NET_IDF, "WiFi STA started");
            break;
        case WIFI_EVENT_STA_STOP:
            g_snapshot.wifiStarted = false;
            g_snapshot.connecting = false;
            g_snapshot.connected = false;
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
            LOG_I(TAG_NET_IDF, "WiFi STA stopped");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t* event = static_cast<const wifi_event_sta_connected_t*>(eventData);
            g_snapshot.connecting = true;
            g_snapshot.disconnectReason = 0;
            if (event != nullptr) {
                g_snapshot.channel = event->channel;
                char ssid[kMaxSsidLen + 1] = {};
                const size_t ssidLen = event->ssid_len < kMaxSsidLen ? event->ssid_len : kMaxSsidLen;
                memcpy(ssid, event->ssid, ssidLen);
                ssid[ssidLen] = '\0';
                copyText(g_snapshot.ssid, sizeof(g_snapshot.ssid), ssid);
            }
            LOG_I(TAG_NET_IDF, "WiFi STA associated: %s channel=%u",
                  g_snapshot.ssid,
                  static_cast<unsigned>(g_snapshot.channel));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t* event = static_cast<const wifi_event_sta_disconnected_t*>(eventData);
            g_snapshot.connected = false;
            g_snapshot.connecting = false;
            g_snapshot.rssi = -127;
            g_snapshot.channel = 0;
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
            g_snapshot.disconnectReason = event != nullptr ? event->reason : 0;
            g_snapshot.wifiFailCount++;
            LOG_W(TAG_NET_IDF, "WiFi STA disconnected reason=%u failCount=%u",
                  static_cast<unsigned>(g_snapshot.disconnectReason),
                  static_cast<unsigned>(g_snapshot.wifiFailCount));
            break;
        }
        case WIFI_EVENT_AP_START:
            g_snapshot.wifiStarted = true;
            g_snapshot.portalActive = true;
            ensurePortalSsidLocked();
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "192.168.4.1");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "portal_ready");
            LOG_W(TAG_NET_IDF, "WiFi provisioning portal AP started: %s", g_snapshot.portalSsid);
            break;
        case WIFI_EVENT_AP_STOP:
            g_snapshot.portalActive = false;
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "0.0.0.0");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "");
            LOG_I(TAG_NET_IDF, "WiFi provisioning portal AP stopped");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "client_connected");
            LOG_I(TAG_NET_IDF, "WiFi provisioning client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (!g_snapshot.connecting) {
                copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "portal_ready");
            }
            LOG_I(TAG_NET_IDF, "WiFi provisioning client disconnected");
            break;
        default:
            break;
    }
}

void handleIpEvent(void*, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    if (eventBase != IP_EVENT || eventId != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t* event = static_cast<const ip_event_got_ip_t*>(eventData);
    MutexLock lock(pdMS_TO_TICKS(100));
    if (!lock.locked()) {
        return;
    }

    if (event != nullptr) {
        snprintf(g_snapshot.ip, sizeof(g_snapshot.ip), IPSTR, IP2STR(&event->ip_info.ip));
    }
    g_snapshot.connected = true;
    g_snapshot.connecting = false;
    g_snapshot.disconnectReason = 0;
    g_snapshot.wifiFailCount = 0;
    applyDnsConfig();
    updateRssiLocked();
    LOG_I(TAG_NET_IDF, "WiFi got IP: %s rssi=%d", g_snapshot.ip, static_cast<int>(g_snapshot.rssi));
    AppIdfTime::onNetworkUp();
}

esp_err_t registerEventHandlers() {
    if (g_wifiEventHandler == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handleWifiEvent, nullptr,
                                                            &g_wifiEventHandler);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (g_ipEventHandler == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handleIpEvent, nullptr,
                                                            &g_ipEventHandler);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t initWifiDriver() {
    const bool alreadyInitialized = g_snapshot.initialized;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (g_staNetif == nullptr) {
        g_staNetif = esp_netif_create_default_wifi_sta();
        if (g_staNetif == nullptr) {
            return ESP_FAIL;
        }
    }

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&initConfig);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = registerEventHandlers();
    if (err != ESP_OK) {
        return err;
    }

    if (!alreadyInitialized) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_wifi_set_mode(g_snapshot.portalActive ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            return err;
        }
        err = esp_wifi_set_max_tx_power(78);
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
            return err;
        }
        applyDnsConfig();
        g_snapshot.initialized = true;
    }

    return ESP_OK;
}

esp_err_t copyCredentialsToWifiConfig(const char* ssid, const char* password, wifi_config_t* outConfig) {
    if (ssid == nullptr || ssid[0] == '\0' || outConfig == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssidLen = strlen(ssid);
    const size_t passwordLen = password != nullptr ? strlen(password) : 0;
    if (ssidLen == 0 || ssidLen > kMaxSsidLen || passwordLen > 63) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(outConfig, 0, sizeof(*outConfig));
    memcpy(outConfig->sta.ssid, ssid, ssidLen);
    if (passwordLen > 0) {
        memcpy(outConfig->sta.password, password, passwordLen);
    }
    outConfig->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    outConfig->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    outConfig->sta.threshold.rssi = -90;
    outConfig->sta.threshold.authmode = WIFI_AUTH_OPEN;
    return ESP_OK;
}

void ensurePortalSsidLocked() {
    if (g_snapshot.portalSsid[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(g_snapshot.portalSsid, sizeof(g_snapshot.portalSsid), "HC-A1000-%02X%02X", mac[4], mac[5]);
}

void jsonEscape(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; input != nullptr && input[i] != '\0' && out + 1 < outputSize; ++i) {
        const char c = input[i];
        if ((c == '"' || c == '\\') && out + 2 < outputSize) {
            output[out++] = '\\';
            output[out++] = c;
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            output[out++] = c;
        }
    }
    output[out] = '\0';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void urlDecode(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }
    size_t out = 0;
    for (size_t i = 0; input != nullptr && input[i] != '\0' && out + 1 < outputSize; ++i) {
        if (input[i] == '+') {
            output[out++] = ' ';
        } else if (input[i] == '%' && input[i + 1] != '\0' && input[i + 2] != '\0') {
            const int hi = hexValue(input[i + 1]);
            const int lo = hexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output[out++] = static_cast<char>((hi << 4) | lo);
                i += 2;
            }
        } else {
            output[out++] = input[i];
        }
    }
    output[out] = '\0';
}

bool formValue(const char* body, const char* key, char* output, size_t outputSize) {
    if (body == nullptr || key == nullptr || output == nullptr || outputSize == 0) {
        return false;
    }
    output[0] = '\0';
    const size_t keyLen = strlen(key);
    const char* cursor = body;
    while (cursor != nullptr && *cursor != '\0') {
        const char* next = strchr(cursor, '&');
        const size_t fieldLen = next != nullptr ? static_cast<size_t>(next - cursor) : strlen(cursor);
        if (fieldLen > keyLen && strncmp(cursor, key, keyLen) == 0 && cursor[keyLen] == '=') {
            char encoded[256] = {};
            const size_t valueLen = fieldLen - keyLen - 1;
            const size_t copyLen = valueLen < sizeof(encoded) - 1 ? valueLen : sizeof(encoded) - 1;
            memcpy(encoded, cursor + keyLen + 1, copyLen);
            encoded[copyLen] = '\0';
            urlDecode(encoded, output, outputSize);
            trimInPlace(output);
            return true;
        }
        cursor = next != nullptr ? next + 1 : nullptr;
    }
    return false;
}

esp_err_t receiveRequestBody(httpd_req_t* request, char* body, size_t bodySize) {
    if (request == nullptr || body == nullptr || bodySize == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->content_len >= bodySize) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t received = 0;
    while (received < request->content_len) {
        const int ret = httpd_req_recv(request, body + received, request->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(ret);
    }
    body[received] = '\0';
    return ESP_OK;
}

esp_err_t sendJson(httpd_req_t* request, const char* json) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json != nullptr ? json : "{}");
}

esp_err_t handlePortalRoot(httpd_req_t* request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, kPortalHtml);
}

esp_err_t handlePortalScan(httpd_req_t* request) {
    ScanResult* results = static_cast<ScanResult*>(
        heap_caps_calloc(kMaxScanResults, sizeof(ScanResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (results == nullptr) {
        results = static_cast<ScanResult*>(heap_caps_calloc(kMaxScanResults, sizeof(ScanResult), MALLOC_CAP_8BIT));
    }
    char* json = static_cast<char*>(heap_caps_calloc(kPortalScanJsonSize, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (json == nullptr) {
        json = static_cast<char*>(heap_caps_calloc(kPortalScanJsonSize, sizeof(char), MALLOC_CAP_8BIT));
    }
    if (results == nullptr || json == nullptr) {
        if (results != nullptr) {
            heap_caps_free(results);
        }
        if (json != nullptr) {
            heap_caps_free(json);
        }
        return sendJson(request, "{\"networks\":[],\"ok\":false,\"error\":\"ESP_ERR_NO_MEM\"}");
    }

    size_t count = 0;
    const esp_err_t err = scan(results, kMaxScanResults, &count);
    LOG_I(TAG_NET_IDF,
          "Portal WiFi scan completed: %s, networks=%u",
          esp_err_to_name(err),
          static_cast<unsigned>(count));

    size_t used = snprintf(json, kPortalScanJsonSize, "{\"networks\":[");
    if (err == ESP_OK) {
        for (size_t i = 0; i < count && used + 80 < kPortalScanJsonSize; ++i) {
            char escapedSsid[80] = {};
            jsonEscape(results[i].ssid, escapedSsid, sizeof(escapedSsid));
            used += snprintf(json + used,
                             kPortalScanJsonSize - used,
                             "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                             i == 0 ? "" : ",",
                             escapedSsid,
                             static_cast<int>(results[i].rssi),
                             results[i].secure ? "true" : "false");
        }
    }
    snprintf(json + used,
             kPortalScanJsonSize - used,
             "],\"ok\":%s,\"error\":\"%s\"}",
             err == ESP_OK ? "true" : "false",
             err == ESP_OK ? "OK" : esp_err_to_name(err));
    const esp_err_t sendErr = sendJson(request, json);
    heap_caps_free(results);
    heap_caps_free(json);
    return sendErr;
}

esp_err_t handlePortalSave(httpd_req_t* request) {
    char body[320] = {};
    char ssid[kMaxSsidLen + 1] = {};
    char password[kMaxPasswordLen + 1] = {};
    esp_err_t err = receiveRequestBody(request, body, sizeof(body));
    if (err == ESP_OK && !formValue(body, "ssid", ssid, sizeof(ssid))) {
        err = ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK) {
        formValue(body, "password", password, sizeof(password));
        err = saveCredentials(ssid, password);
    }

    char json[192] = {};
    if (err == ESP_OK) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "connecting");
        }
        snprintf(json, sizeof(json), "{\"ok\":true,\"message\":\"saved_connecting\"}");
    } else {
        snprintf(json,
                 sizeof(json),
                 "{\"ok\":false,\"message\":\"%s\"}",
                 err == ESP_ERR_INVALID_ARG ? "invalid_ssid" : esp_err_to_name(err));
    }
    sendJson(request, json);

    if (err == ESP_OK) {
        connect(ssid, password, false);
    }
    return ESP_OK;
}

esp_err_t handlePortalStatus(httpd_req_t* request) {
    Snapshot snap = snapshot();
    char ssid[80] = {};
    char apSsid[32] = {};
    char hint[48] = {};
    jsonEscape(snap.ssid, ssid, sizeof(ssid));
    jsonEscape(snap.portalSsid, apSsid, sizeof(apSsid));
    jsonEscape(snap.portalHint, hint, sizeof(hint));

    char json[384] = {};
    snprintf(json,
             sizeof(json),
             "{\"portal\":%s,\"waiting\":%s,\"connected\":%s,\"ip\":\"%s\","
             "\"ssid\":\"%s\",\"ap_ssid\":\"%s\",\"hint\":\"%s\"}",
             snap.portalActive ? "true" : "false",
             snap.connecting ? "true" : "false",
             snap.connected ? "true" : "false",
             snap.ip,
             ssid,
             apSsid,
             hint);
    return sendJson(request, json);
}

esp_err_t handlePortalNotFound(httpd_req_t* request, httpd_err_code_t) {
    return handlePortalRoot(request);
}

esp_err_t registerPortalUri(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t config = {};
    config.uri = uri;
    config.method = method;
    config.handler = handler;
    config.user_ctx = nullptr;
    return httpd_register_uri_handler(g_httpServer, &config);
}

esp_err_t startHttpServer() {
    if (g_httpServer != nullptr) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = kPortalHttpStackBytes;
    esp_err_t err = httpd_start(&g_httpServer, &config);
    if (err != ESP_OK) {
        g_httpServer = nullptr;
        return err;
    }

    err = registerPortalUri("/", HTTP_GET, handlePortalRoot);
    if (err == ESP_OK) {
        err = registerPortalUri("/scan", HTTP_GET, handlePortalScan);
    }
    if (err == ESP_OK) {
        err = registerPortalUri("/save", HTTP_POST, handlePortalSave);
    }
    if (err == ESP_OK) {
        err = registerPortalUri("/status", HTTP_GET, handlePortalStatus);
    }
    if (err == ESP_OK) {
        err = registerPortalUri("/generate_204", HTTP_GET, handlePortalRoot);
    }
    if (err == ESP_OK) {
        err = registerPortalUri("/generate204", HTTP_GET, handlePortalRoot);
    }
    if (err == ESP_OK) {
        err = httpd_register_err_handler(g_httpServer, HTTPD_404_NOT_FOUND, handlePortalNotFound);
    }
    if (err != ESP_OK) {
        httpd_stop(g_httpServer);
        g_httpServer = nullptr;
        return err;
    }
    return ESP_OK;
}

void stopHttpServer() {
    if (g_httpServer != nullptr) {
        httpd_stop(g_httpServer);
        g_httpServer = nullptr;
    }
}

}  // namespace

esp_err_t start() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    {
        MutexLock lock;
        if (!lock.locked()) {
            return setLastError(ESP_ERR_TIMEOUT);
        }

        err = initWifiDriver();
        if (err != ESP_OK) {
            LOG_E(TAG_NET_IDF, "WiFi driver init failed: %s", esp_err_to_name(err));
            return setLastError(err);
        }
    }

    Credentials credentials;
    err = loadCredentials(&credentials);
    if (err == ESP_OK) {
        g_credentials = credentials;
        g_snapshot.credentialsLoaded = true;
        return connect(credentials.ssid, credentials.password, false);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NOT_FOUND) {
        g_snapshot.credentialsLoaded = false;
        LOG_W(TAG_NET_IDF, "No stored WiFi credentials; starting provisioning portal");
        return startPortal();
    }

    LOG_W(TAG_NET_IDF, "Stored WiFi credential load failed: %s", esp_err_to_name(err));
    return setLastError(err);
}

bool isInitialized() {
    return g_snapshot.initialized;
}

bool isConnected() {
    return g_snapshot.connected;
}

Snapshot snapshot() {
    MutexLock lock(pdMS_TO_TICKS(50));
    if (lock.locked()) {
        updateRssiLocked();
    }
    return g_snapshot;
}

esp_err_t loadCredentials(Credentials* credentials) {
    if (credentials == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *credentials = {};

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kWifiPrefsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }

    size_t ssidLen = sizeof(credentials->ssid);
    err = nvs_get_str(handle, kWifiSsidKey, credentials->ssid, &ssidLen);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }

    size_t passwordLen = sizeof(credentials->password);
    const esp_err_t passwordErr = nvs_get_str(handle, kWifiPasswordKey, credentials->password, &passwordLen);
    nvs_close(handle);
    if (passwordErr != ESP_OK && passwordErr != ESP_ERR_NVS_NOT_FOUND) {
        return passwordErr;
    }

    trimInPlace(credentials->ssid);
    return credentials->ssid[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t saveCredentials(const char* ssid, const char* password) {
    if (ssid == nullptr || ssid[0] == '\0' || strlen(ssid) > kMaxSsidLen ||
        (password != nullptr && strlen(password) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kWifiPrefsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = nvs_set_str(handle, kWifiSsidKey, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kWifiPasswordKey, password != nullptr ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        copyText(g_credentials.ssid, sizeof(g_credentials.ssid), ssid);
        copyText(g_credentials.password, sizeof(g_credentials.password), password != nullptr ? password : "");
        g_snapshot.wifiFailCount = 0;
        g_snapshot.credentialsLoaded = true;
        LOG_I(TAG_NET_IDF, "WiFi credentials saved to NVS: %s", ssid);
    }
    return setLastError(err);
}

esp_err_t clearCredentials() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kWifiPrefsNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, kWifiSsidKey);
        nvs_erase_key(handle, kWifiPasswordKey);
        err = nvs_commit(handle);
        nvs_close(handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        g_credentials = {};
        g_snapshot.wifiFailCount = 0;
        g_snapshot.credentialsLoaded = false;
        // 断开当前 STA 连接，避免凭据已清但 WiFi 仍在线的不一致状态
        esp_wifi_disconnect();
        LOG_I(TAG_NET_IDF, "WiFi credentials cleared and STA disconnected");
    }
    return setLastError(err);
}

bool hasStoredCredentials() {
    Credentials credentials;
    return loadCredentials(&credentials) == ESP_OK;
}

esp_err_t connectStored() {
    Credentials credentials;
    const esp_err_t err = loadCredentials(&credentials);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    return connect(credentials.ssid, credentials.password, false);
}

esp_err_t connect(const char* ssid, const char* password, bool saveToNvs) {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = initWifiDriver();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    wifi_config_t wifiConfig = {};
    err = copyCredentialsToWifiConfig(ssid, password, &wifiConfig);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    if (saveToNvs) {
        err = saveCredentials(ssid, password);
        if (err != ESP_OK) {
            return err;
        }
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            copyText(g_snapshot.ssid, sizeof(g_snapshot.ssid), ssid);
            g_snapshot.credentialsLoaded = true;
            g_snapshot.connecting = true;
            g_snapshot.connected = false;
            g_snapshot.disconnectReason = 0;
            copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
            copyText(g_credentials.ssid, sizeof(g_credentials.ssid), ssid);
            copyText(g_credentials.password, sizeof(g_credentials.password), password != nullptr ? password : "");
        }
    }

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(kConnectWaitMs));
    const bool keepPortalActive = isPortalActive();
    err = esp_wifi_set_mode(keepPortalActive ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return setLastError(err);
    }
    err = esp_wifi_connect();
    if (err == ESP_OK) {
        LOG_I(TAG_NET_IDF, "WiFi connect requested: %s", ssid);
    } else {
        LOG_W(TAG_NET_IDF, "WiFi connect request failed: %s", esp_err_to_name(err));
    }
    return setLastError(err);
}

esp_err_t disconnect(bool stopWifi) {
    if (stopWifi) {
        stopHttpServer();
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        err = ESP_OK;
    }
    if (stopWifi) {
        const esp_err_t stopErr = esp_wifi_stop();
        if (err == ESP_OK && stopErr != ESP_ERR_WIFI_NOT_STARTED) {
            err = stopErr;
        }
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.connecting = false;
        g_snapshot.connected = false;
        g_snapshot.rssi = -127;
        g_snapshot.channel = 0;
        copyText(g_snapshot.ip, sizeof(g_snapshot.ip), "0.0.0.0");
        if (stopWifi) {
            g_snapshot.portalActive = false;
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "0.0.0.0");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "");
        }
    }
    return setLastError(err);
}

esp_err_t setDefaultRoute() {
    if (g_staNetif == nullptr || !isConnected()) {
        return setLastError(ESP_ERR_INVALID_STATE);
    }
    return setLastError(esp_netif_set_default_netif(g_staNetif));
}

esp_err_t scan(ScanResult* results, size_t maxResults, size_t* count) {
    if (count == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (results == nullptr || maxResults == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = initWifiDriver();
    if (err != ESP_OK) {
        return setLastError(err);
    }
    if (!wifiStartedSnapshot()) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            return setLastError(err);
        }
    }

    wifi_scan_config_t scanConfig = {};
    scanConfig.ssid = nullptr;
    scanConfig.bssid = nullptr;
    scanConfig.channel = 0;
    scanConfig.show_hidden = true;
    scanConfig.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanConfig.scan_time.active.min = kScanActiveMinMs;
    scanConfig.scan_time.active.max = kScanActiveMaxMs;

    err = esp_wifi_scan_start(&scanConfig, true);
    if (err != ESP_OK) {
        return setLastError(err);
    }

    uint16_t apCount = 0;
    err = esp_wifi_scan_get_ap_num(&apCount);
    if (err != ESP_OK) {
        return setLastError(err);
    }

    wifi_ap_record_t records[kMaxScanResults] = {};
    uint16_t recordCount = apCount < maxResults ? apCount : static_cast<uint16_t>(maxResults);
    if (recordCount > kMaxScanResults) {
        recordCount = kMaxScanResults;
    }
    err = esp_wifi_scan_get_ap_records(&recordCount, records);
    if (err != ESP_OK) {
        return setLastError(err);
    }

    for (uint16_t i = 0; i < recordCount; ++i) {
        copyText(results[i].ssid, sizeof(results[i].ssid), reinterpret_cast<const char*>(records[i].ssid));
        results[i].rssi = records[i].rssi;
        results[i].authMode = records[i].authmode;
        results[i].channel = records[i].primary;
        results[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
    }
    *count = recordCount;
    return ESP_OK;
}

esp_err_t startPortal() {
    esp_err_t err = ensureMutex();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    if (!hasPortalMemoryHeadroom()) {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "low_memory");
        }
        return setLastError(ESP_ERR_NO_MEM);
    }

    err = initWifiDriver();
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = ensureApNetif();
    if (err != ESP_OK) {
        return setLastError(err);
    }

    char portalSsid[kMaxPortalSsidLen] = {};
    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            ensurePortalSsidLocked();
            copyText(portalSsid, sizeof(portalSsid), g_snapshot.portalSsid);
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "192.168.4.1");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "portal_starting");
        }
    }
    if (portalSsid[0] == '\0') {
        copyText(portalSsid, sizeof(portalSsid), "HC-A1000");
    }

    wifi_config_t apConfig = {};
    const size_t ssidLen = strlen(portalSsid);
    memcpy(apConfig.ap.ssid, portalSsid, ssidLen);
    apConfig.ap.ssid_len = static_cast<uint8_t>(ssidLen);
    apConfig.ap.channel = kPortalChannel;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;
    apConfig.ap.max_connection = kPortalMaxConnections;
    apConfig.ap.beacon_interval = 100;
    apConfig.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &apConfig);
    if (err != ESP_OK) {
        return setLastError(err);
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return setLastError(err);
    }
    vTaskDelay(pdMS_TO_TICKS(kPortalApReadyDelayMs));

    err = startHttpServer();
    if (err != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.portalActive = false;
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "0.0.0.0");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "portal_http_error");
        }
        return setLastError(err);
    }
    const esp_err_t dnsErr = ensureDnsTask();
    if (dnsErr != ESP_OK) {
        LOG_W(TAG_NET_IDF, "DNS captive portal task not available: %s", esp_err_to_name(dnsErr));
    }

    {
        MutexLock lock(pdMS_TO_TICKS(100));
        if (lock.locked()) {
            g_snapshot.wifiStarted = true;
            g_snapshot.portalActive = true;
            copyText(g_snapshot.portalSsid, sizeof(g_snapshot.portalSsid), portalSsid);
            copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "192.168.4.1");
            copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "portal_ready");
        }
    }

    LOG_W(TAG_NET_IDF, "Provisioning portal ready: SSID=%s IP=192.168.4.1", portalSsid);
    return setLastError(ESP_OK);
}

esp_err_t stopPortal() {
    stopHttpServer();

    esp_err_t err = ESP_OK;
    if (g_snapshot.initialized) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
            err = ESP_OK;
        }
    }

    MutexLock lock(pdMS_TO_TICKS(100));
    if (lock.locked()) {
        g_snapshot.portalActive = false;
        copyText(g_snapshot.portalIp, sizeof(g_snapshot.portalIp), "0.0.0.0");
        copyText(g_snapshot.portalHint, sizeof(g_snapshot.portalHint), "");
    }
    LOG_I(TAG_NET_IDF, "Provisioning portal stopped");
    return setLastError(err);
}

bool isPortalActive() {
    return snapshot().portalActive;
}

uint32_t dnsTaskStackHighWatermark() {
    return g_dnsTaskMemory.handle != nullptr ? uxTaskGetStackHighWaterMark(g_dnsTaskMemory.handle) : 0;
}

const char* authModeName(wifi_auth_mode_t authMode) {
    switch (authMode) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE:
            return "enterprise";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2/wpa3";
        default:
            return "unknown";
    }
}

}  // namespace AppIdfNetwork
