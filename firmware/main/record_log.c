#include "record_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#define RECORD_PARTITION "fs"
#define RECORD_BASE_PATH "/fs"
#define RECORD_DIR RECORD_BASE_PATH "/rec"

static const char *TAG = "record_log";
static bool s_ready;
static volatile bool s_error;

bool record_log_init(void) {
    if (s_ready) return true;
    esp_vfs_littlefs_conf_t conf = {
        .base_path = RECORD_BASE_PATH,
        .partition_label = RECORD_PARTITION,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        /* 新设备/损坏镜像：格式化后重试一次 */
        ESP_LOGW(TAG, "fs mount failed (%s); formatting %s", esp_err_to_name(err),
                 RECORD_PARTITION);
        err = esp_littlefs_format(RECORD_PARTITION);
        if (err == ESP_OK) err = esp_vfs_littlefs_register(&conf);
    }
    if (err != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "fs init failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(RECORD_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "fs ready: %u KB total, %u KB used", (unsigned)(total / 1024),
                 (unsigned)(used / 1024));
    }
    s_ready = true;
    return true;
}

bool record_log_append(const char *date, const char *line) {
    if (!s_ready || !date || !line || line[0] == '\0') return false;
    if (strlen(date) != 8) return false;
    for (const char *p = date; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return false;
    }

    if (mkdir(RECORD_DIR, 0777) != 0 && errno != EEXIST) {
        s_error = true;
        ESP_LOGE(TAG, "mkdir %s failed: %s", RECORD_DIR, strerror(errno));
        return false;
    }

    char path[48];
    if (snprintf(path, sizeof(path), RECORD_DIR "/%s.jsonl", date) >= (int)sizeof(path)) {
        return false;
    }
    FILE *f = fopen(path, "a");
    if (f == NULL) {
        s_error = true;
        ESP_LOGE(TAG, "open %s failed: %s", path, strerror(errno));
        return false;
    }
    bool ok = fprintf(f, "%s\n", line) > 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        s_error = true;
        ESP_LOGE(TAG, "write %s failed", path);
    }
    return ok;
}

bool record_log_has_error(void) {
    return s_error;
}
