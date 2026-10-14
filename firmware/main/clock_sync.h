// main/clock_sync.h —— SNTP 校时（WiFi 获得网络后由 wifi_link 自动触发）。
// 时区固定 UTC+8（CST-8）；设备无 RTC，重启后需重新校时。
#pragma once

#include <stdbool.h>

/* 安装时区并启动 SNTP（单服务器 ntp.aliyun.com）；幂等，未联网时也会启动，
   联网后由 lwIP 自动重试直至同步成功。 */
void clock_sync_start(void);

/* SNTP 是否已同步过至少一次（本次开机内）。 */
bool clock_sync_synced(void);
