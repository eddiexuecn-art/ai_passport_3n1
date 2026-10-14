// main/trio_podcast.c —— 播客应用层 worker：目录请求、起播流程、收听上报。
// 复用面：wifi_link（联网状态）、todo_cfg（server 地址，经 podcast_config_shim 取
// token）、podcast_player（流式播放 worker）、podcast_sync（收听上报 outbox）。
// 精简 v1：不做多代续播对账（demo 的 resume 状态机），续播位置以服务端
// /api/episodes 的中心进度为准；离线会话由 outbox 持久化、联网后补传。
#include "trio_podcast.h"

#include <string.h>

#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "podcast_catalogue_limits.h"
#include "podcast_http.h"
#include "podcast_player.h"
#include "podcast_sync.h"
#include "rtttl_player.h"
#include "todo_cfg.h"
#include "todo_sync.h"
#include "wifi_link.h"

#define TAG "trio_podcast"
#define LISTEN_TIMEOUT_MS 800  /* 收听上报走快超时，保 worker 响应 */
#define REQ_TIMEOUT_MS 5000
#define OBSERVE_PERIOD_US (10LL * 1000 * 1000)
#define PREPARE_POLL_US (2LL * 1000 * 1000)
#define POSITION_MS_PER_SEGMENT 300000ULL /* 定长分段：300s @ 32KB/s */

typedef enum {
    CMD_FETCH_SHOWS,
    CMD_OPEN_SHOW,
    CMD_PLAY_EPISODE,
    CMD_PAUSE_RESUME,
    CMD_PAUSE,
    CMD_SEEK,
    CMD_VOLUME,
    CMD_STOP,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    int value;
} podcast_cmd_t;

typedef struct {
    char id[24];
    char title[TRIO_PODCAST_TITLE_MAX];
    uint64_t published;
} show_item_t;

typedef struct {
    char id[64];
    char title[TRIO_PODCAST_TITLE_MAX];
    unsigned duration;
    unsigned segments;
    bool ready;
} episode_item_t;

static struct {
    /* 共享目录（lock 保护；UI 短临界区拷贝） */
    SemaphoreHandle_t lock;
    show_item_t shows[TRIO_PODCAST_MAX_SHOWS];
    uint8_t show_count;
    bool shows_loading, shows_failed;
    bool shows_has_more;
    episode_item_t episodes[TRIO_PODCAST_MAX_EPISODES];
    uint8_t episode_count;
    uint8_t browse_show;
    bool eps_loading, eps_failed;
    bool eps_has_more;
    char cur_title[TRIO_PODCAST_TITLE_MAX];

    /* worker 私有 */
    QueueHandle_t cmds;
    TaskHandle_t worker;
    volatile bool shutdown_req;
    char server[TODO_CFG_SERVER_MAX];
    char device_id[TODO_CFG_DEVICE_ID_MAX];
    podcast_sync_t sync;
    bool sync_ready;
    int sync_active;
    uint64_t current_revision;
    uint32_t last_seek_id;         /* 上次观察到的绝对 seek 代数 */
    uint32_t start_session;        /* 起播前快照会话号：派发前忽略观察 */
    char cur_show[24], cur_episode[64];
    uint64_t cur_position_ms;
    unsigned cur_duration, cur_segments;
    bool cur_finished;
    bool has_current, preparing;
    bool wifi_ps_off;              /* 流式期间关闭 WiFi 省电（一次性） */
    int64_t next_observe;
    int64_t next_prepare_poll;
} p;

/* ---------- 小工具 ---------- */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    strlcpy(dst, src, cap);
}

/* ---------- HTTP（照搬 demo_podcast request()，联网状态改用 wifi_link） ---------- */

static cJSON *request(const char *path, const char *body) {
    if (!wifi_link_has_ip()) return NULL;
    char url[256];
    int len = snprintf(url, sizeof(url), "%s%s", p.server, path);
    if (len < 0 || len >= (int)sizeof(url)) return NULL;
    bool listening = !strncmp(path, "/api/listening/", 15);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = listening ? LISTEN_TIMEOUT_MS : REQ_TIMEOUT_MS,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .buffer_size = 512,
    };
    esp_http_client_handle_t client = podcast_http_client(cfg);
    if (!client) {
        if (!wifi_link_has_ip()) return NULL;
        /* 无令牌（尚未 claim）时触发一次同步 worker 唤醒加速配对 */
        todo_sync_kick();
        return NULL;
    }
    int body_size = body ? (int)strlen(body) : 0;
    if (body) esp_http_client_set_header(client, "Content-Type", "application/json");
    cJSON *json = NULL;
    char *text = NULL;
    if (esp_http_client_open(client, body_size) == ESP_OK) {
        if (!body || esp_http_client_write(client, body, body_size) == body_size) {
            int64_t size = esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status == 401 || status == 403) {
                ESP_LOGW(TAG, "鉴权被拒（%d），配对可能失效，已请求同步 worker 重试", status);
                todo_sync_kick();
            }
            if (status >= 200 && status < 300 && size > 0 && size <= PODCAST_MAX_JSON_BYTES) {
                text = malloc((size_t)size + 1);
                if (text) {
                    int received = 0;
                    while (received < size) {
                        int n = esp_http_client_read(client, text + received, (int)size - received);
                        if (n <= 0) break;
                        received += n;
                    }
                    if (received == size) {
                        text[received] = 0;
                        json = cJSON_Parse(text);
                    }
                }
            }
        }
    }
    free(text);
    esp_http_client_cleanup(client);
    return json;
}

static const char *jstr(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static unsigned jnum(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) && item->valuedouble > 0 && item->valuedouble <= 10000000
               ? (unsigned)item->valuedouble
               : 0;
}

static uint64_t jwhole(cJSON *obj, const char *key) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) && n->valuedouble > 0 && n->valuedouble <= 9007199254740991.0
               ? (uint64_t)n->valuedouble
               : 0;
}

/* 服务端 id 仅允许安全字符（拼 URL/JSON 用）。 */
static bool identity(char *dst, size_t cap, const char *src) {
    size_t len = strlen(src);
    if (!len || len >= cap) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = src[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

/* ---------- 目录拉取 ---------- */

static bool fetch_shows(void) {
    xSemaphoreTake(p.lock, portMAX_DELAY);
    p.shows_loading = true;
    p.shows_failed = false;
    xSemaphoreGive(p.lock);

    static show_item_t fresh[TRIO_PODCAST_MAX_SHOWS]; /* worker 单线程独占 */
    memset(fresh, 0, sizeof(fresh));
    int count = 0, offset = 0, expected = -1;
    bool ok = true;
    for (;;) {
        char path[96];
        snprintf(path, sizeof(path), "/api/shows_lite?offset=%d&limit=%d", offset,
                 PODCAST_SHOW_PAGE_SIZE);
        cJSON *json = request(path, NULL);
        if (!json) {
            ok = false;
            break;
        }
        cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "s"), *item;
        if (!cJSON_IsArray(array)) {
            cJSON_Delete(json);
            ok = false;
            break;
        }
        cJSON *total = cJSON_GetObjectItemCaseSensitive(json, "total");
        bool paged = cJSON_IsNumber(total);
        int available = paged ? total->valueint : -1;
        if (paged && (available < 0 || available > TRIO_PODCAST_MAX_SHOWS ||
                      (expected >= 0 && available != expected) ||
                      jnum(json, "offset") != (unsigned)offset)) {
            cJSON_Delete(json);
            ok = false;
            break; /* 目录在分页中途变化：整体重试 */
        }
        if (paged) expected = available;
        int response_count = cJSON_GetArraySize(array);
        cJSON_ArrayForEach(item, array) {
            if (count == TRIO_PODCAST_MAX_SHOWS) break;
            if (!identity(fresh[count].id, sizeof(fresh[count].id), jstr(item, "i"))) continue;
            copy_str(fresh[count].title, sizeof(fresh[count].title), jstr(item, "n"));
            fresh[count].published = jwhole(item, "p");
            ++count;
        }
        offset += response_count;
        cJSON_Delete(json);
        if (!paged || offset >= available || count >= TRIO_PODCAST_MAX_SHOWS) break;
    }

    /* 最近更新在前（同刻按服务端顺序稳定插入） */
    for (int i = 1; i < count; ++i) {
        show_item_t value = fresh[i];
        int at = i;
        while (at > 0 && fresh[at - 1].published < value.published) {
            fresh[at] = fresh[at - 1];
            --at;
        }
        fresh[at] = value;
    }

    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (ok) {
        memcpy(p.shows, fresh, sizeof(p.shows));
        p.show_count = (uint8_t)count;
        p.shows_has_more = expected > count;
        if (p.browse_show >= count) p.browse_show = 0;
    }
    p.shows_loading = false;
    p.shows_failed = !ok;
    xSemaphoreGive(p.lock);
    ESP_LOGI(TAG, "节目目录：%s（%d 项）", ok ? "已更新" : "失败", ok ? count : 0);
    return ok;
}

static bool read_episode(cJSON *item, episode_item_t *ep) {
    memset(ep, 0, sizeof(*ep));
    if (!identity(ep->id, sizeof(ep->id), jstr(item, "id"))) return false;
    copy_str(ep->title, sizeof(ep->title), jstr(item, "title"));
    ep->duration = jnum(item, "duration");
    ep->segments = jnum(item, "segments");
    ep->ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "ready"));
    return true;
}

static bool fetch_episode_page(uint8_t show_index, int offset) {
    if (show_index >= p.show_count) return false;
    char path[240];
    snprintf(path, sizeof(path), "/api/shows/%s/episodes?offset=%d&limit=%d&order=newest&lite=1",
             p.shows[show_index].id, offset, PODCAST_SHOW_PAGE_SIZE);
    cJSON *json = request(path, NULL);
    if (!json) return false;
    cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "episodes"), *item;
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(json);
        return false;
    }
    episode_item_t fresh[PODCAST_SHOW_PAGE_SIZE] = {0};
    int count = 0;
    cJSON_ArrayForEach(item, array) {
        if (count >= PODCAST_SHOW_PAGE_SIZE) break;
        if (read_episode(item, &fresh[count])) ++count;
    }
    int total = (int)jnum(json, "total");
    cJSON_Delete(json);

    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (offset == 0) memset(p.episodes, 0, sizeof(p.episodes));
    int base = offset;
    for (int i = 0; i < count && base + i < TRIO_PODCAST_MAX_EPISODES; ++i) {
        p.episodes[base + i] = fresh[i];
    }
    int kept = base + count;
    if (kept > TRIO_PODCAST_MAX_EPISODES) kept = TRIO_PODCAST_MAX_EPISODES;
    p.episode_count = (uint8_t)kept;
    p.eps_has_more = total > kept;
    p.browse_show = show_index;
    p.eps_loading = false;
    p.eps_failed = false;
    xSemaphoreGive(p.lock);
    ESP_LOGI(TAG, "剧集 %s：+%d（缓存 %d/total %d）", p.shows[show_index].id, count, kept, total);
    return count > 0 || offset > 0;
}

/* ---------- 收听上报（podcast_sync outbox） ---------- */

static podcast_sync_state_t sync_audio_state(podcast_player_state_t state) {
    return state == PODCAST_PLAYER_FINISHED ? PODCAST_SYNC_ENDED
           : state == PODCAST_PLAYER_PLAYING ? PODCAST_SYNC_PLAYING
           : state == PODCAST_PLAYER_IDLE || state == PODCAST_PLAYER_ERROR ? PODCAST_SYNC_STOPPED
                                                                           : PODCAST_SYNC_PAUSED;
}

/* 观察 player 快照 → outbox 记录（简化自 demo sync_observe_snapshot：
 * 保留关闭会话终态与活动会话观察，去掉多代 resume 对账）。 */
static void sync_observe(const podcast_player_snapshot_t *s, bool close) {
    if (!p.sync_ready) return;

    /* 被 STOP/新 START 替换掉的旧会话：补一条终态观察 */
    for (int i = 0; i < PODCAST_SYNC_SLOTS; ++i) {
        podcast_sync_record_t *r = &p.sync.records[i];
        if (r->used && r->audio_session && r->audio_session == s->closed_session_id) {
            podcast_sync_state_t ended =
                r->state == PODCAST_SYNC_ENDED ? PODCAST_SYNC_ENDED : PODCAST_SYNC_STOPPED;
            if (s->closed_heard_ms > r->heard_origin_ms + r->heard_ms ||
                s->closed_elapsed_ms != r->position_ms || r->state != ended) {
                uint64_t heard =
                    s->closed_heard_ms >= r->heard_origin_ms ? s->closed_heard_ms - r->heard_origin_ms : 0;
                podcast_sync_observe(&p.sync, i, r->audio_session, s->closed_elapsed_ms, heard,
                                     ended, false);
                r->audio_session = 0;
                (void)podcast_sync_freeze(&p.sync, i);
            }
        }
    }

    int at = p.sync_active;
    if (at < 0 || !p.has_current || strcmp(s->show_id, p.cur_show) ||
        strcmp(s->episode_id, p.cur_episode))
        return;
    if (s->session_id == p.start_session) return; /* 起播尚未派发：旧快照不观察 */

    podcast_sync_record_t *r = &p.sync.records[at];
    if (!r->used) return;
    bool seek = s->absolute_seek_id != p.last_seek_id;
    p.last_seek_id = s->absolute_seek_id;
    podcast_sync_state_t st =
        close ? (s->state == PODCAST_PLAYER_FINISHED || r->state == PODCAST_SYNC_ENDED
                     ? PODCAST_SYNC_ENDED
                     : PODCAST_SYNC_STOPPED)
              : sync_audio_state(s->state);
    uint64_t heard = s->heard_ms >= r->heard_origin_ms ? s->heard_ms - r->heard_origin_ms : 0;
    bool changed = st != r->state || seek;
    podcast_sync_observe(&p.sync, at, s->session_id, s->elapsed_ms, heard, st, seek);
    if (close || changed || now_us() >= p.next_observe) {
        if (!podcast_sync_freeze(&p.sync, at)) ESP_LOGE(TAG, "同步记录落盘失败");
        p.next_observe = now_us() + OBSERVE_PERIOD_US;
    }
}

/* 发送单条 outbox 记录：先建会话（无 session_id 时），再补事件。 */
static bool sync_send_record(int at) {
    if (!p.sync_ready || at < 0) return false;
    podcast_sync_record_t *record = &p.sync.records[at];
    char body[512];
    cJSON *json;
    if (!record->session_id[0]) {
        if (!podcast_sync_creation_body(&p.sync, at, body, sizeof(body))) return false;
        json = request("/api/listening/sessions", body);
        if (!json) return false;
        bool ok = podcast_sync_opened(&p.sync, at, jstr(json, "session_id"),
                                      jwhole(json, "position_ms"), jnum(json, "next_seq"),
                                      cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "stale")));
        cJSON_Delete(json);
        if (!ok) return false;
    }
    if (!podcast_sync_freeze(&p.sync, at)) return false;
    if (!record->pending) return true;
    if (!podcast_sync_event_body(&p.sync, at, body, sizeof(body))) return false;
    json = request("/api/listening/events", body);
    if (!json) return false;
    bool accepted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "accepted"));
    uint64_t v = jwhole(cJSON_GetObjectItemCaseSensitive(json, "progress"), "revision");
    if (accepted) {
        accepted = podcast_sync_acknowledge(&p.sync, at, v);
        if (accepted && !p.sync.records[at].used && p.sync_active == at) p.sync_active = -1;
    }
    cJSON_Delete(json);
    return accepted;
}

static void flush_sync(void) {
    if (!p.sync_ready || !wifi_link_has_ip()) return;
    for (int n = 0; n < 3; ++n) {
        int at = podcast_sync_pending(&p.sync);
        if (at < 0) break;
        if (!sync_send_record(at)) break;
    }
}

/* ---------- 起播流程 ---------- */

static bool sync_begin_current(void) {
    if (!p.sync_ready) return true;
    podcast_sync_clock(&p.sync, (uint64_t)(now_us() / 1000));
    bool offline = !wifi_link_has_ip() ||
                   podcast_sync_has_offline(&p.sync, p.cur_show, p.cur_episode) ||
                   (!p.current_revision && p.cur_position_ms);
    int previous = p.sync_active;
    p.sync_active = podcast_sync_begin(&p.sync, p.cur_show, p.cur_episode, p.cur_position_ms,
                                       p.current_revision, offline, false);
    if (p.sync_active < 0) {
        p.sync_active = previous;
        ESP_LOGW(TAG, "离线记录待同步，本次不上报");
        return false;
    }
    if (wifi_link_has_ip()) {
        char body[512];
        if (podcast_sync_creation_body(&p.sync, p.sync_active, body, sizeof(body))) {
            cJSON *json = request("/api/listening/sessions", body);
            if (json) {
                bool ok = podcast_sync_opened(
                    &p.sync, p.sync_active, jstr(json, "session_id"), jwhole(json, "position_ms"),
                    jnum(json, "next_seq"),
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "stale")));
                cJSON_Delete(json);
                if (ok && !offline) {
                    podcast_sync_record_t *r = &p.sync.records[p.sync_active];
                    p.cur_position_ms = r->position_ms; /* 中心位置可能回拨 */
                }
            }
        }
    }
    p.next_observe = now_us() + OBSERVE_PERIOD_US;
    return true;
}

static bool start_current(void) {
    (void)sync_begin_current();
    podcast_player_cursor_t cursor = {
        .segment = (uint32_t)(p.cur_position_ms / POSITION_MS_PER_SEGMENT),
        .byte_offset = (p.cur_position_ms % POSITION_MS_PER_SEGMENT) * 32,
    };
    if (p.cur_finished || cursor.segment >= p.cur_segments) {
        cursor.segment = 0;
        cursor.byte_offset = 0;
    }
    podcast_player_snapshot_t live;
    if (podcast_player_snapshot(&live)) p.start_session = live.session_id;

    /* 音量对齐：任务页音量循环直接写 bsp，起播前把播放器会话音量拉平。 */
    podcast_player_set_volume(bsp_audio_volume_get());

    if (!podcast_player_start(p.cur_show, p.cur_episode, cursor, p.cur_segments)) {
        ESP_LOGW(TAG, "播放器忙，起播被拒");
        return false;
    }
    /* 流式播放对 WiFi 省电敏感：起播后一次性关闭（demo_podcast 同款）。 */
    if (!p.wifi_ps_off) {
        if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) p.wifi_ps_off = true;
    }
    p.preparing = false;
    p.last_seek_id = 0;
    ESP_LOGI(TAG, "起播 %s/%s @ %llums", p.cur_show, p.cur_episode,
             (unsigned long long)p.cur_position_ms);
    return true;
}

/* 拉单集详情：ready 则起播；未 ready 则提交服务端预转并进入轮询。 */
static bool details(bool begin_prepare) {
    char path[160];
    snprintf(path, sizeof(path), "/api/episodes/%s/%s?lite=1", p.cur_show, p.cur_episode);
    cJSON *json = request(path, NULL);
    if (!json) return false;
    p.cur_duration = jnum(json, "duration");
    p.current_revision = jwhole(json, "v");
    uint64_t central = jwhole(json, "p");
    p.cur_finished = jnum(json, "c") == 2;
    if (central) {
        p.cur_position_ms = central;
    } else if (!p.current_revision) {
        p.cur_position_ms = 0; /* 服务端无进度且无版本：从头开始 */
    }
    const char *title = jstr(json, "title");
    if (title[0]) copy_str(p.cur_title, sizeof(p.cur_title), title);
    cJSON *segments = cJSON_GetObjectItemCaseSensitive(json, "segments");
    p.cur_segments = jnum(json, "segment_count");
    if (!p.cur_segments && cJSON_IsArray(segments))
        p.cur_segments = (unsigned)cJSON_GetArraySize(segments);
    bool ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ready"));
    bool failed = strcmp(jstr(json, "status"), "failed") == 0;
    cJSON_Delete(json);

    if (ready && p.cur_segments && p.cur_segments <= 256) return start_current();
    if (ready && p.cur_segments > 256) {
        p.preparing = false;
        ESP_LOGW(TAG, "单集过长，暂不支持");
        return false;
    }
    if (!begin_prepare) {
        if (failed) p.preparing = false;
        return false;
    }
    char body[192];
    snprintf(body, sizeof(body), "{\"show_id\":\"%s\",\"episode_id\":\"%s\"}", p.cur_show,
             p.cur_episode);
    json = request("/api/prepare", body);
    if (!json) {
        p.preparing = false;
        return false;
    }
    cJSON_Delete(json);
    p.preparing = true;
    p.next_prepare_poll = now_us() + PREPARE_POLL_US;
    ESP_LOGI(TAG, "服务端预转音频中…");
    return true;
}

static void play_episode(int idx) {
    xSemaphoreTake(p.lock, portMAX_DELAY);
    if (idx < 0 || idx >= p.episode_count || p.browse_show >= p.show_count) {
        xSemaphoreGive(p.lock);
        return;
    }
    episode_item_t ep = p.episodes[idx];
    char show_id[24];
    strlcpy(show_id, p.shows[p.browse_show].id, sizeof(show_id));
    xSemaphoreGive(p.lock);

    podcast_player_snapshot_t closing;
    if (podcast_player_snapshot(&closing)) sync_observe(&closing, true);
    if (!podcast_player_stop()) {
        ESP_LOGW(TAG, "播放器忙，无法切换单集");
        return;
    }
    rtttl_player_stop(); /* 残余提示音让位给播客音频 */

    strlcpy(p.cur_show, show_id, sizeof(p.cur_show));
    strlcpy(p.cur_episode, ep.id, sizeof(p.cur_episode));
    copy_str(p.cur_title, sizeof(p.cur_title), ep.title);
    p.cur_duration = ep.duration;
    p.cur_segments = ep.segments;
    p.cur_position_ms = 0;
    p.cur_finished = false;
    p.has_current = true;
    p.current_revision = 0;
    p.preparing = false;
    (void)details(true);
}

/* ---------- 命令处理与周期任务 ---------- */

static void handle_cmd(const podcast_cmd_t *cmd) {
    switch (cmd->kind) {
    case CMD_FETCH_SHOWS:
        fetch_shows();
        break;
    case CMD_OPEN_SHOW:
        xSemaphoreTake(p.lock, portMAX_DELAY);
        p.eps_loading = true;
        p.eps_failed = false;
        xSemaphoreGive(p.lock);
        if (!fetch_episode_page((uint8_t)cmd->value, 0)) {
            xSemaphoreTake(p.lock, portMAX_DELAY);
            p.eps_loading = false;
            p.eps_failed = true;
            xSemaphoreGive(p.lock);
        }
        break;
    case CMD_PLAY_EPISODE:
        play_episode(cmd->value);
        break;
    case CMD_PAUSE_RESUME: {
        podcast_player_snapshot_t s;
        if (!podcast_player_snapshot(&s)) break;
        if (s.state == PODCAST_PLAYER_PAUSED) {
            podcast_player_resume(); /* 状态变化由周期观察补报 */
        } else if (s.state == PODCAST_PLAYER_PLAYING || s.state == PODCAST_PLAYER_BUFFERING) {
            if (podcast_player_pause()) {
                podcast_player_snapshot_t now;
                if (podcast_player_snapshot(&now)) sync_observe(&now, false);
            }
        }
        break;
    }
    case CMD_PAUSE:
        podcast_player_pause();
        break;
    case CMD_SEEK:
        podcast_player_seek_relative(cmd->value);
        break;
    case CMD_VOLUME:
        podcast_player_adjust_volume(cmd->value);
        break;
    case CMD_STOP: {
        podcast_player_snapshot_t s;
        if (podcast_player_snapshot(&s)) sync_observe(&s, true);
        podcast_player_stop();
        p.has_current = false; /* 终态观察已封存，停止后续观察 */
        p.sync_active = -1;
        break;
    }
    default:
        break;
    }
}

static void periodic(void) {
    if (p.preparing && now_us() >= p.next_prepare_poll) {
        if (!details(false)) p.next_prepare_poll = now_us() + PREPARE_POLL_US;
    }
    if (p.has_current && !p.preparing) {
        podcast_player_snapshot_t s;
        if (podcast_player_snapshot(&s)) sync_observe(&s, false);
    }
    flush_sync();
}

static void worker_task(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "worker started");
    podcast_cmd_t cmd;
    for (;;) {
        if (p.shutdown_req) break; /* 应用 exit：worker 自删，避免持锁被外部强杀 */
        if (xQueueReceive(p.cmds, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            handle_cmd(&cmd);
        }
        periodic();
    }
    ESP_LOGW(TAG, "worker stopped");
    p.worker = NULL;
    vTaskDelete(NULL);
}

/* ---------- 对外接口 ---------- */

static bool send_cmd(cmd_kind_t kind, int value) {
    if (p.cmds == NULL || p.server[0] == '\0') {
        ESP_LOGW(TAG, "send_cmd %d 被拒：cmds=%p server=%s", (int)kind, (void *)p.cmds,
                 p.server[0] ? "已配" : "空");
        return false;
    }
    const podcast_cmd_t cmd = { .kind = kind, .value = value };
    return xQueueSend(p.cmds, &cmd, 0) == pdTRUE;
}

void trio_podcast_init(void) {
    if (p.worker != NULL) return; /* 已在运行（幂等） */
    if (!todo_cfg_load(p.server, sizeof(p.server), NULL, 0, NULL, 0) || p.server[0] == '\0') {
        ESP_LOGW(TAG, "未配置服务端，播客功能停用");
        return;
    }
    /* 历史配网可能存入尾斜杠（如 http://host:8899/），拼接路径会变成 //api/... */
    size_t len = strlen(p.server);
    while (len > 0 && p.server[len - 1] == '/') p.server[--len] = '\0';
    if (!todo_cfg_device_id(p.device_id, sizeof(p.device_id))) {
        ESP_LOGE(TAG, "设备标识生成失败，播客功能停用");
        p.server[0] = '\0';
        return;
    }
    p.lock = xSemaphoreCreateMutex();
    p.cmds = xQueueCreate(8, sizeof(podcast_cmd_t));
    if (p.lock == NULL || p.cmds == NULL) {
        ESP_LOGE(TAG, "播客队列创建失败");
        return;
    }
    /* 收听上报的 client_id 必须与配对时注册的设备编号一致（服务端
     * device_client 校验，不一致 403）。podcast_sync 以 uint64 种子还原
     * "device-%016llx" 字符串，因此从持久化 device_id 反解出数值。 */
    uint64_t seed = 0;
    {
        const char *hex = p.device_id;
        if (!strncmp(hex, "device-", 7)) hex += 7;
        for (int i = 0; i < 16 && hex[i]; ++i) {
            char c = hex[i];
            unsigned v;
            if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
            else {
                c |= 0x20;
                v = (unsigned)(c - 'a') + 10u;
            }
            seed = (seed << 4) | (uint64_t)v;
        }
    }
    p.sync_ready = podcast_sync_init(&p.sync, seed);
    if (!p.sync_ready) ESP_LOGE(TAG, "收听上报存储初始化失败（仅影响上报，不影响播放）");
    p.sync_active = -1;
    bool player_ok = podcast_player_init(p.server);
    if (!player_ok) ESP_LOGE(TAG, "播放器初始化失败，仅播放不可用");
    /* worker 必须无条件创建：否则目录命令会入队后无人消费（孤儿化静默失败）。 */
    p.shutdown_req = false;
    /* worker 6144：目录/单集 cJSON 解析与 todo_sync worker 同级；
     * 8192 会白白挤占堆——播客模式堆是起播成败的关键资源。 */
    if (xTaskCreate(worker_task, "trio_podcast", 6144, NULL, 3, &p.worker) != pdPASS) {
        ESP_LOGE(TAG, "播客 worker 启动失败");
        p.worker = NULL;
        return;
    }
    ESP_LOGW(TAG, "init ok: server=%s player=%d sync=%d", p.server, player_ok, p.sync_ready);
}

bool trio_podcast_shows(trio_podcast_title_t *out, uint8_t cap, trio_podcast_list_info_t *info) {
    if (p.lock == NULL) {
        if (info) memset(info, 0, sizeof(*info));
        return false;
    }
    xSemaphoreTake(p.lock, portMAX_DELAY);
    uint8_t n = p.show_count < cap ? p.show_count : cap;
    for (uint8_t i = 0; i < n; ++i) copy_str(out[i].text, sizeof(out[i].text), p.shows[i].title);
    if (info) {
        info->loading = p.shows_loading;
        info->failed = p.shows_failed;
        info->has_more = p.shows_has_more;
        info->count = p.show_count;
    }
    xSemaphoreGive(p.lock);
    return true;
}

bool trio_podcast_episodes(trio_podcast_title_t *out, uint8_t cap, trio_podcast_list_info_t *info) {
    if (p.lock == NULL) {
        if (info) memset(info, 0, sizeof(*info));
        return false;
    }
    xSemaphoreTake(p.lock, portMAX_DELAY);
    uint8_t n = p.episode_count < cap ? p.episode_count : cap;
    for (uint8_t i = 0; i < n; ++i) copy_str(out[i].text, sizeof(out[i].text), p.episodes[i].title);
    if (info) {
        info->loading = p.eps_loading;
        info->failed = p.eps_failed;
        info->has_more = p.eps_has_more;
        info->count = p.episode_count;
    }
    xSemaphoreGive(p.lock);
    return true;
}

bool trio_podcast_player_snapshot(podcast_player_snapshot_t *out) {
    return podcast_player_snapshot(out);
}

bool trio_podcast_current_title(char *out, size_t cap) {
    if (p.lock == NULL) return false;
    xSemaphoreTake(p.lock, portMAX_DELAY);
    const char *title = p.cur_title[0] ? p.cur_title : NULL;
    bool has = p.has_current;
    if (has) copy_str(out, cap, title ? title : p.cur_show);
    xSemaphoreGive(p.lock);
    return has;
}

bool trio_podcast_playing(void) {
    podcast_player_snapshot_t s;
    if (!podcast_player_snapshot(&s)) return false;
    return s.state == PODCAST_PLAYER_PLAYING || s.state == PODCAST_PLAYER_BUFFERING;
}

bool trio_podcast_fetch_shows(void) { return send_cmd(CMD_FETCH_SHOWS, 0); }
bool trio_podcast_open_show(uint8_t index) { return send_cmd(CMD_OPEN_SHOW, index); }
bool trio_podcast_play_episode(uint8_t index) { return send_cmd(CMD_PLAY_EPISODE, index); }
bool trio_podcast_pause_resume(void) { return send_cmd(CMD_PAUSE_RESUME, 0); }
bool trio_podcast_pause(void) { return send_cmd(CMD_PAUSE, 0); }
bool trio_podcast_seek(int seconds) { return send_cmd(CMD_SEEK, seconds); }
bool trio_podcast_volume(int delta) {
    /* 直通播放器命令队列：绕过本 worker（每圈仅消化 1 条命令且常被上报
     * HTTP 阻塞 500-800ms），否则快速连按会挤爆深度 8 的命令队列被丢包。
     * 音量无需与收听上报串行化，播放器 worker 天然线程安全。 */
    return podcast_player_adjust_volume(delta);
}
bool trio_podcast_stop(void) { return send_cmd(CMD_STOP, 0); }
