// main/todo_cfg.h —— 服务端配置与设备身份（netcfg 分区 NVS，namespace "todo"）。
// 配对流程复用原版：配网页录入服务端地址 + 后台“设备”页生成的六位配对码，
// 设备上线后用配对码 claim 换取令牌（10 分钟有效、一次使用）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TODO_CFG_SERVER_MAX 128u     /* 含 NUL */
#define TODO_CFG_TOKEN_MAX 65u       /* 含 NUL */
#define TODO_CFG_CODE_MAX 8u         /* 六位配对码 + 余量 */
#define TODO_CFG_DEVICE_ID_MAX 24u   /* "device-" + 16 hex + NUL */

/* 配网页写入：服务端地址 + 配对码（同时清除旧令牌，等待设备 claim 换新）。 */
bool todo_cfg_save_pairing(const char *server, const char *code);

/* claim 成功后写入令牌并清除配对码。 */
bool todo_cfg_save_token(const char *token);

/* 读取全部；缺省键填空串。分区未初始化等错误返回 false。 */
bool todo_cfg_load(char *server, size_t server_cap, char *token, size_t token_cap,
                   char *code, size_t code_cap);

/* claim 被拒（401）后清除配对码，避免重启后拿失效码反复请求。 */
void todo_cfg_clear_code(void);

/* 设备标识（首次调用生成 "device-XXXXXXXXXXXXXXXX" 并持久化）。 */
bool todo_cfg_device_id(char *out, size_t cap);

/* 是否已配置服务端地址。 */
bool todo_cfg_has_server(void);
