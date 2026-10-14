#include "clock_sync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "clock_sync";
static bool s_started;
static volatile bool s_synced;

static void on_time_sync(struct timeval *tv) {
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP 校时成功");
}

void clock_sync_start(void) {
    if (s_started) return;
    /* 设备面向中国市场：固定 UTC+8。CST-8 是 POSIX TZ 语义（UTC 偏移 -8 小时）。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
        return;
    }
    s_started = true;
}

bool clock_sync_synced(void) {
    return s_synced;
}
