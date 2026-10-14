#include "app_mode.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "wifi_link.h" /* wifi_link_storage_prepare：netcfg 分区 NVS 初始化（幂等） */

#define NETCFG_PARTITION "netcfg"
#define NAMESPACE "trio"
#define KEY_MODE "mode"

static const char *TAG = "app_mode";

app_mode_t app_mode_get(void) {
    if (wifi_link_storage_prepare() != ESP_OK) return APP_MODE_LAUNCHER;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(NETCFG_PARTITION, NAMESPACE,
                                            NVS_READONLY, &handle);
    if (err != ESP_OK) return APP_MODE_LAUNCHER; /* 尚未写过 = 首次进主控页 */
    uint8_t mode = 0;
    err = nvs_get_u8(handle, KEY_MODE, &mode);
    nvs_close(handle);
    if (err != ESP_OK || mode > APP_MODE_AI) return APP_MODE_LAUNCHER;
    return (app_mode_t)mode;
}

bool app_mode_set(app_mode_t mode) {
    if (mode > APP_MODE_AI) return false;
    if (wifi_link_storage_prepare() != ESP_OK) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(NETCFG_PARTITION, NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 netcfg/trio 失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(handle, KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存模式 %d 失败: %s", (int)mode, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "运行模式 -> %d", (int)mode);
    return true;
}

bool app_mode_reboot_into(app_mode_t mode) {
    if (!app_mode_set(mode)) return false;
    /* AI = 切到 ota_0 的小智固件；其余模式一律强制 factory，
     * 防止 otadata 残留指向 ota_0 导致选 TODO/PODCAST 后再次进小智。 */
    const esp_partition_t *target =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 mode == APP_MODE_AI
                                     ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                     : ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                 NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "目标启动分区不存在 (mode=%d)", (int)mode);
        return false;
    }
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换启动分区失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "重启进入 %s (mode=%d)", target->label, (int)mode);
    esp_restart(); /* 不返回 */
    return true;   /* 声明兜底，实际不可达 */
}
