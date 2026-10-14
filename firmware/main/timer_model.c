#include "timer_model.h"

#include <stddef.h>

static uint32_t seconds_until(uint64_t deadline_ms, uint64_t now_ms) {
    if (now_ms >= deadline_ms) return 0;
    return (uint32_t)((deadline_ms - now_ms + 999) / 1000);
}

void timer_model_defaults(timer_model_t *timer) {
    if (!timer) return;
    timer->version = TIMER_MODEL_VERSION;
    timer->state = TIMER_IDLE;
    timer->preset_sec = TIMER_DEFAULT_PRESET_SEC;
    timer->remaining_sec = timer->preset_sec;
    timer->started_at = TIMER_TIME_UNKNOWN;
    timer->ended_at = TIMER_TIME_UNKNOWN;
    timer->completed = false;
    timer->deadline_ms = 0;
}

void timer_model_restore(timer_model_t *timer) {
    if (!timer) return;
    if (timer->version != TIMER_MODEL_VERSION ||
        timer->preset_sec < TIMER_PRESET_MIN_SEC ||
        timer->preset_sec > TIMER_PRESET_MAX_SEC ||
        timer->preset_sec % TIMER_STEP_SEC != 0 ||
        timer->state > TIMER_FINISHED) {
        timer_model_defaults(timer);
        return;
    }
    if (timer->state == TIMER_RUNNING) {
        timer->state = TIMER_PAUSED; /* 断电后以暂停态恢复 */
    } else if (timer->state == TIMER_FINISHED) {
        /* 完成记录视为已导出，重新回到空闲 */
        timer->state = TIMER_IDLE;
        timer->started_at = TIMER_TIME_UNKNOWN;
        timer->ended_at = TIMER_TIME_UNKNOWN;
        timer->completed = false;
        timer->remaining_sec = timer->preset_sec;
    }
    timer->deadline_ms = 0;
    if (timer->state == TIMER_IDLE && timer->remaining_sec == 0) {
        timer->remaining_sec = timer->preset_sec;
    }
}

bool timer_model_select(timer_model_t *timer, int direction) {
    if (!timer || timer->state != TIMER_IDLE || direction == 0) return false;
    int32_t preset = (int32_t)timer->preset_sec +
                     (direction > 0 ? (int32_t)TIMER_STEP_SEC : -(int32_t)TIMER_STEP_SEC);
    if (preset < (int32_t)TIMER_PRESET_MIN_SEC) preset = (int32_t)TIMER_PRESET_MAX_SEC;
    if (preset > (int32_t)TIMER_PRESET_MAX_SEC) preset = (int32_t)TIMER_PRESET_MIN_SEC;
    timer->preset_sec = (uint32_t)preset;
    timer->remaining_sec = timer->preset_sec;
    return true;
}

bool timer_model_start(timer_model_t *timer, uint64_t now_ms, int64_t now_epoch) {
    if (!timer || timer->state != TIMER_IDLE) return false;
    timer->remaining_sec = timer->preset_sec;
    timer->started_at = now_epoch;
    timer->ended_at = TIMER_TIME_UNKNOWN;
    timer->completed = false;
    timer->deadline_ms = now_ms + (uint64_t)timer->preset_sec * 1000ULL;
    timer->state = TIMER_RUNNING;
    return true;
}

bool timer_model_pause(timer_model_t *timer, uint64_t now_ms) {
    if (!timer || timer->state != TIMER_RUNNING) return false;
    timer->remaining_sec = seconds_until(timer->deadline_ms, now_ms);
    timer->state = TIMER_PAUSED;
    timer->deadline_ms = 0;
    return true;
}

bool timer_model_resume(timer_model_t *timer, uint64_t now_ms) {
    if (!timer || timer->state != TIMER_PAUSED || timer->remaining_sec == 0) return false;
    timer->state = TIMER_RUNNING;
    timer->deadline_ms = now_ms + (uint64_t)timer->remaining_sec * 1000ULL;
    return true;
}

bool timer_model_cancel(timer_model_t *timer, int64_t now_epoch) {
    if (!timer) return false;
    if (timer->state != TIMER_RUNNING && timer->state != TIMER_PAUSED) return false;
    timer->completed = false;
    timer->ended_at = now_epoch;
    timer->state = TIMER_IDLE;
    timer->remaining_sec = timer->preset_sec;
    timer->deadline_ms = 0;
    return true;
}

timer_event_t timer_model_tick(timer_model_t *timer, uint64_t now_ms, int64_t now_epoch) {
    if (!timer || timer->state != TIMER_RUNNING) return TIMER_EVENT_NONE;
    timer->remaining_sec = seconds_until(timer->deadline_ms, now_ms);
    if (timer->remaining_sec == 0) {
        timer->state = TIMER_FINISHED;
        timer->completed = true;
        timer->ended_at = now_epoch;
        return TIMER_EVENT_COMPLETE;
    }
    return TIMER_EVENT_NONE;
}

bool timer_model_ack(timer_model_t *timer) {
    if (!timer || timer->state != TIMER_FINISHED) return false;
    timer->state = TIMER_IDLE;
    timer->started_at = TIMER_TIME_UNKNOWN;
    timer->ended_at = TIMER_TIME_UNKNOWN;
    timer->completed = false;
    timer->remaining_sec = timer->preset_sec;
    return true;
}
