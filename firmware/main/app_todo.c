// main/app_todo.c —— TODO 应用实现。屏幕/按键/tick 自 ui_trio 时代搬迁，
// 结构不变；生命周期按 app_framework.h 收口：上电 enter 建屏，退出=整机重启。
// 应用间借用已删除：提示音不再查询播客状态（主控架构下应用独占运行期，
// 播客 exit 时已停止播放并归还喇叭）。
#include "app_todo.h"

#include <stdio.h>

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "task_model.h"
#include "task_store.h"
#include "timer_model.h"
#include "trio_app.h"

/* 调色板：白底黑字 + 单一强调色（播放器式）。 */
#define C_BG 0xFFFFFF   /* 纯白底 */
#define C_INK 0x1F2328  /* 主文字 */
#define C_MUTED 0x8A9299 /* 次要文字 */
#define C_LINE 0xE5E1D8 /* 分隔线 */
#define C_FOCUS 0xE0533D /* 专注（番茄红） */
#define C_REST 0x2F8F5B /* 休息（绿） */
#define C_BLUE 0x1567F6 /* 强调蓝（页首标题） */
#define C_DONE 0xB9BDC2 /* 已完成任务 */

#define LIST_TOP 46u
#define ROW_H 36u
#define BAR_H 34u /* 底部操作条高度（240x320 竖屏） */
#define LIST_ROWS_VISIBLE ((320u - LIST_TOP - BAR_H - 2u) / ROW_H)

#define F_SMALL &lv_font_montserrat_14
#define F_HEAD &lv_font_montserrat_20
#define F_TITLE &lv_font_montserrat_28
#define F_BIG &lv_font_montserrat_48

/* 列表页 */
static lv_obj_t *s_scr_tasks;
static lv_obj_t *s_row_name[TASK_LIST_MAX];
static lv_obj_t *s_row_info[TASK_LIST_MAX];
static lv_obj_t *s_row_mark[TASK_LIST_MAX];
static lv_obj_t *s_tasks_bar;

/* 运行页 */
static lv_obj_t *s_scr_running;
static lv_obj_t *s_run_name;
static lv_obj_t *s_run_clock;
static lv_obj_t *s_run_round;
static lv_obj_t *s_run_bar;

/* 计时器页 */
static lv_obj_t *s_scr_timer;
static lv_obj_t *s_timer_clock;
static lv_obj_t *s_timer_hint;
static lv_obj_t *s_timer_bar;

/* 任务菜单页（长按 OK 唤出）：SUSPEND / INTERRUPT / END */
static lv_obj_t *s_scr_menu;
static lv_obj_t *s_menu_task;
static lv_obj_t *s_menu_items[3];
static uint8_t s_menu_task_idx;
static int s_menu_sel;

static uint8_t s_list_first; /* 滚动窗口首行（选中行超出可视行数时翻页） */

/* ---------- 基础构件 ---------- */

static lv_obj_t *page_create(const char *title, uint32_t head_color, const char *corner,
                             lv_obj_t **screen) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t *head = lv_label_create(scr);
    lv_label_set_text(head, title);
    lv_obj_set_style_text_font(head, F_TITLE, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(head_color), 0);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, corner);
    lv_obj_set_style_text_font(sub, F_SMALL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_MUTED), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, -14, 14);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 36);

    /* 底部白色操作条（文字由各页设置），上方保留分隔线 */
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

static void format_clock(char *out, uint32_t sec) {
    const uint32_t max = 99u * 60u + 59u;
    if (sec > max) sec = max; /* 让 gcc 确认 9 字节缓冲足够 */
    snprintf(out, 9, "%02u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

static lv_obj_t *center_label(lv_obj_t *scr, lv_coord_t y, const lv_font_t *font,
                              uint32_t color) {
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

/* ---------- 列表页 ---------- */

/* 单字母状态标记（行右侧）：P=挂起 F=专注中 R=休息 N=待下一轮；已结束=F(Finished) */
static void row_mark_for(const task_t *task, char *mark, uint32_t *color) {
    *mark = 0;
    *color = C_FOCUS;
    switch (task->state) {
    case TASK_STATE_RUNNING:
        *mark = 'F';
        break;
    case TASK_STATE_PAUSED:
    case TASK_STATE_REST_PAUSED:
        *mark = 'P';
        break;
    case TASK_STATE_RESTING:
        *mark = 'R';
        *color = C_REST;
        break;
    case TASK_STATE_READY:
        *mark = 'N';
        break;
    default:
        break;
    }
}

static void tasks_refresh(void) {
    task_list_t *list = trio_app_tasks();
    uint8_t visible = task_list_visible_count(list);
    lv_label_set_text_fmt(s_tasks_bar, "%u/%u",
                          (unsigned)(list->count - visible), (unsigned)list->count);

    /* 滚动窗口：保证选中行始终可见 */
    if (list->count <= LIST_ROWS_VISIBLE) {
        s_list_first = 0;
    } else {
        if (list->selected < s_list_first) s_list_first = list->selected;
        if (list->selected >= s_list_first + LIST_ROWS_VISIBLE) {
            s_list_first = (uint8_t)(list->selected - LIST_ROWS_VISIBLE + 1);
        }
    }

    for (uint8_t i = 0; i < TASK_LIST_MAX; ++i) {
        const bool in_window = i < list->count && i >= s_list_first &&
                               i < s_list_first + LIST_ROWS_VISIBLE;
        if (!in_window) {
            lv_label_set_text(s_row_name[i], "");
            lv_label_set_text(s_row_info[i], "");
            lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_TRANSP, 0);
            continue;
        }
        const task_t *task = task_list_get(list, i);
        const bool selected = i == list->selected;
        const lv_coord_t y = (lv_coord_t)(LIST_TOP + (i - s_list_first) * ROW_H);
        lv_obj_align(s_row_name[i], LV_ALIGN_TOP_LEFT, 24, y + 6);
        lv_obj_align(s_row_info[i], LV_ALIGN_TOP_RIGHT, -14, y + 11);
        lv_obj_align(s_row_mark[i], LV_ALIGN_TOP_LEFT, 10, y + 3);
        lv_label_set_text(s_row_name[i], task->name);
        lv_obj_set_style_text_font(s_row_name[i], F_TITLE, 0);
        if (task->ended_today) {
            lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(C_DONE), 0);
            lv_label_set_text(s_row_info[i], "F");
            lv_obj_set_style_text_color(s_row_info[i], lv_color_hex(C_FOCUS), 0);
            lv_obj_set_style_bg_color(s_row_mark[i], lv_color_hex(C_DONE), 0);
        } else {
            char mark = 0;
            uint32_t mark_color = C_FOCUS;
            row_mark_for(task, &mark, &mark_color);
            lv_obj_set_style_text_color(s_row_name[i],
                                        lv_color_hex(selected ? C_FOCUS : C_INK), 0);
            if (mark) {
                lv_label_set_text_fmt(s_row_info[i], "%c", mark);
                lv_obj_set_style_text_color(s_row_info[i], lv_color_hex(mark_color), 0);
            } else {
                lv_label_set_text(s_row_info[i], "");
            }
            lv_obj_set_style_bg_color(s_row_mark[i],
                                      lv_color_hex(selected ? C_FOCUS : C_LINE), 0);
        }
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_COVER, 0);
    }
}

/* ---------- 运行页 ---------- */

static void running_refresh(void) {
    task_list_t *list = trio_app_tasks();
    const task_t *task = task_list_get(list, list->selected);
    if (!task || task->ended_today) return;

    lv_label_set_text(s_run_name, task->name);
    lv_label_set_text_fmt(s_run_round, "ROUND %u", (unsigned)(task->rounds_done + 1));

    char clock[9];
    uint32_t color = C_INK;
    const char *bar = "PAUSE";
    switch (task->state) {
    case TASK_STATE_RESTING:
    case TASK_STATE_REST_PAUSED:
        format_clock(clock, task->remaining_sec);
        color = C_REST;
        break;
    case TASK_STATE_READY:
        format_clock(clock, TASK_FOCUS_SEC);
        color = C_MUTED;
        bar = "NEXT";
        break;
    default:
        format_clock(clock, task->remaining_sec);
        break;
    }
    lv_obj_set_style_text_color(s_run_clock, lv_color_hex(color), 0);
    lv_label_set_text(s_run_clock, clock);
    lv_label_set_text(s_run_bar, bar);
}

/* ---------- 计时器页 ---------- */

static void timer_refresh(void) {
    timer_model_t *timer = trio_app_timer();
    bool in_cycle = timer->state != TIMER_IDLE;
    char clock[9];
    format_clock(clock, in_cycle ? timer->remaining_sec : timer->preset_sec);
    lv_label_set_text(s_timer_clock, clock);
    lv_obj_set_style_text_color(s_timer_clock,
                                lv_color_hex(timer->state == TIMER_FINISHED
                                                 ? C_FOCUS
                                                 : C_INK),
                                0);
    switch (timer->state) {
    case TIMER_RUNNING:
        lv_label_set_text(s_timer_hint, "UP +5 \xC2\xB7 DOWN -5");
        lv_label_set_text(s_timer_bar, "OK PAUSE \xC2\xB7 HOLD CANCEL");
        break;
    case TIMER_PAUSED:
        lv_label_set_text(s_timer_hint, "PAUSED");
        lv_label_set_text(s_timer_bar, "OK PAUSE \xC2\xB7 HOLD CANCEL");
        break;
    case TIMER_FINISHED:
        lv_label_set_text(s_timer_hint, "TIME IS UP");
        lv_label_set_text(s_timer_bar, "OK BACK");
        break;
    default:
        lv_label_set_text(s_timer_hint, "UP +5 \xC2\xB7 DOWN -5");
        lv_label_set_text(s_timer_bar, "OK START \xC2\xB7 HOLD BACK");
        break;
    }
}

/* ---------- 任务菜单页（SUSPEND / INTERRUPT / END） ---------- */

static void menu_refresh(void) {
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_text_color(s_menu_items[i],
                                    lv_color_hex(i == s_menu_sel ? C_FOCUS : C_INK), 0);
    }
}

static void menu_open(uint8_t idx) {
    s_menu_task_idx = idx;
    s_menu_sel = 0;
    const task_t *task = task_list_get(trio_app_tasks(), idx);
    lv_label_set_text(s_menu_task, task ? task->name : "");
    menu_refresh();
    lv_screen_load(s_scr_menu);
}

static void menu_execute(void) {
    switch (s_menu_sel) {
    case 0: /* SUSPEND：暂停挂起，任务留在列表 */
        task_list_pause(trio_app_tasks(), s_menu_task_idx,
                        (uint64_t)(esp_timer_get_time() / 1000));
        task_store_request_save(trio_app_tasks());
        break;
    case 1: /* INTERRUPT：放弃本轮，任务回 IDLE 仍可见 */
        trio_app_interrupt_task(s_menu_task_idx);
        break;
    default: /* END：结束任务，今日隐藏 */
        trio_app_end_task(s_menu_task_idx);
        break;
    }
    lv_screen_load(s_scr_tasks);
    tasks_refresh();
}

/* ---------- 刷新（tick 上下文） ---------- */

static void refresh_all(void) {
    lv_obj_t *current = lv_screen_active();
    if (current == s_scr_tasks) tasks_refresh();
    else if (current == s_scr_running) running_refresh();
    else if (current == s_scr_timer) timer_refresh();
}

/* ---------- 按键 ---------- */

static void tasks_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    task_list_t *list = trio_app_tasks();
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        lv_screen_load(s_scr_timer); /* 列表页长按确定 → 独立计时器 */
        timer_refresh();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        /* 单行移动选择；越过可视窗口时自动整页翻页，到头即停不循环 */
        task_list_select(list, btn == BSP_BTN_UP ? -1 : 1);
        tasks_refresh();
    } else if (btn == BSP_BTN_OK) {
        const task_t *task = task_list_get(list, list->selected);
        bool ok = false;
        if (task && !task->ended_today) {
            switch (task->state) {
            case TASK_STATE_READY:
                trio_app_continue_task(list->selected); /* 确认开启下一轮 */
                ok = true;
                break;
            case TASK_STATE_RUNNING:
            case TASK_STATE_RESTING:
                ok = true; /* 已在进行中，直接进入任务页 */
                break;
            default:
                ok = trio_app_start_task(list->selected); /* IDLE/挂起态 */
                break;
            }
        }
        if (ok) {
            running_refresh();
            lv_screen_load(s_scr_running); /* 仅在成功开始后进入任务页 */
        }
    }
}

/* 任务页上/下键：循环背光高/中/低 与 音量大/中/小（直接生效，不显示不确认） */
static const uint8_t BRIGHT_LEVELS[] = {100, 60, 30};
static const uint8_t VOLUME_LEVELS[] = {100, 58, 25}; /* 中档对齐 rtttl 播放器初值 */
static uint8_t s_bright_idx;
static uint8_t s_volume_idx = 1;

static void running_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    task_list_t *list = trio_app_tasks();
    uint8_t idx = list->selected;
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        menu_open(idx); /* 长按：唤出 SUSPEND/INTERRUPT/END 菜单 */
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_bright_idx = (uint8_t)((s_bright_idx + 1) % (sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0])));
        bsp_display_backlight(BRIGHT_LEVELS[s_bright_idx]);
        return;
    }
    if (btn == BSP_BTN_DOWN) {
        s_volume_idx = (uint8_t)((s_volume_idx + 1) % (sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0])));
        bsp_audio_set_volume(VOLUME_LEVELS[s_volume_idx]);
        return;
    }
    if (btn != BSP_BTN_OK) return;
    const task_t *task = task_list_get(list, idx);
    if (!task) return;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    switch (task->state) {
    case TASK_STATE_RUNNING:
    case TASK_STATE_RESTING:
        task_list_pause(list, idx, now);
        break;
    case TASK_STATE_PAUSED:
    case TASK_STATE_REST_PAUSED:
        task_list_resume(list, idx, now);
        break;
    case TASK_STATE_READY:
        trio_app_continue_task(idx); /* 按键确认：开启下一轮 45 分钟专注 */
        break;
    default: break;
    }
    running_refresh();
}

static void menu_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + (btn == BSP_BTN_DOWN ? 1 : 2)) % 3;
            menu_refresh();
        } else if (btn == BSP_BTN_OK) {
            menu_execute();
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        lv_screen_load(s_scr_running); /* 取消：回任务页 */
        running_refresh();
    }
}

static void timer_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    timer_model_t *timer = trio_app_timer();
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            if (timer->state == TIMER_IDLE) {
                timer_model_select(timer, btn == BSP_BTN_UP ? 1 : -1); /* 上=加时长 */
                timer_refresh();
            }
        } else if (btn == BSP_BTN_OK) {
            switch (timer->state) {
            case TIMER_IDLE: trio_app_timer_start(); break;
            case TIMER_RUNNING: timer_model_pause(timer, now); break;
            case TIMER_PAUSED: timer_model_resume(timer, now); break;
            case TIMER_FINISHED: trio_app_timer_cancel(); break;
            default: break;
            }
            timer_refresh();
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (timer->state == TIMER_RUNNING || timer->state == TIMER_PAUSED) {
            trio_app_timer_cancel();
        }
        lv_screen_load(s_scr_tasks);
        tasks_refresh();
    }
}

/* ---------- 页面构建 ---------- */

static void tasks_page_build(void) {
    lv_obj_t *bar = page_create("TODAY", C_BLUE, "", &s_scr_tasks);
    s_tasks_bar = bar_label(bar);

    for (uint8_t i = 0; i < TASK_LIST_MAX; ++i) {
        /* 选中标记：左侧 3px 竖条；行位置由 tasks_refresh 按滚动窗口摆放 */
        s_row_mark[i] = lv_obj_create(s_scr_tasks);
        lv_obj_set_size(s_row_mark[i], 3, ROW_H - 6);
        lv_obj_set_style_radius(s_row_mark[i], 2, 0);
        lv_obj_set_style_border_width(s_row_mark[i], 0, 0);

        s_row_name[i] = center_label(s_scr_tasks, 0, F_TITLE, C_INK);
        lv_obj_set_width(s_row_name[i], 178);
        lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_DOT);
        s_row_info[i] = center_label(s_scr_tasks, 0, F_TITLE, C_FOCUS);
    }
    tasks_refresh();
}

static void running_page_build(void) {
    lv_obj_t *bar = page_create("TASK", C_INK, "", &s_scr_running);
    s_run_bar = bar_label(bar);
    lv_obj_t *focus = center_label(s_scr_running, 0, F_TITLE, C_FOCUS);
    lv_label_set_text(focus, "Focus");
    lv_obj_align(focus, LV_ALIGN_TOP_RIGHT, -14, 6);
    s_run_name = center_label(s_scr_running, 64, F_TITLE, C_INK);
    lv_obj_set_width(s_run_name, 216);
    lv_label_set_long_mode(s_run_name, LV_LABEL_LONG_DOT);
    s_run_clock = center_label(s_scr_running, 130, F_BIG, C_INK);
    s_run_round = center_label(s_scr_running, 214, F_SMALL, C_MUTED);
    running_refresh();
}

static void timer_page_build(void) {
    lv_obj_t *bar = page_create("TIMER", C_INK, "", &s_scr_timer);
    s_timer_bar = bar_label(bar);
    s_timer_clock = center_label(s_scr_timer, 110, F_BIG, C_INK);
    s_timer_hint = center_label(s_scr_timer, 200, F_SMALL, C_MUTED);
    timer_refresh();
}

static void menu_page_build(void) {
    lv_obj_t *bar = page_create("TASK MENU", C_INK, "", &s_scr_menu);
    lv_label_set_text(bar_label(bar), "OK CONFIRM \xC2\xB7 HOLD CANCEL");

    s_menu_task = center_label(s_scr_menu, 46, F_SMALL, C_MUTED);
    static const char *ITEMS[3] = {"SUSPEND", "INTERRUPT", "END"};
    for (int i = 0; i < 3; ++i) {
        s_menu_items[i] = center_label(s_scr_menu, 100 + i * 48, F_HEAD, C_INK);
        lv_label_set_text(s_menu_items[i], ITEMS[i]);
    }
    menu_refresh();
}

/* ---------- 生命周期 ---------- */

static void app_todo_enter(void) {
    s_bright_idx = 0;
    s_list_first = 0;
    s_menu_sel = 0;

    tasks_page_build();
    running_page_build();
    timer_page_build();
    menu_page_build();
    lv_screen_load(s_scr_tasks);
}

static void app_todo_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    lv_obj_t *current = lv_screen_active();
    if (current == s_scr_tasks) tasks_key(btn, ev);
    else if (current == s_scr_running) running_key(btn, ev);
    else if (current == s_scr_menu) menu_key(btn, ev);
    else if (current == s_scr_timer) timer_key(btn, ev);
}

static void app_todo_tick(void) {
    trio_app_tick();
    refresh_all();
}

const trio_app_t APP_TODO = {
    .enter = app_todo_enter,
    .key = app_todo_key,
    .tick = app_todo_tick,
};
