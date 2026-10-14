#include "wifi_link.h"

#include <string.h>

#include "clock_sync.h"
#include "demo_radio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NETCFG_PARTITION "netcfg"
#define NETCFG_NAMESPACE "wifi"
#define CONNECT_MAX_RETRY 5

#define WIFI_SSID_MAX 33u /* 含 NUL，与 wifi_config_t.sta.ssid 一致 */
#define WIFI_PASS_MAX 65u

static const char *TAG = "wifi_link";
static esp_netif_t *s_sta_netif;
static esp_timer_handle_t s_retry_timer;
static bool s_ready;
static volatile wifi_link_state_t s_state = WIFI_LINK_IDLE;
static bool s_wifi_started;
static int s_retry;

static void on_retry_timer(void *arg);

static void set_state(wifi_link_state_t state) {
    s_state = state;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        if (disc != NULL) {
            ESP_LOGW(TAG, "Wi-Fi 断开 reason=%d", disc->reason);
        }
        if (s_state == WIFI_LINK_CONNECTED) {
            ESP_LOGW(TAG, "网络中断，尝试重连");
        }
        set_state(WIFI_LINK_CONNECTING);
        if (s_retry < CONNECT_MAX_RETRY) {
            s_retry++;
            esp_wifi_connect();
        } else {
            set_state(WIFI_LINK_FAILED);
            ESP_LOGE(TAG, "连接重试 %d 次仍失败，30 秒后自动重试", s_retry);
            if (s_retry_timer == NULL) {
                const esp_timer_create_args_t args = {
                    .callback = &on_retry_timer, .name = "wifi_retry",
                };
                if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) return;
            }
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, 30ULL * 1000 * 1000);
        }
    }
}

static void on_retry_timer(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "自动重试连接");
    s_retry = 0;
    (void)wifi_link_connect();
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_retry = 0;
    set_state(WIFI_LINK_CONNECTED);
    ESP_LOGI(TAG, "已获 IP，启动 SNTP 校时");
    clock_sync_start();
}

esp_err_t wifi_link_storage_prepare(void) {
    static bool storage_ready;
    if (storage_ready) return ESP_OK;

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    /* netcfg 分区只存配网凭据，损坏时清空重初始化是安全的。 */
    err = nvs_flash_init_partition(NETCFG_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "netcfg 分区需重置: %s", esp_err_to_name(err));
        err = nvs_flash_erase_partition(NETCFG_PARTITION);
        if (err == ESP_OK) err = nvs_flash_init_partition(NETCFG_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netcfg 分区初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    storage_ready = true;
    return ESP_OK;
}

esp_err_t wifi_link_prepare(void) {
    static bool ready;
    if (ready) return ESP_OK;

    esp_err_t err = wifi_link_storage_prepare();
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "事件注册失败: %s", esp_err_to_name(err));
        return err;
    }
    ready = true;
    return ESP_OK;
}

static nvs_handle_t netcfg_open(esp_err_t *err_out) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(NETCFG_PARTITION, NETCFG_NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err_out != NULL) *err_out = err;
    return err == ESP_OK ? handle : 0;
}

esp_err_t wifi_link_credentials_save(const char *ssid, const char *pass) {
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;
    size_t ssid_len = strnlen(ssid, WIFI_SSID_MAX);
    size_t pass_len = strnlen(pass, WIFI_PASS_MAX);
    if (ssid_len == 0 || ssid_len >= WIFI_SSID_MAX || pass_len >= WIFI_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    nvs_handle_t handle = netcfg_open(&err);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", pass);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "凭据已保存 (ssid=%s)", ssid);
    return err;
}

bool wifi_link_credentials_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap) {
    if (!ssid || ssid_cap < WIFI_SSID_MAX || !pass || pass_cap < WIFI_PASS_MAX) return false;
    nvs_handle_t handle;
    if (nvs_open_from_partition(NETCFG_PARTITION, NETCFG_NAMESPACE, NVS_READONLY, &handle)
        != ESP_OK) {
        return false;
    }
    size_t ssid_len = ssid_cap;
    size_t pass_len = pass_cap;
    bool ok = nvs_get_str(handle, "ssid", ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(handle, "pass", pass, &pass_len) == ESP_OK;
    nvs_close(handle);
    return ok && ssid[0] != '\0';
}

esp_err_t wifi_link_credentials_clear(void) {
    esp_err_t err;
    nvs_handle_t handle = netcfg_open(&err);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, "ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(handle, "pass");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_link_connect(void) {
    esp_err_t err = wifi_link_prepare();
    if (err != ESP_OK) return err;

    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
    if (!wifi_link_credentials_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        set_state(WIFI_LINK_NO_CREDENTIALS);
        return ESP_ERR_INVALID_STATE;
    }

    /* STA netif 只创建一次：缺失时关联成功也不会启动 DHCP，永远拿不到 IP */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "连接 %s (密码长度 %u)", ssid, (unsigned)strnlen(pass, WIFI_PASS_MAX));

    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            /* 凭据必须在 start 之前写入：STA_START 事件会立即发起 connect */
            wifi_config_t wc = {0};
            strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
            strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
            err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        set_state(WIFI_LINK_CONNECTING);
        return ESP_OK; /* STA_START 事件里发起 connect */
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err == ESP_OK) set_state(WIFI_LINK_CONNECTING);
    return err;
}

void wifi_link_autostart(void) {
    if (wifi_link_prepare() != ESP_OK) return;
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
    if (!wifi_link_credentials_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        set_state(WIFI_LINK_NO_CREDENTIALS);
        ESP_LOGI(TAG, "netcfg 无凭据，跳过自动联网（待配网 UI 录入）");
        return;
    }
    s_retry = 0;
    set_state(WIFI_LINK_CONNECTING);
    esp_err_t err = wifi_link_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "自动联网失败: %s", esp_err_to_name(err));
    }
}

wifi_link_state_t wifi_link_state(void) {
    return s_state;
}

bool wifi_link_has_ip(void) {
    return s_state == WIFI_LINK_CONNECTED;
}

/* 当前 STA IPv4 字符串（诊断用）；未拿到 IP 返回空串。 */
void wifi_link_ip_str(char *out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    if (s_sta_netif == NULL) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) return;
    if (info.ip.addr == 0) return;
    snprintf(out, cap, IPSTR, IP2STR(&info.ip));
}
