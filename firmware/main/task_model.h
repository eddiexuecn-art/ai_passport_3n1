// main/task_model.h —— 每日任务 + 番茄钟纯逻辑（与 UI/硬件无关，可主机单测）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TASK_MODEL_VERSION 1u
#define TASK_FOCUS_SEC (5u * 60u) /* 单轮专注时长（临时测试值，正式恢复 45u*60u） */
#define TASK_REST_SEC (5u * 60u)   /* 休息时长 */
#define TASK_LIST_MAX 10u          /* 任务数量上限 */
#define TASK_NAME_MAX 24u          /* 名称长度上限（字节，含 NUL） */
#define TASK_TIME_UNKNOWN ((int64_t)-1) /* 未知 epoch：时间尚未经 SNTP 同步 */

typedef enum {
    TASK_STATE_IDLE = 0,
    TASK_STATE_RUNNING,
    TASK_STATE_PAUSED,
    TASK_STATE_RESTING,
    TASK_STATE_REST_PAUSED,
    TASK_STATE_READY, /* 休息结束：等待按键确认开启下一轮 */
} task_state_t;

typedef enum {
    TASK_EVENT_NONE = 0,
    TASK_EVENT_FOCUS_COMPLETE, /* 专注到点：提示休息 */
    TASK_EVENT_REST_COMPLETE,  /* 休息到点：提示继续，等待按键确认开下一轮 */
} task_event_t;

typedef struct {
    char name[TASK_NAME_MAX];
    task_state_t state;
    uint32_t remaining_sec; /* 当前倒计时（专注或休息，按状态解释） */
    uint8_t rounds_done;    /* 今日已完成专注轮数 */
    bool ended_today;       /* 今日已手动结束：隐藏，隔天自动恢复 */
    int64_t started_at;     /* 今日首次开始时刻（epoch 秒） */
    int64_t ended_at;       /* 结束时刻；进行中为 TASK_TIME_UNKNOWN */
    uint32_t active_sec;    /* 今日累计专注秒数（不含暂停/休息） */
    /* 运行时字段，不写入存储： */
    uint64_t deadline_ms;
    uint64_t round_start_ms;
} task_t;

typedef struct {
    uint16_t version;
    char date[9]; /* "YYYYMMDD"，空串 = 日期未知（未同步过时间） */
    uint8_t count;
    uint8_t selected;
    task_t tasks[TASK_LIST_MAX];
} task_list_t;

void task_list_defaults(task_list_t *list);
/* 断电恢复：运行中的专注/休息一律降级为暂停，运行时字段清零。 */
void task_list_restore(task_list_t *list);

/* 隔天恢复：日期变更判定与重置。记录导出由应用层在两步之间完成。 */
bool task_list_needs_rollover(const task_list_t *list, const char *date);
void task_list_apply_rollover(task_list_t *list, const char *date);

bool task_list_add(task_list_t *list, const char *name);
bool task_list_remove(task_list_t *list, uint8_t index);
bool task_list_rename(task_list_t *list, uint8_t index, const char *name);

/* 同一时刻只允许一个任务处于专注/休息：start 会自动暂停其他进行中的任务。 */
bool task_list_start(task_list_t *list, uint8_t index, uint64_t now_ms, int64_t now_epoch);
bool task_list_pause(task_list_t *list, uint8_t index, uint64_t now_ms);
bool task_list_resume(task_list_t *list, uint8_t index, uint64_t now_ms);
/* 休息结束后的按键确认：开启下一轮 45 分钟专注（同样会暂停其他进行中的任务）。 */
bool task_list_continue(task_list_t *list, uint8_t index, uint64_t now_ms);
/* 手动结束：任务今日隐藏，记录封口（ended_at）。 */
bool task_list_end(task_list_t *list, uint8_t index, uint64_t now_ms, int64_t now_epoch);
/* 中断本轮：丢弃本轮剩余倒计时，任务回到 IDLE 且今日仍可见（与 end 不同：
   不设 ended_today，之后可重新 start，今日累计 active_sec/轮数保留）。 */
bool task_list_interrupt(task_list_t *list, uint8_t index, uint64_t now_ms);

task_event_t task_list_tick(task_list_t *list, uint8_t index, uint64_t now_ms);

const task_t *task_list_get(const task_list_t *list, uint8_t index);
/* 选择光标只在可见（未结束）任务间移动。 */
bool task_list_select(task_list_t *list, int direction);
uint8_t task_list_visible_count(const task_list_t *list);
