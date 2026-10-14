#include "todo_sync.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "clock_sync.h"
#include "clock_time.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "todo_cfg.h"
#include "trio_app.h"
#include "task_store.h"
#include "wifi_link.h"

#define NETCFG_PARTITION "netcfg"
#define CFG_NAMESPACE "todo"
#define UPL_DATE_KEY "ul_date"
#define UPL_COUNT_KEY "ul_cnt"
#define REC_DIR "/fs/rec"
#define HTTP_TIMEOUT_MS 8000
#define RESP_CAP 1536
#define POST_BATCH 16
#define PULL_RETRY_MS (60 * 1000)

static const char *TAG = "todo_sync";

static char s_server[TODO_CFG_SERVER_MAX];
static char s_token[TODO_CFG_TOKEN_MAX];
static char s_code[TODO_CFG_CODE_MAX];
static char s_device_id[TODO_CFG_DEVICE_ID_MAX];
static char s_bearer[TODO_CFG_TOKEN_MAX + 8];
static bool s_has_cfg;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_stage_lock;
static volatile bool s_boot_triggered;      /* 网络就绪后只自动触发一次拉取 */
static volatile bool s_pull_done;           /* 本次启动已完成一次任务模板拉取 */
static volatile bool s_pull_failed;         /* 上次拉取失败，允许 60s 重试 */
static int64_t s_pull_fail_at;
static char s_stage[TODO_SYNC_TASK_MAX][TASK_NAME_MAX];
static uint8_t s_stage_count;
static volatile bool s_stage_ready;

/* ---------- HTTP ---------- */

static bool http_call(const char *path, bool post, const char *body,
                      char *resp, size_t resp_cap, int *status_out) {
    char url[TODO_CFG_SERVER_MAX + 40];
    if (snprintf(url, sizeof(url), "%s%s", s_server, path) >= (int)sizeof(url)) return false;

    esp_http_client_config_t config = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return false;
    /* 令牌为空（claim 前）不得携带 Authorization：空令牌会被服务器直接 401 拒绝 */
    if (s_token[0] != '\0') esp_http_client_set_header(client, "Authorization", s_bearer);
    if (post) esp_http_client_set_header(client, "Content-Type", "application/json");

    bool ok = false;
    int status = 0;
    size_t received = 0;
    if (esp_http_client_open(client, post ? (int)strlen(body) : 0) == ESP_OK) {
        if (post && body) {
            esp_http_client_write(client, body, (int)strlen(body));
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        while (received + 1 < resp_cap) {
            int n = esp_http_client_read(client, resp + received, (int)(resp_cap - 1 - received));
            if (n <= 0) break;
            received += (size_t)n;
        }
        resp[received] = '\0';
        ok = status >= 200 && status < 300;
    }
    *status_out = status;
    esp_http_client_cleanup(client);
    if (!ok) ESP_LOGW(TAG, "%s %s -> %d", post ? "POST" : "GET", path, status);
    return ok;
}

/* ---------- 今日记录增量上传 ---------- */

static bool load_upload_mark(char date[9], uint32_t *count) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(NETCFG_PARTITION, CFG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        date[0] = '\0';
        *count = 0;
        return true; /* 尚无标记视为从零开始 */
    }
    size_t len = 9;
    bool ok = true;
    if (nvs_get_str(handle, UPL_DATE_KEY, date, &len) != ESP_OK) date[0] = '\0';
    uint32_t saved = 0;
    if (nvs_get_u32(handle, UPL_COUNT_KEY, &saved) != ESP_OK) saved = 0;
    nvs_close(handle);
    if (strlen(date) != 8) {
        date[0] = '\0';
        saved = 0;
    }
    *count = saved;
    return ok;
}

static bool save_upload_mark(const char *date, uint32_t count) {
    nvs_handle_t handle;
    if (nvs_open_from_partition(NETCFG_PARTITION, CFG_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(handle, UPL_DATE_KEY, date) == ESP_OK &&
              nvs_set_u32(handle, UPL_COUNT_KEY, count) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

/* 从今日记录文件读取 [uploaded, total) 区间的行，返回 0 表示没有可传内容；-1 表示文件不可读 */
static int collect_pending(const char *date, uint32_t uploaded, char lines[][256], int max_lines) {
    char path[40];
    if (snprintf(path, sizeof(path), REC_DIR "/%s.jsonl", date) >= (int)sizeof(path)) return -1;
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1; /* 当日还没有记录文件 */
    char raw[256];
    uint32_t index = 0;
    int collected = 0;
    while (fgets(raw, sizeof(raw), f) != NULL) {
        size_t len = strlen(raw);
        while (len > 0 && (raw[len - 1] == '\n' || raw[len - 1] == '\r')) raw[--len] = '\0';
        if (len == 0) continue;
        if (index >= uploaded && collected < max_lines) {
            snprintf(lines[collected], 256, "%s", raw);
            collected++;
        }
        index++;
    }
    fclose(f);
    (void)index;
    return collected;
}

/* 上传今日记录文件中尚未上传的行；返回是否处理成功（无待传也算成功）。 */
static bool push_pending_records(void) {
    char date[9];
    if (!clock_time_now_date(date)) return true; /* 未校时：当日文件尚不存在 */
    char mark_date[9];
    uint32_t uploaded;
    if (!load_upload_mark(mark_date, &uploaded)) return false;
    if (strcmp(mark_date, date) != 0) uploaded = 0;

    /* worker 栈有限：批缓冲必须静态（本任务单线程独占） */
    static char lines[POST_BATCH][256];
    int collected = collect_pending(date, uploaded, lines, POST_BATCH);
    if (collected < 0) return true; /* 今日尚无记录文件 */
    if (collected == 0) return true;

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "records");
    for (int i = 0; i < collected; ++i) {
        cJSON_AddItemToArray(array, cJSON_CreateString(lines[i]));
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return false;

    char resp[RESP_CAP];
    int status = 0;
    bool ok = http_call("/api/todo/records", true, body, resp, sizeof(resp), &status);
    cJSON_free(body);
    if (!ok) return false;

    uploaded += (uint32_t)collected;
    if (!save_upload_mark(date, uploaded)) return false;
    ESP_LOGI(TAG, "已上传 %d 条记录（累计 %lu）", collected, (unsigned long)uploaded);
    return true;
}

/* ---------- 任务模板拉取与应用 ---------- */

static bool stage_server_tasks(void) {
    char resp[RESP_CAP];
    int status = 0;
    if (!http_call("/api/todo/tasks", false, NULL, resp, sizeof(resp), &status)) return false;

    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        ESP_LOGW(TAG, "任务模板响应解析失败");
        return false;
    }
    bool ok = false;
    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (cJSON_IsArray(tasks)) {
        uint8_t count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, tasks) {
            if (count >= TODO_SYNC_TASK_MAX) break;
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!cJSON_IsString(name) || name->valuestring[0] == '\0') continue;
            xSemaphoreTake(s_stage_lock, portMAX_DELAY);
            strlcpy(s_stage[count], name->valuestring, TASK_NAME_MAX);
            count++;
            xSemaphoreGive(s_stage_lock);
        }
        xSemaphoreTake(s_stage_lock, portMAX_DELAY);
        s_stage_count = count;
        s_stage_ready = true;
        xSemaphoreGive(s_stage_lock);
        ok = true;
        ESP_LOGI(TAG, "已拉取 %u 个服务端任务", (unsigned)count);
    }
    cJSON_Delete(root);
    return ok;
}

/* 按“名字相同保留本地进度”合并服务端任务；顺序以服务端为准。LVGL 任务上下文调用。 */
static void apply_server_tasks(void) {
    task_list_t *list = trio_app_tasks();
    char names[TODO_SYNC_TASK_MAX][TASK_NAME_MAX];
    uint8_t count = 0;
    xSemaphoreTake(s_stage_lock, portMAX_DELAY);
    count = s_stage_count;
    for (uint8_t i = 0; i < count; ++i) {
        strlcpy(names[i], s_stage[i], TASK_NAME_MAX);
    }
    s_stage_ready = false;
    xSemaphoreGive(s_stage_lock);
    if (count == 0) return; /* 服务端未配置任务：保持设备现状 */

    /* 无变化则跳过（名字与顺序完全一致） */
    if (list->count == count) {
        bool same = true;
        for (uint8_t i = 0; i < count && same; ++i) {
            if (strcmp(list->tasks[i].name, names[i]) != 0) same = false;
        }
        if (same) return;
    }

    task_t merged[TASK_LIST_MAX];
    uint8_t merged_count = 0;
    for (uint8_t i = 0; i < count && merged_count < TASK_LIST_MAX; ++i) {
        task_t *existing = NULL;
        for (uint8_t j = 0; j < list->count; ++j) {
            if (strcmp(list->tasks[j].name, names[i]) == 0) {
                existing = &list->tasks[j];
                break;
            }
        }
        if (existing) {
            merged[merged_count++] = *existing; /* 同名任务保留全部进度 */
        } else {
            memset(&merged[merged_count], 0, sizeof(task_t));
            strlcpy(merged[merged_count].name, names[i], TASK_NAME_MAX);
            merged[merged_count].state = TASK_STATE_IDLE;
            merged[merged_count].remaining_sec = TASK_FOCUS_SEC;
            merged[merged_count].started_at = TASK_TIME_UNKNOWN;
            merged[merged_count].ended_at = TASK_TIME_UNKNOWN;
            merged_count++;
        }
    }

    memset(list->tasks, 0, sizeof(list->tasks));
    memcpy(list->tasks, merged, sizeof(task_t) * merged_count);
    list->count = merged_count;
    list->selected = 0;
    task_store_request_save(list);
    ESP_LOGI(TAG, "已应用服务端任务列表：%u 个", (unsigned)merged_count);
}

/* ---------- worker 与对外接口 ---------- */

/* 用六位配对码换取设备令牌（/api/devices/claim，一次使用）；成功后持久化令牌。 */
static bool ensure_token(void) {
    if (s_token[0] != '\0') return true;
    if (s_code[0] == '\0') {
        ESP_LOGW(TAG, "尚未配对：请在配网页填写服务端地址和配对码");
        return false;
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"device_id\":\"%s\"}", s_code, s_device_id);
    char resp[RESP_CAP];
    int status = 0;
    if (!http_call("/api/devices/claim", true, body, resp, sizeof(resp), &status)) return false;
    if (status == 401) {
        ESP_LOGW(TAG, "配对码无效或已过期，请重新进配网生成新配对码");
        s_code[0] = '\0';
        todo_cfg_clear_code(); /* 同步清除 NVS，避免重启后拿旧码反复 401 */
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) return false;
    cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    bool ok = cJSON_IsString(token) && token->valuestring[0] != '\0';
    if (ok) {
        strlcpy(s_token, token->valuestring, sizeof(s_token));
        snprintf(s_bearer, sizeof(s_bearer), "Bearer %s", s_token);
        s_code[0] = '\0'; /* 一次性配对码用后即弃 */
        ok = todo_cfg_save_token(s_token);
        ESP_LOGI(TAG, "配对成功，已换取设备令牌");
    }
    cJSON_Delete(root);
    return ok;
}

static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_wake, pdMS_TO_TICKS(PULL_RETRY_MS)) != pdTRUE) {
            /* 周期醒来重试失败过的拉取/上传 */
        }
        if (!s_has_cfg || !wifi_link_has_ip() || !clock_sync_synced()) continue;
        if (!ensure_token()) continue;

        if (!s_pull_done && (!s_pull_failed || esp_timer_get_time() - s_pull_fail_at > PULL_RETRY_MS * 1000LL)) {
            if (stage_server_tasks()) {
                s_pull_done = true;
                s_pull_failed = false;
            } else {
                s_pull_failed = true;
                s_pull_fail_at = esp_timer_get_time();
            }
        }
        (void)push_pending_records();
    }
}

void todo_sync_init(void) {
    char token[TODO_CFG_TOKEN_MAX];
    char code[TODO_CFG_CODE_MAX];
    if (!todo_cfg_load(s_server, sizeof(s_server), token, sizeof(token), code, sizeof(code)) ||
        s_server[0] == '\0') {
        ESP_LOGI(TAG, "未配置服务端，任务同步停用");
        return;
    }
    strlcpy(s_token, token, sizeof(s_token));
    strlcpy(s_code, code, sizeof(code));
    /* 历史配网可能存入尾斜杠（http://host:8899/），拼接后变成 //api/... */
    size_t len = strlen(s_server);
    while (len > 0 && s_server[len - 1] == '/') s_server[--len] = '\0';
    if (!todo_cfg_device_id(s_device_id, sizeof(s_device_id))) {
        ESP_LOGE(TAG, "设备标识生成失败，任务同步停用");
        return;
    }
    snprintf(s_bearer, sizeof(s_bearer), "Bearer %s", s_token);
    s_has_cfg = true;

    s_wake = xSemaphoreCreateBinary();
    s_stage_lock = xSemaphoreCreateMutex();
    if (s_wake == NULL || s_stage_lock == NULL ||
        xTaskCreate(worker_task, "todo_sync", 6144, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "同步 worker 启动失败");
        return;
    }
    if (s_token[0] != '\0') {
        ESP_LOGI(TAG, "任务同步已启用 → %s", s_server);
    } else {
        ESP_LOGI(TAG, "任务同步待配对 → %s（配对码 %s）",
                 s_server, s_code[0] != '\0' ? s_code : "未填");
    }
}

void todo_sync_note_record(void) {
    if (s_wake != NULL) xSemaphoreGive(s_wake);
}

void todo_sync_kick(void) {
    if (s_wake != NULL) xSemaphoreGive(s_wake);
}

void todo_sync_tick(void) {
    if (!s_has_cfg) return;
    if (!s_boot_triggered && wifi_link_has_ip() && clock_sync_synced()) {
        s_boot_triggered = true;
        xSemaphoreGive(s_wake);
    }
    if (s_stage_ready) apply_server_tasks();
}
