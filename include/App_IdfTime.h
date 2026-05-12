#pragma once

namespace AppIdfTime {

// 设置 TZ=CST-8 并配置 SNTP 服务器列表（不立即启动 SNTP，等到联网事件再启动）。
// 应在 app_main() 早期调用一次。重复调用安全。
void init();

// WiFi 拿到 IP 或 4G PPP 拨号成功时调用。
// 第一次会启动 SNTP，之后调用会触发 sntp_restart 立即重新拉取。线程安全。
void onNetworkUp();

// 系统时间是否已被同步过（按 tm_year >= 124 / 即 2024 年判断）。
bool isSynced();

}  // namespace AppIdfTime
