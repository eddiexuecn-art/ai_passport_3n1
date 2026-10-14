// main/podcast_config_shim.c —— podcast 模块与三合一共享凭据的桥接层。
// podcast_http.h 与 trio_podcast 通过 podcast_config_get()/authorization() 读到的
// 服务端地址与 Bearer 令牌，实际存储在 todo_cfg（netcfg "todo" namespace），
// 与 todo_sync 共用同一次配对 claim，不建第二个配置存储。
#include "podcast_config.h"

#include <stdio.h>
#include <string.h>

#include "todo_cfg.h"

static podcast_config_t s_cached;
static char s_bearer[TODO_CFG_TOKEN_MAX + 8];

/* 每次调用重读 NVS：token 可能由 todo_sync 的 claim 在运行期才写入。
 * 各任务读到的内容恒等（同一 token），并发写同值无撕裂风险。 */
static bool refresh(void) {
    char token[TODO_CFG_TOKEN_MAX];
    char code[TODO_CFG_CODE_MAX];
    if (!todo_cfg_load(s_cached.server, sizeof(s_cached.server), token, sizeof(token),
                       code, sizeof(code)) ||
        s_cached.server[0] == '\0' || token[0] == '\0') {
        return false;
    }
    strlcpy(s_cached.token, token, sizeof(s_cached.token));
    snprintf(s_bearer, sizeof(s_bearer), "Bearer %s", token);
    return true;
}

const podcast_config_t *podcast_config_get(void) {
    return refresh() ? &s_cached : NULL;
}

const char *podcast_config_authorization(void) {
    return refresh() ? s_bearer : NULL;
}
