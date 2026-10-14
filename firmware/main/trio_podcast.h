// main/trio_podcast.h —— 播客应用层：目录浏览 + 播放控制 + 收听上报。
// 复用三合一的联网与凭据设施（wifi_link / todo_cfg / todo_sync 的 claim 令牌），
// 经 podcast_config_shim 桥接给 podcast_player / podcast_http；不建第二个配置存储。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "podcast_player.h"

#define TRIO_PODCAST_TITLE_MAX 96  /* 含 NUL；UTF-8 中文约 30 字 */
#define TRIO_PODCAST_MAX_SHOWS 16
#define TRIO_PODCAST_MAX_EPISODES 16
#define TRIO_PODCAST_PAGE_SIZE 8   /* 单次请求页大小（与 server 契约一致） */

typedef struct {
    char text[TRIO_PODCAST_TITLE_MAX];
} trio_podcast_title_t;

typedef struct {
    bool loading;   /* 该列表正在请求 */
    bool failed;    /* 最近一次请求失败（未配对/网络/服务端），可重试 */
    bool has_more;  /* 服务端还有更多条目未拉取 */
    uint8_t count;  /* 当前缓存条数 */
} trio_podcast_list_info_t;

/* main.c 在 PODCAST 模式 enter 前调用一次；未配置 server 时空转。
 * 幂等：worker 已存活时直接返回。退出=整机重启，无需 shutdown。 */
void trio_podcast_init(void);

/* 目录读取（UI/LVGL 上下文；内部短临界区拷贝，不阻塞）。 */
bool trio_podcast_shows(trio_podcast_title_t *out, uint8_t cap, trio_podcast_list_info_t *info);
bool trio_podcast_episodes(trio_podcast_title_t *out, uint8_t cap, trio_podcast_list_info_t *info);
bool trio_podcast_player_snapshot(podcast_player_snapshot_t *out);
/* 播放视图大标题：单集标题，缺省回退节目名；未在播放返回 false。 */
bool trio_podcast_current_title(char *out, size_t cap);
/* 正在缓冲/播放（rtttl 提示音抑制判断）。 */
bool trio_podcast_playing(void);

/* 命令（非阻塞入队；false=未就绪或队列满）。 */
bool trio_podcast_fetch_shows(void);
bool trio_podcast_open_show(uint8_t index);            /* 拉取该节目剧集第一页 */
bool trio_podcast_play_episode(uint8_t episode_index); /* 当前节目下的单集起播 */
bool trio_podcast_pause_resume(void);
bool trio_podcast_pause(void);                         /* 显式暂停（开始任务时自动调用） */
bool trio_podcast_seek(int seconds);                   /* 相对快进/快退 */
bool trio_podcast_volume(int delta);                   /* 相对音量 ±（worker 内钳位 0..100） */
bool trio_podcast_stop(void);
