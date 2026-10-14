// main/task_store.c —— 任务快照持久化：store 分区 NVS + 后台保存任务。
// request-save 模式（仿官方 pomodoro demo）：队列元素为序列化后的快照本体，
// 后台任务串行消费写入，避免 NVS 写入阻塞 UI 路径，也避免共享缓冲竞态。
#include "task_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "task_store_format.h"

#define STORE_PARTITION "store"
#define STORE_NAMESPACE "task"
#define STORE_KEY "snapshot"
#define SAVE_TASK_STACK 3072u

static const char *TAG = "task_store";

typedef struct {
    uint8_t data[TASK_STORE_SNAPSHOT_MAX];
    size_t len;
} store_msg_t;

static QueueHandle_t s_queue;
static nvs_handle_t s_nvs;
static bool s_ready;
static volatile bool s_error;

/* 后台任务：串行消费队列中的快照并写 NVS。 */
static void save_task(void *arg) {
    (void)arg;
    store_msg_t msg;
    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = nvs_set_blob(s_nvs, STORE_KEY, msg.data, msg.len);
        if (err == ESP_OK) err = nvs_commit(s_nvs);
        if (err != ESP_OK) {
            s_error = true;
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        }
    }
}

bool task_store_init(task_list_t *list) {
    if (!list) return false;
    if (s_ready) return true;

    esp_err_t err = nvs_flash_init_partition(STORE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "store NVS needs recovery: %s", esp_err_to_name(err));
        err = nvs_flash_erase_partition(STORE_PARTITION);
        if (err == ESP_OK) err = nvs_flash_init_partition(STORE_PARTITION);
    }
    if (err != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "store NVS init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        s_error = true;
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t buf[TASK_STORE_SNAPSHOT_MAX];
    size_t len = sizeof(buf);
    err = nvs_get_blob(s_nvs, STORE_KEY, buf, &len);
    if (err == ESP_OK && task_store_format_deserialize(list, buf, len)) {
        task_list_restore(list); /* 运行中任务降级为暂停 */
        ESP_LOGI(TAG, "Restored %u tasks from snapshot", (unsigned)list->count);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        task_list_defaults(list);
    } else {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Stored snapshot invalid (err=%s); using defaults",
                     esp_err_to_name(err));
        }
        task_list_defaults(list);
    }

    s_queue = xQueueCreate(1, sizeof(store_msg_t));
    if (s_queue == NULL ||
        xTaskCreate(save_task, "task_save", SAVE_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_error = true;
        ESP_LOGE(TAG, "Failed to create persistence worker");
        return false;
    }
    s_ready = true;
    return true;
}

void task_store_request_save(const task_list_t *list) {
    if (!s_ready || !list) return;
    store_msg_t msg;
    msg.len = task_store_format_serialize(list, msg.data, sizeof(msg.data));
    if (msg.len == 0) {
        s_error = true;
        ESP_LOGE(TAG, "Snapshot serialize failed");
        return;
    }
    if (xQueueOverwrite(s_queue, &msg) != pdTRUE) s_error = true;
}

bool task_store_has_error(void) {
    return s_error;
}
