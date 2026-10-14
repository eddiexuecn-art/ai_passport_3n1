// main/app_podcast.c —— PODCAST 应用实现（屏幕/按键自 trio_podcast_ui 搬迁）。
// 退出=整机重启（双击确定由 main 统一拦截），本应用不感知主控页；
// SHOWS 页长按确定不再跳转任务列表（那是旧"常驻共存"架构的入口）。
#include "app_podcast.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "trio_podcast.h"
#include "wifi_link.h"

LV_FONT_DECLARE(app_cjk_18);

static const char *TAG = "app_podcast";

/* 调色板与全局风格一致 */
#define C_BG 0xFFFFFF
#define C_INK 0x1F2328
#define C_MUTED 0x8A9299
#define C_LINE 0xE5E1D8
#define C_FOCUS 0xE0533D

#define LIST_TOP 46u
#define ROW_H 36u
#define BAR_H 34u
#define VISIBLE_ROWS 6u
#define F_SMALL &lv_font_montserrat_14
#define F_HEAD &lv_font_montserrat_20
#define F_BIG &lv_font_montserrat_48
#define F_CJK &app_cjk_18

typedef enum { VIEW_SHOWS = 0, VIEW_EPS, VIEW_PLAY } pod_view_t;

static lv_obj_t *s_scr_shows;
static lv_obj_t *s_scr_eps;
static lv_obj_t *s_scr_play;

static lv_obj_t *s_show_name[VISIBLE_ROWS];
static lv_obj_t *s_show_mark[VISIBLE_ROWS];
static lv_obj_t *s_show_bar;
static lv_obj_t *s_ep_name[VISIBLE_ROWS];
static lv_obj_t *s_ep_mark[VISIBLE_ROWS];
static lv_obj_t *s_ep_bar;

static lv_obj_t *s_play_title;
static lv_obj_t *s_play_clock;
static lv_obj_t *s_play_sub;
static lv_obj_t *s_play_bar;      /* lv_bar 进度 */
static lv_obj_t *s_play_footer;

static pod_view_t s_view = VIEW_SHOWS;
static uint8_t s_sel;
static uint8_t s_first; /* 滚动窗口首行 */

/* ---------- 构件 ---------- */

static lv_obj_t *page_create(const char *title, lv_obj_t **screen) {
    lv_obj_t *scr = lv_obj_create(NULL);
    if (scr == NULL) {
        ESP_LOGE(TAG, "lv_obj_create 失败：LVGL 池耗尽（%s）", title);
        *screen = NULL;
        return NULL;
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t *head = lv_label_create(scr);
    lv_label_set_text(head, title);
    lv_obj_set_style_text_font(head, F_HEAD, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(C_FOCUS), 0);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 240, BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar_line = lv_obj_create(scr);
    lv_obj_set_size(bar_line, 240, 1);
    lv_obj_set_style_bg_color(bar_line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(bar_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_line, 0, 0);
    lv_obj_set_style_radius(bar_line, 0, 0);
    lv_obj_align(bar_line, LV_ALIGN_BOTTOM_MID, 0, -(BAR_H + 1));

    *screen = scr;
    return bar;
}

static lv_obj_t *bar_label(lv_obj_t *bar) {
    lv_obj_t *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, F_SMALL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(C_INK), 0);
    lv_obj_center(label);
    return label;
}

static lv_obj_t *row_build(lv_obj_t *scr, lv_obj_t **name, lv_obj_t **mark) {
    *mark = lv_obj_create(scr);
    *name = lv_label_create(scr);
    if (*mark == NULL || *name == NULL) {
        ESP_LOGE(TAG, "row_build 失败：LVGL 池耗尽");
        *mark = NULL;
        *name = NULL;
        return NULL;
    }
    lv_obj_set_size(*mark, 3, ROW_H - 6);
    lv_obj_set_style_radius(*mark, 2, 0);
    lv_obj_set_style_border_width(*mark, 0, 0);

    lv_obj_set_width(*name, 200);
    lv_obj_set_style_text_font(*name, F_CJK, 0);
    lv_obj_set_style_text_color(*name, lv_color_hex(C_INK), 0);
    lv_label_set_long_mode(*name, LV_LABEL_LONG_DOT);
    return *name;
}

static void format_mmss(char *out, size_t cap, uint32_t sec) {
    const uint32_t max = 99u * 60u + 59u; /* 让 gcc 确认 9 字节缓冲足够 */
    if (sec > max) sec = max;
    snprintf(out, cap, "%02u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

/* ---------- SHOWS ---------- */

static void shows_refresh(void) {
    static trio_podcast_title_t items[TRIO_PODCAST_MAX_SHOWS];
    trio_podcast_list_info_t info;
    trio_podcast_shows(items, TRIO_PODCAST_MAX_SHOWS, &info);
    uint8_t sel = s_sel < info.count ? s_sel : 0;

    if (info.count == 0) s_first = 0;
    else {
        if (sel < s_first) s_first = sel;
        if (sel >= s_first + VISIBLE_ROWS) s_first = (uint8_t)(sel - VISIBLE_ROWS + 1);
    }
    for (uint8_t i = 0; i < VISIBLE_ROWS; ++i) {
        uint8_t idx = s_first + i;
        if (idx >= info.count) {
            lv_label_set_text(s_show_name[i], "");
            lv_obj_set_style_bg_opa(s_show_mark[i], LV_OPA_TRANSP, 0);
            continue;
        }
        lv_label_set_text(s_show_name[i], items[idx].text);
        lv_obj_set_style_text_color(s_show_name[i],
                                    lv_color_hex(idx == sel ? C_FOCUS : C_INK), 0);
        lv_obj_set_style_bg_color(s_show_mark[i],
                                  lv_color_hex(idx == sel ? C_FOCUS : C_LINE), 0);
        lv_obj_set_style_bg_opa(s_show_mark[i], LV_OPA_COVER, 0);
        lv_coord_t y = (lv_coord_t)(LIST_TOP + i * ROW_H);
        lv_obj_align(s_show_mark[i], LV_ALIGN_TOP_LEFT, 10, y + 3);
        lv_obj_align(s_show_name[i], LV_ALIGN_TOP_LEFT, 24, y + 8);
    }

    if (info.loading) lv_label_set_text(s_show_bar, "LOADING...");
    else if (info.count == 0 && info.failed) lv_label_set_text(s_show_bar, "RETRY: OK");
    else if (info.count == 0) lv_label_set_text(s_show_bar, "NO SHOWS");
    else lv_label_set_text(s_show_bar, "OK OPEN");
}

/* ---------- EPISODES ---------- */

static void eps_refresh(void) {
    static trio_podcast_title_t items[TRIO_PODCAST_MAX_EPISODES];
    trio_podcast_list_info_t info;
    trio_podcast_episodes(items, TRIO_PODCAST_MAX_EPISODES, &info);
    uint8_t sel = s_sel < info.count ? s_sel : 0;

    if (info.count == 0) s_first = 0;
    else {
        if (sel < s_first) s_first = sel;
        if (sel >= s_first + VISIBLE_ROWS) s_first = (uint8_t)(sel - VISIBLE_ROWS + 1);
    }
    for (uint8_t i = 0; i < VISIBLE_ROWS; ++i) {
        uint8_t idx = s_first + i;
        if (idx >= info.count) {
            lv_label_set_text(s_ep_name[i], "");
            lv_obj_set_style_bg_opa(s_ep_mark[i], LV_OPA_TRANSP, 0);
            continue;
        }
        lv_label_set_text(s_ep_name[i], items[idx].text);
        lv_obj_set_style_text_color(s_ep_name[i], lv_color_hex(idx == sel ? C_FOCUS : C_INK), 0);
        lv_obj_set_style_bg_color(s_ep_mark[i], lv_color_hex(idx == sel ? C_FOCUS : C_LINE), 0);
        lv_obj_set_style_bg_opa(s_ep_mark[i], LV_OPA_COVER, 0);
        lv_coord_t y = (lv_coord_t)(LIST_TOP + i * ROW_H);
        lv_obj_align(s_ep_mark[i], LV_ALIGN_TOP_LEFT, 10, y + 3);
        lv_obj_align(s_ep_name[i], LV_ALIGN_TOP_LEFT, 24, y + 8);
    }

    if (info.loading) lv_label_set_text(s_ep_bar, "LOADING...");
    else if (info.count == 0 && info.failed) lv_label_set_text(s_ep_bar, "RETRY: OK");
    else if (info.count == 0) lv_label_set_text(s_ep_bar, "NO EPISODES");
    else lv_label_set_text(s_ep_bar, "OK PLAY \xC2\xB7 HOLD OK BACK");
}

/* ---------- PLAYING ---------- */

static void play_refresh(void) {
    char title[TRIO_PODCAST_TITLE_MAX];
    if (!trio_podcast_current_title(title, sizeof(title))) title[0] = '\0';
    lv_label_set_text(s_play_title, title);

    podcast_player_snapshot_t s;
    if (!trio_podcast_player_snapshot(&s)) {
        lv_label_set_text(s_play_sub, "");
        lv_label_set_text(s_play_footer, "OK PLAY \xC2\xB7 HOLD OK STOP");
        return;
    }

    uint32_t total = (uint32_t)s.total_seconds;
    if (!total && s.segment_count) total = s.segment_count * 300u;
    uint32_t elapsed = (uint32_t)s.elapsed_seconds;
    char a[9], b[9];
    format_mmss(a, sizeof(a), elapsed);
    format_mmss(b, sizeof(b), total);
    lv_label_set_text(s_play_clock, a);
    lv_label_set_text_fmt(s_play_sub, "%s / %s", a, b);
    if (total) lv_bar_set_value(s_play_bar, (int32_t)(elapsed * 100 / total), LV_ANIM_OFF);
    else lv_bar_set_value(s_play_bar, 0, LV_ANIM_OFF);

    switch (s.state) {
    case PODCAST_PLAYER_BUFFERING:
        lv_label_set_text(s_play_footer, "BUFFERING...");
        break;
    case PODCAST_PLAYER_PLAYING:
        lv_label_set_text(s_play_footer, "OK PAUSE \xC2\xB7 HOLD OK STOP");
        break;
    case PODCAST_PLAYER_PAUSED:
        lv_label_set_text(s_play_footer, "OK RESUME \xC2\xB7 HOLD OK STOP");
        break;
    case PODCAST_PLAYER_FINISHED:
        lv_label_set_text(s_play_footer, "FINISHED");
        break;
    case PODCAST_PLAYER_ERROR:
        lv_label_set_text(s_play_footer, s.error ? s.error : "ERROR");
        break;
    default:
        lv_label_set_text(s_play_footer, "OK PLAY \xC2\xB7 HOLD OK STOP");
        break;
    }
}

/* ---------- 页面构建 ---------- */

static void podcast_screens_build(void) {
    /* 底部条必须经 bar_label 建出"条内 label"：直接把容器当 label 用会
     * 越过 lv_obj 块边界野写 LVGL 池（此前 PODCAST 闪退的根因）。 */
    s_show_bar = bar_label(page_create("PODCAST", &s_scr_shows));
    for (uint8_t i = 0; i < VISIBLE_ROWS; ++i) row_build(s_scr_shows, &s_show_name[i], &s_show_mark[i]);

    s_ep_bar = bar_label(page_create("EPISODES", &s_scr_eps));
    for (uint8_t i = 0; i < VISIBLE_ROWS; ++i) row_build(s_scr_eps, &s_ep_name[i], &s_ep_mark[i]);

    s_play_footer = bar_label(page_create("NOW PLAYING", &s_scr_play));
    s_play_title = lv_label_create(s_scr_play);
    lv_obj_set_style_text_font(s_play_title, F_CJK, 0);
    lv_obj_set_style_text_color(s_play_title, lv_color_hex(C_INK), 0);
    lv_obj_set_width(s_play_title, 216);
    lv_label_set_long_mode(s_play_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_play_title, LV_ALIGN_TOP_MID, 0, 54);

    s_play_clock = lv_label_create(s_scr_play);
    lv_obj_set_style_text_font(s_play_clock, F_BIG, 0);
    lv_obj_set_style_text_color(s_play_clock, lv_color_hex(C_INK), 0);
    lv_obj_align(s_play_clock, LV_ALIGN_TOP_MID, 0, 120);

    s_play_bar = lv_bar_create(s_scr_play);
    lv_obj_set_size(s_play_bar, 200, 6);
    lv_bar_set_range(s_play_bar, 0, 100);
    lv_bar_set_value(s_play_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_play_bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_color(s_play_bar, lv_color_hex(C_FOCUS), LV_PART_INDICATOR);
    lv_obj_align(s_play_bar, LV_ALIGN_TOP_MID, 0, 200);

    s_play_sub = lv_label_create(s_scr_play);
    lv_obj_set_style_text_font(s_play_sub, F_SMALL, 0);
    lv_obj_set_style_text_color(s_play_sub, lv_color_hex(C_MUTED), 0);
    lv_obj_align(s_play_sub, LV_ALIGN_TOP_MID, 0, 216);
}

/* ---------- 按键 ---------- */

static void shows_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    trio_podcast_list_info_t info;
    trio_podcast_title_t probe[1];
    trio_podcast_shows(probe, 0, &info);
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (info.count == 0) return;
        int next = s_sel + (btn == BSP_BTN_DOWN ? 1 : -1);
        if (next < 0) next = 0;
        if (next >= info.count) next = info.count - 1;
        s_sel = (uint8_t)next;
        shows_refresh();
    } else if (btn == BSP_BTN_OK) {
        if (info.count == 0) {
            if (!info.loading) trio_podcast_fetch_shows();
            return;
        }
        if (trio_podcast_open_show(s_sel)) {
            s_view = VIEW_EPS;
            s_sel = 0;
            s_first = 0;
            eps_refresh();
            lv_screen_load(s_scr_eps);
        }
    }
}

static void eps_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    trio_podcast_list_info_t info;
    trio_podcast_title_t probe[1];
    trio_podcast_episodes(probe, 0, &info);
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_view = VIEW_SHOWS;
        shows_refresh();
        lv_screen_load(s_scr_shows);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (info.count == 0) return;
        int next = s_sel + (btn == BSP_BTN_DOWN ? 1 : -1);
        if (next < 0) next = 0;
        if (next >= info.count) next = info.count - 1;
        s_sel = (uint8_t)next;
        eps_refresh();
    } else if (btn == BSP_BTN_OK) {
        if (info.count == 0) {
            if (!info.loading) {
                trio_podcast_list_info_t shows_info;
                trio_podcast_shows(probe, 0, &shows_info);
                if (shows_info.count) trio_podcast_open_show(s_sel < shows_info.count ? s_sel : 0);
            }
            return;
        }
        if (trio_podcast_play_episode(s_sel)) {
            s_view = VIEW_PLAY;
            play_refresh();
            lv_screen_load(s_scr_play);
        }
    }
}

static void play_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG) {
        if (btn == BSP_BTN_OK) {
            trio_podcast_stop();
            s_view = VIEW_EPS;
            eps_refresh();
            lv_screen_load(s_scr_eps);
        } else if (btn == BSP_BTN_UP) {
            trio_podcast_seek(-15);
        } else if (btn == BSP_BTN_DOWN) {
            trio_podcast_seek(15);
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) trio_podcast_pause_resume();
    else if (btn == BSP_BTN_UP) trio_podcast_volume(20);
    else if (btn == BSP_BTN_DOWN) trio_podcast_volume(-20);
}

/* ---------- 生命周期 ---------- */

static void app_podcast_enter(void) {
    s_view = VIEW_SHOWS;
    s_sel = 0;
    s_first = 0;
    trio_podcast_init(); /* 幂等：首建 worker+player；未配 server 则 UI 走 RETRY 态 */
    podcast_screens_build();

    trio_podcast_list_info_t info;
    trio_podcast_title_t probe[1];
    trio_podcast_shows(probe, 0, &info); /* cap=0 仅取状态 */
    if (info.count == 0 && !info.loading && !info.failed) trio_podcast_fetch_shows();
    shows_refresh();
    lv_screen_load(s_scr_shows);
}

static void app_podcast_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    lv_obj_t *current = lv_screen_active();
    if (current == s_scr_shows) shows_key(btn, ev);
    else if (current == s_scr_eps) eps_key(btn, ev);
    else if (current == s_scr_play) play_key(btn, ev);
}

static void app_podcast_tick(void) {
    switch (s_view) {
    case VIEW_SHOWS: {
        /* 目录失败（开机拉取早于 WiFi 就绪）时，IP 一到就自动重拉，每 5s 封顶 */
        static int64_t next_retry_us;
        trio_podcast_list_info_t info;
        trio_podcast_title_t probe[1];
        trio_podcast_shows(probe, 0, &info);
        int64_t now = esp_timer_get_time();
        if (info.count == 0 && info.failed && !info.loading &&
            now >= next_retry_us && wifi_link_has_ip()) {
            next_retry_us = now + 5000000LL;
            trio_podcast_fetch_shows();
        }
        shows_refresh();
        break;
    }
    case VIEW_EPS: eps_refresh(); break;
    default: play_refresh(); break;
    }
}

const trio_app_t APP_PODCAST = {
    .enter = app_podcast_enter,
    .key = app_podcast_key,
    .tick = app_podcast_tick,
};
