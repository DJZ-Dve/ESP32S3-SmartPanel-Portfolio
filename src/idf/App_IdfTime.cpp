#include "App_IdfTime.h"

#include <atomic>
#include <stdlib.h>
#include <time.h>

#include "App_Log.h"
#include "esp_sntp.h"

namespace AppIdfTime {
namespace {

constexpr const char* TAG_TIME = "IDF_TIME";

constexpr const char* kNtpServers[] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
    "time.windows.com",
};
constexpr size_t kNtpServerCount = sizeof(kNtpServers) / sizeof(kNtpServers[0]);

std::atomic<bool> g_inited{false};
std::atomic<bool> g_sntpStarted{false};

void onSntpSync(struct timeval* tv) {
    if (tv != nullptr) {
        time_t sec = tv->tv_sec;
        struct tm tm = {};
        localtime_r(&sec, &tm);
        LOG_I(TAG_TIME, "SNTP sync ok: %04d-%02d-%02d %02d:%02d:%02d",
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

}  // namespace

void init() {
    if (g_inited.exchange(true)) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (size_t i = 0; i < kNtpServerCount && i < SNTP_MAX_SERVERS; ++i) {
        esp_sntp_setservername(static_cast<u8_t>(i), kNtpServers[i]);
    }
    sntp_set_time_sync_notification_cb(onSntpSync);

    LOG_I(TAG_TIME, "Time module init done (TZ=CST-8, %u NTP servers)",
          static_cast<unsigned>(kNtpServerCount));
}

void onNetworkUp() {
    if (!g_inited.load()) {
        // 安全兜底：如果 init 还没调用就先 init
        init();
    }

    if (!g_sntpStarted.exchange(true)) {
        esp_sntp_init();
        LOG_I(TAG_TIME, "SNTP started by network up event");
    } else {
        esp_sntp_restart();
        LOG_I(TAG_TIME, "SNTP restarted by network up event");
    }
}

bool isSynced() {
    time_t now = 0;
    time(&now);
    struct tm tm = {};
    localtime_r(&now, &tm);
    return tm.tm_year >= 124;  // 2024 年起视为已同步
}

}  // namespace AppIdfTime
