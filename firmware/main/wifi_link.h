// main/wifi_link.h —— WiFi STA 连接与凭据管理（netcfg 分区 NVS）。
// 三合一公共基础设施：开机自动连 Wi-Fi 并触发 SNTP；播放器/语音终端复用同一连接。
// 凭据录入 UI（SoftAP 配网）在 UI 阶段接入，本模块先提供保存/读取 API。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    WIFI_LINK_IDLE = 0,        /* 未启动连接 */
    WIFI_LINK_NO_CREDENTIALS,  /* netcfg 分区无凭据 */
    WIFI_LINK_CONNECTING,      /* 正在连接/重试 */
    WIFI_LINK_CONNECTED,       /* 已获 IP，SNTP 已触发 */
    WIFI_LINK_FAILED,          /* 重试耗尽，等待外部重新调用 connect */
} wifi_link_state_t;

/* 一次性网络准备：默认 NVS + netif/event loop + netcfg 分区 NVS + 事件注册。幂等。 */
esp_err_t wifi_link_prepare(void);

/* 仅存储准备：默认 NVS + netif/event loop + netcfg 分区 NVS，不注册事件。
 * 供启动期配网服务在独立接管射频（自建 APSTA、不触发 wifi_link 自动重连）前调用；
 * 之后 main 仍走 wifi_link_prepare 注册事件并连接。幂等。 */
esp_err_t wifi_link_storage_prepare(void);

/* 保存/读取/清除凭据（netcfg 分区，namespace "wifi"）。ssid 1..32 字节，pass 0..64 字节。 */
esp_err_t wifi_link_credentials_save(const char *ssid, const char *pass);
bool wifi_link_credentials_load(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
esp_err_t wifi_link_credentials_clear(void);

/* 用已存凭据发起连接；无凭据返回 ESP_ERR_INVALID_STATE。 */
esp_err_t wifi_link_connect(void);

/* 开机一站式：prepare → 有凭据即连接（main 在后台调用，不阻塞 UI）。 */
void wifi_link_autostart(void);

wifi_link_state_t wifi_link_state(void);
/* 是否已获 IP（连接成功）。 */
bool wifi_link_has_ip(void);

/* 当前 STA IPv4 字符串（诊断用）；未拿到 IP 时写入空串。 */
void wifi_link_ip_str(char *out, size_t cap);
