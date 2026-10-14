#include "trio_setup.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_display.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "todo_cfg.h"
#include "wifi_link.h"

#define NETCFG_PARTITION "netcfg"
#define WIFI_NAMESPACE "wifi"
#define SETUP_FLAG_KEY "setup"
#define SETUP_IDLE_TIMEOUT_US (10LL * 60 * 1000 * 1000) /* 无提交 10 分钟自动放弃,进入应用 */
#define STATUS_MAX 96
#define SCAN_CACHE_MAX 16

static const char *TAG = "trio_setup";

/* 屏幕与网页共用的固定提示语；saved 判定依赖 SAVED_MSG 原文比较。 */
#define SAVED_MSG "Saved! Starting app..."

typedef struct {
    char ssid[33];
    char pass[65];
    char server[TODO_CFG_SERVER_MAX];
    char code[TODO_CFG_CODE_MAX];
} setup_input_t;

static atomic_bool s_active, s_cancelled, s_busy;
static QueueHandle_t s_submissions;
static SemaphoreHandle_t s_status_lock;
static char s_status[STATUS_MAX], s_hotspot[32], s_hotspot_pass[17];
static httpd_handle_t s_web;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static bool s_screen_ready, s_wifi_inited, s_wifi_started;
static bool s_can_cancel;
/* 最近一次网页活动（HTTP 请求或提交），由 s_status_lock 保护。 */
static int64_t s_last_activity;
static void activity_touch(void);
static bool activity_expired(void);

/* 手机连着热点时 C3 单射频的扫描会被限制在 AP 信道（瞬间完成、只见零星 AP），
 * 且信道跳变会重置手机连接。因此在 AP 启动后、客户端接入前做一次全信道扫描并缓存，
 * 页面始终读缓存。 */
static wifi_ap_record_t s_scan_cache[SCAN_CACHE_MAX];
static uint16_t s_scan_cache_count;

/* ---------- 设备屏幕（英文文案：Montserrat 无中文字形） ---------- */

static lv_obj_t *s_scr, *s_status_label;

static lv_obj_t *setup_label(lv_obj_t *parent, lv_coord_t y, const char *text,
                             const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 216);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static bool screen_create(void) {
    if (!bsp_lvgl_lock(1000)) return false;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0xF7F5F0), 0); /* 与 trio 暖米白一致 */
    setup_label(s_scr, 24, "SETUP", &lv_font_montserrat_20, 0x1F2430);
    char line[64];
    snprintf(line, sizeof(line), "Wi-Fi: %s", s_hotspot);
    setup_label(s_scr, 72, line, &lv_font_montserrat_14, 0x1F2430);
    snprintf(line, sizeof(line), "PASS: %s", s_hotspot_pass);
    setup_label(s_scr, 96, line, &lv_font_montserrat_14, 0x1F2430);
    setup_label(s_scr, 120, "Open: http://192.168.4.1", &lv_font_montserrat_14, 0x626874);
    s_status_label = setup_label(s_scr, 176, "", &lv_font_montserrat_14, 0xE0533D);
    lv_screen_load(s_scr);
    bsp_lvgl_unlock();
    s_screen_ready = true;
    return true;
}

static void screen_message(const char *message) {
    if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(s_status, sizeof(s_status), "%s", message);
        xSemaphoreGive(s_status_lock);
    }
    if (s_screen_ready && bsp_lvgl_lock(200)) {
        lv_label_set_text(s_status_label, message);
        bsp_lvgl_unlock();
    }
}

static void screen_delete(void) {
    if (!s_screen_ready) return;
    if (bsp_lvgl_lock(500)) {
        lv_obj_delete(s_scr);
        bsp_lvgl_unlock();
    }
    s_screen_ready = false;
}

/* ---------- 内置网页 ---------- */

static const char html_start[] =
"<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>三合一设备配网</title><style>*{scrollbar-width:none}*::-webkit-scrollbar{display:none}html{font-family:system-ui;color:#151515;background:#f7f8fa}body{max-width:440px;margin:0 auto;padding:32px 22px}"
"h1{font-size:28px;margin:0 0 12px}p{line-height:1.7;color:#626874}label{display:block;margin:22px 0 8px}input{box-sizing:border-box;width:100%;padding:14px;border:1px solid #ced2da;border-radius:10px;font-size:16px;background:#fff}"
".net{display:block;width:100%;text-align:left;padding:12px;margin-top:8px;border:1px solid #ced2da;border-radius:10px;background:#fff;color:#151515;font-size:16px}"
"button{width:100%;padding:15px;margin-top:28px;border:0;border-radius:10px;background:#E05B4C;color:#fff;font-size:17px}button:disabled{opacity:.5}#status{min-height:48px}small{display:block;line-height:1.7;color:#626874}summary{cursor:pointer;color:#0667f6;margin-top:22px}</style>"
"<h1>连接你的三合一设备</h1><p>手机连着设备热点即可填写；提交后直接保存，设备会自动连接并进入应用。服务端设置可留空，之后在应用里补填。</p>"
"<form id=f><label>2.4 GHz 无线网络名称</label><input id=ssid required maxlength=32 autocomplete=off>"
"<button type=button id=scan style=margin-top:10px>扫描附近网络</button>"
"<div id=wifilist></div>"
"<label>无线网络密码</label><input id=password type=password maxlength=64 autocomplete=off><small>开放网络可以留空；其他网络密码至少 8 个字符。</small>"
"<details><summary>服务端设置（可选）</summary>"
"<label>服务端地址</label><input id=server type=url maxlength=127 placeholder='http://192.168.1.10:8899' autocomplete=off>"
"<label>六位配对码</label><input id=code inputmode=numeric pattern='[0-9]{6}' minlength=6 maxlength=6 autocomplete=off>"
"<small>在后台「设备」页生成配对码（10 分钟有效、一次使用）；地址和配对码需同时填写。</small></details>"
"<button id=submit>连接并保存</button></form><p id=status aria-live=polite></p>"
"<small>已有配置时，长按设备确定键可取消并重启返回。</small>"
"<script>let poll;const state=document.getElementById('status'),button=document.getElementById('submit');"
"async function loadScan(){const b=document.getElementById('scan');b.disabled=true;b.textContent='正在扫描…';try{const r=await fetch('/api/wifi',{cache:'no-store'}),j=await r.json();const box=document.getElementById('wifilist');box.innerHTML='';const nets=(j.networks||[]);const n=nets.length;nets.forEach(w=>{const o=document.createElement('button');o.type='button';o.className='net';o.textContent=w.ssid+'  ('+w.rssi+' dBm)';o.onclick=()=>{document.getElementById('ssid').value=w.ssid;};box.appendChild(o);});state.textContent=n?('扫到 '+n+' 个网络，点名称直接填入。'):'没扫到网络，请直接手填名称。';}catch(e){state.textContent='扫描请求失败: '+(e&&e.message?e.message:e);b.disabled=false;}b.textContent='重新扫描';b.disabled=false;}"
"document.getElementById('scan').onclick=loadScan;loadScan();"
"async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'}),j=await r.json();state.textContent=j.message;button.disabled=j.busy;if(j.saved){clearInterval(poll);state.textContent='保存成功，设备正在启动应用。手机可以切回原来的无线网络。';}}catch(e){state.textContent='连接暂时中断，手机可能正在重连设备热点；正在重试。';}}"
"document.getElementById('f').onsubmit=async(e)=>{e.preventDefault();button.disabled=true;state.textContent='正在提交连接信息…';"
"try{const r=await fetch('/api/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:document.getElementById('ssid').value,password:document.getElementById('password').value,server:document.getElementById('server').value,code:document.getElementById('code').value})});const j=await r.json();state.textContent=j.message;if(!r.ok)button.disabled=false;}catch(e){button.disabled=false;state.textContent='提交未确认，请连上设备热点后重试。';}};poll=setInterval(refresh,1500);refresh();</script></html>";

static esp_err_t json_reply(httpd_req_t *req, const char *message, const char *http_status) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "message", message);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return e;
}

static esp_err_t index_get(httpd_req_t *req) {
    activity_touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, html_start, sizeof(html_start) - 1);
}

static esp_err_t status_get(httpd_req_t *req) {
    activity_touch(); /* 页面每 1.5s 轮询本接口，视为持续活动 */
    char message[sizeof(s_status)] = "请填写连接信息";
    if (xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(message, s_status, sizeof(message));
        xSemaphoreGive(s_status_lock);
    }
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "message", message);
    cJSON_AddBoolToObject(j, "busy", atomic_load(&s_busy));
    cJSON_AddBoolToObject(j, "saved", !strcmp(message, SAVED_MSG));
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return e;
}

static esp_err_t wifi_get(httpd_req_t *req) {
    activity_touch();
    /* 仅提供开机 STA 全信道扫描缓存；APSTA 带客户端的实时扫描不可靠。 */
    const uint16_t count = s_scan_cache_count;
    ESP_LOGI(TAG, "serve scan cache: %u APs", (unsigned)count);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");
    const char *prev = "";
    bool first = true;
    for (uint16_t i = 0; i < count; i++) {
        if (!s_scan_cache[i].ssid[0] || !strncmp(prev, (const char *)s_scan_cache[i].ssid, sizeof(s_scan_cache[i].ssid))) continue;
        prev = (const char *)s_scan_cache[i].ssid;
        char safe[67] = "";
        size_t out = 0;
        for (size_t in = 0; in < sizeof(s_scan_cache[i].ssid) && out + 2 < sizeof(safe); in++) {
            unsigned char ch = s_scan_cache[i].ssid[in];
            if (!ch) break;
            if (ch == '"' || ch == '\\') safe[out++] = '\\';
            if (ch < 0x20) ch = '?';
            safe[out++] = (char)ch;
        }
        char item[96];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}", first ? "" : ",", safe, s_scan_cache[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *field(cJSON *j, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool valid_ssid(const char *s) {
    size_t len = strnlen(s, 33);
    return len >= 1 && len <= 32;
}

static bool valid_pass(const char *s) {
    size_t len = strnlen(s, 65);
    return len == 0 || len >= 8; /* 开放网络留空；WPA2 至少 8 字符 */
}

static bool valid_server(const char *s) {
    if (!s[0]) return true;
    size_t len = strnlen(s, TODO_CFG_SERVER_MAX);
    if (len < 8 || len >= TODO_CFG_SERVER_MAX) return false;
    return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8);
}

static bool valid_code(const char *s) {
    if (!s[0]) return true; /* 留空 = 不做服务端配对 */
    size_t len = strnlen(s, TODO_CFG_CODE_MAX);
    if (len != 6) return false;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static esp_err_t setup_post(httpd_req_t *req) {
    if (!atomic_load(&s_active)) return json_reply(req, "设置服务已关闭，请重启", "503 Service Unavailable");
    if (req->content_len <= 0 || req->content_len > 768) return json_reply(req, "填写内容过长", "413 Payload Too Large");
    char body[769];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return json_reply(req, "提交中断，请重试", "400 Bad Request");
        received += (size_t)n;
    }
    body[received] = 0;
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (!j) return json_reply(req, "填写内容无效", "400 Bad Request");
    const char *ssid = field(j, "ssid"), *pass = field(j, "password");
    const char *server = field(j, "server"), *code = field(j, "code");
    setup_input_t input = {0};
    const bool valid = ssid && pass && server && code && valid_ssid(ssid) && valid_pass(pass) &&
                       valid_server(server) && valid_code(code) && (!code[0] || server[0]);
    if (valid) {
        strlcpy(input.ssid, ssid, sizeof(input.ssid));
        strlcpy(input.pass, pass, sizeof(input.pass));
        strlcpy(input.server, server, sizeof(input.server));
        strlcpy(input.code, code, sizeof(input.code));
    }
    cJSON_Delete(j);
    if (!valid) return json_reply(req, "请检查网络名称/密码；配对码为六位数字且需与服务端地址同时填写", "400 Bad Request");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        return json_reply(req, "正在连接，请稍候", "409 Conflict");
    }
    if (xQueueSend(s_submissions, &input, 0) != pdTRUE) {
        atomic_store(&s_busy, false);
        memset(&input, 0, sizeof(input));
        return json_reply(req, "稍后再试", "503 Service Unavailable");
    }
    memset(&input, 0, sizeof(input));
    return json_reply(req, "正在保存配置", "202 Accepted");
}

/* ---------- 射频与 HTTP 服务 ---------- */

static bool start_network(void) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return false;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    s_wifi_inited = true;
    return esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK;
}

static volatile bool s_scan_done_flag;

static void on_scan_done(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_scan_done_flag = true;
}

/* demo_wifi 同款 STA 全信道扫描：非阻塞发起 + SCAN_DONE 事件收结果。
 * 带客户端的 APSTA 扫描会被限制在 AP 信道且可能重置手机连接，故先扫后起热点。 */
static bool sta_scan_cache_full(void) {
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) return false;
    s_wifi_started = true;

    esp_event_handler_instance_t scan_handler = NULL;
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              on_scan_done, NULL, &scan_handler);
    if (err != ESP_OK) return false;

    s_scan_done_flag = false;
    err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_OK) {
        const int64_t deadline = esp_timer_get_time() + 8000000;
        while (!s_scan_done_flag && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_handler);

    uint16_t count = SCAN_CACHE_MAX;
    if (err != ESP_OK || esp_wifi_scan_get_ap_records(&count, s_scan_cache) != ESP_OK) {
        count = 0;
    }
    s_scan_cache_count = count;
    ESP_LOGI(TAG, "sta scan: %u APs", (unsigned)count);

    /* 切 APSTA 起热点 */
    err = esp_wifi_stop();
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        wifi_config_t ap = {0};
        memcpy(ap.ap.ssid, s_hotspot, strlen(s_hotspot));
        ap.ap.ssid_len = strlen(s_hotspot);
        memcpy(ap.ap.password, s_hotspot_pass, sizeof(s_hotspot_pass));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        ap.ap.pmf_cfg.required = false;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err == ESP_OK) err = esp_wifi_start();
    }
    s_wifi_started = err == ESP_OK;
    return s_wifi_started;
}

static bool start_web(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 3;
    cfg.backlog_conn = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 3;
    cfg.send_wait_timeout = 3;
    if (httpd_start(&s_web, &cfg) != ESP_OK) return false;
    const httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    const httpd_uri_t setup = {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_post};
    const httpd_uri_t state = {.uri = "/api/status", .method = HTTP_GET, .handler = status_get};
    const httpd_uri_t networks = {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get};
    return httpd_register_uri_handler(s_web, &index) == ESP_OK &&
           httpd_register_uri_handler(s_web, &setup) == ESP_OK &&
           httpd_register_uri_handler(s_web, &state) == ESP_OK &&
           httpd_register_uri_handler(s_web, &networks) == ESP_OK;
}

static bool cleanup(void) {
    atomic_store(&s_active, false);
    if (s_web) {
        if (httpd_stop(s_web) != ESP_OK) return false;
        s_web = NULL;
    }
    if (s_wifi_started) {
        if (esp_wifi_stop() != ESP_OK) return false;
        s_wifi_started = false;
    }
    if (s_wifi_inited) {
        if (esp_wifi_deinit() != ESP_OK) return false;
        s_wifi_inited = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_submissions) {
        vQueueDelete(s_submissions);
        s_submissions = NULL;
    }
    if (s_status_lock) {
        vSemaphoreDelete(s_status_lock);
        s_status_lock = NULL;
    }
    memset(s_hotspot_pass, 0, sizeof(s_hotspot_pass));
    screen_delete();
    return true;
}

/* ---------- 生命周期 ---------- */

/* 网页任一请求都刷新活动时间：页面开着就不会触发空闲超时。 */
static void activity_touch(void) {
    if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_last_activity = esp_timer_get_time();
        xSemaphoreGive(s_status_lock);
    }
}

static bool activity_expired(void) {
    int64_t last = 0;
    if (s_status_lock && xSemaphoreTake(s_status_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        last = s_last_activity;
        xSemaphoreGive(s_status_lock);
    }
    return esp_timer_get_time() - last > SETUP_IDLE_TIMEOUT_US;
}

bool trio_setup_active(void) { return atomic_load(&s_active); }

void trio_setup_key(bsp_btn_t button, bsp_btn_ev_t event) {
    if (atomic_load(&s_active) && button == BSP_BTN_OK && event == BSP_BTN_LONG) {
        atomic_store(&s_cancelled, true);
    }
}

void trio_setup_request(void) {
    esp_err_t err;
    nvs_handle_t handle;
    err = nvs_open_from_partition(NETCFG_PARTITION, WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "置配网请求失败: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(handle, SETUP_FLAG_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "已请求重启进入配网%s", err == ESP_OK ? "" : "（写标志失败）");
}

bool trio_setup_take_request(void) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(NETCFG_PARTITION, WIFI_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    uint8_t requested = 0;
    esp_err_t err = nvs_get_u8(handle, SETUP_FLAG_KEY, &requested);
    if (err == ESP_OK && requested) {
        err = nvs_erase_key(handle, SETUP_FLAG_KEY);
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK && requested;
}

bool trio_setup_run(void) {
    char ssid[33];
    char pass[65];
    const bool has_existing = wifi_link_credentials_load(ssid, sizeof(ssid), pass, sizeof(pass));
    s_can_cancel = has_existing;
    atomic_store(&s_active, true);
    atomic_store(&s_cancelled, false);
    atomic_store(&s_busy, false);

    /* 热点名/密码按 STA MAC 派生且固定：重启或反复进配网都不再漂移，
     * 手机可以记住并自动重连（此前每次开机随机生成，导致 SSID 一直换）。 */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hotspot, sizeof(s_hotspot), "Trio-%02X%02X", mac[4], mac[5]);
    snprintf(s_hotspot_pass, sizeof(s_hotspot_pass), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    s_submissions = xQueueCreate(1, sizeof(setup_input_t));
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_submissions || !s_status_lock || !screen_create()) {
        cleanup();
        return false;
    }
    screen_message(s_can_cancel ? "Reconfigure: fill the form" : "First setup: fill the form");
    if (!start_network()) {
        screen_message("Setup service failed, reboot");
        cleanup();
        return false;
    }
    /* 先 STA 全信道扫描缓存（demo_wifi 同款），再切 APSTA 起热点。 */
    if (!sta_scan_cache_full()) {
        screen_message("Setup service failed, reboot");
        cleanup();
        return false;
    }
    if (!start_web()) {
        screen_message("Setup service failed, reboot");
        cleanup();
        return false;
    }

    ESP_LOGI(TAG, "配网热点 %s 已开启", s_hotspot);
    setup_input_t input = {0};
    s_last_activity = esp_timer_get_time(); /* 从本服务就绪起算 */
    for (;;) {
        if (atomic_load(&s_cancelled)) {
            if (s_can_cancel) {
                /* 保留原配置，重启回正常流程。 */
                cleanup();
                memset(&input, 0, sizeof(input));
                esp_restart();
                return true;
            }
            atomic_store(&s_cancelled, false);
            screen_message("First setup cannot be skipped");
        }
        if (xQueueReceive(s_submissions, &input, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (activity_expired()) {
                screen_message("Setup timed out, starting app");
                vTaskDelay(pdMS_TO_TICKS(1500));
                cleanup();
                return true; /* 继续启动;应用内长按上键可再次进入配网 */
            }
            continue;
        }
        activity_touch();

        /* 不做试连验证：直接保存，由正常启动流程连 Wi-Fi（后台重试，不阻塞应用）。 */
        screen_message("Saving...");
        const char *error = NULL;
        if (wifi_link_credentials_save(input.ssid, input.pass) != ESP_OK) {
            error = "Save failed, try again";
        } else if (input.server[0] && !todo_cfg_save_pairing(input.server, input.code)) {
            error = "Server info save failed";
        }
        if (!error) {
            screen_message(SAVED_MSG);
            memset(&input, 0, sizeof(input));
            vTaskDelay(pdMS_TO_TICKS(1800));
            cleanup(); /* main 随后 wifi_link_autostart 连接并进入应用 */
            return true;
        }
        screen_message(error);
        memset(&input, 0, sizeof(input));
        atomic_store(&s_busy, false);
    }
}
