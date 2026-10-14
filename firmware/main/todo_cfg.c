#include "todo_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#define NETCFG_PARTITION "netcfg"
#define CFG_NAMESPACE "todo"
#define KEY_SERVER "server"
#define KEY_TOKEN "token"
#define KEY_CODE "code"
#define KEY_DEVICE_ID "device_id"

static const char *TAG = "todo_cfg";

static nvs_handle_t cfg_open(esp_err_t *err_out, nvs_open_mode_t mode) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(NETCFG_PARTITION, CFG_NAMESPACE, mode, &handle);
    if (err_out != NULL) *err_out = err;
    return err == ESP_OK ? handle : 0;
}

/* 写/擦一个字符串键；空串表示擦除。返回最终 esp_err_t。 */
static esp_err_t cfg_put_str(nvs_handle_t handle, const char *key, const char *value) {
    if (value && value[0] != '\0') return nvs_set_str(handle, key, value);
    esp_err_t err = nvs_erase_key(handle, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static bool cfg_commit(nvs_handle_t handle, esp_err_t err) {
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool cfg_read_str(nvs_handle_t handle, const char *key, char *out, size_t cap) {
    size_t len = cap;
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true; /* 键不存在视为空配置 */
    if (err != ESP_OK) return false;
    return strnlen(out, cap) < cap; /* 未以 NUL 结尾视为损坏 */
}

bool todo_cfg_save_pairing(const char *server, const char *code) {
    if (!server || server[0] == '\0') return true;
    /* 归一化：去掉结尾的 '/'，避免拼接路径变成 //api/...（308/404） */
    char clean[TODO_CFG_SERVER_MAX];
    strlcpy(clean, server, sizeof(clean));
    size_t len = strlen(clean);
    while (len > 0 && clean[len - 1] == '/') clean[--len] = '\0';
    if (clean[0] == '\0') return true;

    esp_err_t err;
    nvs_handle_t handle = cfg_open(&err, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 netcfg/todo 失败: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(handle, KEY_SERVER, clean);
    if (err == ESP_OK) err = cfg_put_str(handle, KEY_CODE, code);
    if (err == ESP_OK) err = cfg_put_str(handle, KEY_TOKEN, ""); /* 换新配对码时旧令牌作废 */
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "配对配置已保存");
    return true;
}

bool todo_cfg_save_token(const char *token) {
    if (!token || token[0] == '\0') return true;
    esp_err_t err;
    nvs_handle_t handle = cfg_open(&err, NVS_READWRITE);
    if (err != ESP_OK) return false;
    err = nvs_set_str(handle, KEY_TOKEN, token);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_CODE); /* 一次性配对码用后即弃 */
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "令牌保存失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "设备令牌已保存");
    return true;
}

bool todo_cfg_load(char *server, size_t server_cap, char *token, size_t token_cap,
                   char *code, size_t code_cap) {
    esp_err_t err;
    nvs_handle_t handle = cfg_open(&err, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        if (server && server_cap) server[0] = '\0';
        if (token && token_cap) token[0] = '\0';
        if (code && code_cap) code[0] = '\0';
        return true; /* 尚未配置过，视为空配置 */
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 netcfg/todo 失败: %s", esp_err_to_name(err));
        return false;
    }
    bool ok = (!server || cfg_read_str(handle, KEY_SERVER, server, server_cap)) &&
              (!token || cfg_read_str(handle, KEY_TOKEN, token, token_cap)) &&
              (!code || cfg_read_str(handle, KEY_CODE, code, code_cap));
    nvs_close(handle);
    return ok;
}

void todo_cfg_clear_code(void) {
    esp_err_t err;
    nvs_handle_t handle = cfg_open(&err, NVS_READWRITE);
    if (err != ESP_OK) return;
    err = nvs_erase_key(handle, KEY_CODE);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "配对码已清除");
}

bool todo_cfg_device_id(char *out, size_t cap) {
    if (!out || cap < TODO_CFG_DEVICE_ID_MAX) return false;
    esp_err_t err;
    nvs_handle_t handle = cfg_open(&err, NVS_READWRITE);
    if (err != ESP_OK) return false;
    size_t len = cap;
    if (nvs_get_str(handle, KEY_DEVICE_ID, out, &len) == ESP_OK &&
        strnlen(out, cap) == TODO_CFG_DEVICE_ID_MAX - 1) {
        nvs_close(handle);
        return true; /* 已存在 */
    }
    uint64_t seed = ((uint64_t)esp_random() << 32) | esp_random();
    snprintf(out, cap, "device-%016llx", (unsigned long long)seed);
    bool ok = nvs_set_str(handle, KEY_DEVICE_ID, out) == ESP_OK &&
              nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok) out[0] = '\0';
    return ok;
}

bool todo_cfg_has_server(void) {
    char server[TODO_CFG_SERVER_MAX];
    char token[TODO_CFG_TOKEN_MAX];
    char code[TODO_CFG_CODE_MAX];
    return todo_cfg_load(server, sizeof(server), token, sizeof(token),
                         code, sizeof(code)) && server[0] != '\0';
}
