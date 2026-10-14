// main/timer_model.h —— 独立倒计时纯逻辑（与 UI/硬件无关，可主机单测）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TIMER_MODEL_VERSION 1u
#define TIMER_STEP_SEC (5u * 60u)      /* 档位步长：5 分钟 */
#define TIMER_PRESET_MIN_SEC (5u * 60u)
#define TIMER_PRESET_MAX_SEC (60u * 60u)
#define TIMER_DEFAULT_PRESET_SEC (25u * 60u)
#define TIMER_TIME_UNKNOWN ((int64_t)-1)

typedef enum {
    TIMER_IDLE = 0,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_FINISHED,
} timer_state_t;

typedef enum {
    TIMER_EVENT_NONE = 0,
    TIMER_EVENT_COMPLETE, /* 倒计时到点：提示音 */
} timer_event_t;

typedef struct {
    uint16_t version;
    timer_state_t state;
    uint32_t preset_sec;  /* 预设时长，TIMER_STEP_SEC 的整数倍 */
    uint32_t remaining_sec;
    int64_t started_at;   /* epoch 秒；未知为 TIMER_TIME_UNKNOWN */
    int64_t ended_at;     /* 完成/取消时刻；进行中为 TIMER_TIME_UNKNOWN */
    bool completed;       /* true=自然到点，false=手动取消 */
    /* 运行时字段，不写入存储： */
    uint64_t deadline_ms;
} timer_model_t;

void timer_model_defaults(timer_model_t *timer);
void timer_model_restore(timer_model_t *timer);

/* 仅空闲态可调档；超界环绕。 */
bool timer_model_select(timer_model_t *timer, int direction);
bool timer_model_start(timer_model_t *timer, uint64_t now_ms, int64_t now_epoch);
bool timer_model_pause(timer_model_t *timer, uint64_t now_ms);
bool timer_model_resume(timer_model_t *timer, uint64_t now_ms);
/* 手动取消也封一条记录（completed=false）。 */
bool timer_model_cancel(timer_model_t *timer, int64_t now_epoch);
timer_event_t timer_model_tick(timer_model_t *timer, uint64_t now_ms, int64_t now_epoch);
/* FINISHED 确认后回到 IDLE（应用层先导出记录）。 */
bool timer_model_ack(timer_model_t *timer);
