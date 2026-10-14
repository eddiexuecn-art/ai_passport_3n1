#include "task_model.h"

#include <stddef.h>
#include <string.h>

static uint32_t seconds_until(uint64_t deadline_ms, uint64_t now_ms) {
    if (now_ms >= deadline_ms) return 0;
    return (uint32_t)((deadline_ms - now_ms + 999) / 1000);
}

/* 仅在 RUNNING 态累计专注时长；round_start_ms 保持在最后一次累计点。 */
static void accumulate_running(task_t *task, uint64_t now_ms) {
    if (task->state != TASK_STATE_RUNNING) return;
    if (now_ms > task->round_start_ms) {
        task->active_sec += (uint32_t)((now_ms - task->round_start_ms) / 1000ULL);
    }
    task->round_start_ms = now_ms;
}

static void copy_bounded(char *dst, const char *src, size_t cap) {
    size_t i;
    for (i = 0; i + 1 < cap && src != NULL && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void task_list_defaults(task_list_t *list) {
    if (!list) return;
    memset(list, 0, sizeof(*list));
    list->version = TASK_MODEL_VERSION;
}

void task_list_restore(task_list_t *list) {
    if (!list) return;
    if (list->version != TASK_MODEL_VERSION || list->count > TASK_LIST_MAX) {
        task_list_defaults(list);
        return;
    }
    for (uint8_t i = 0; i < list->count; ++i) {
        task_t *task = &list->tasks[i];
        task->name[TASK_NAME_MAX - 1] = '\0';
        if (task->state == TASK_STATE_RUNNING) task->state = TASK_STATE_PAUSED;
        else if (task->state == TASK_STATE_RESTING) task->state = TASK_STATE_REST_PAUSED;
        else if (task->state > TASK_STATE_READY) task->state = TASK_STATE_IDLE;
        task->deadline_ms = 0;
        task->round_start_ms = 0;
    }
    if (list->selected >= list->count) list->selected = 0;
}

bool task_list_needs_rollover(const task_list_t *list, const char *date) {
    if (!list || !date || date[0] == '\0') return false;
    if (list->date[0] == '\0') return false; /* 首次同步：同日，仅记录日期 */
    return strncmp(list->date, date, sizeof(list->date) - 1) != 0;
}

void task_list_apply_rollover(task_list_t *list, const char *date) {
    if (!list) return;
    for (uint8_t i = 0; i < list->count; ++i) {
        task_t *task = &list->tasks[i];
        task->state = TASK_STATE_IDLE;
        task->remaining_sec = 0;
        task->rounds_done = 0;
        task->ended_today = false;
        task->started_at = TASK_TIME_UNKNOWN;
        task->ended_at = TASK_TIME_UNKNOWN;
        task->active_sec = 0;
        task->deadline_ms = 0;
        task->round_start_ms = 0;
    }
    list->selected = 0;
    if (date) copy_bounded(list->date, date, sizeof(list->date));
}

bool task_list_add(task_list_t *list, const char *name) {
    if (!list || !name || name[0] == '\0' || list->count >= TASK_LIST_MAX) return false;
    task_t *task = &list->tasks[list->count];
    memset(task, 0, sizeof(*task));
    copy_bounded(task->name, name, sizeof(task->name));
    task->started_at = TASK_TIME_UNKNOWN;
    task->ended_at = TASK_TIME_UNKNOWN;
    list->count++;
    return true;
}

bool task_list_remove(task_list_t *list, uint8_t index) {
    if (!list || index >= list->count) return false;
    for (uint8_t i = index; i + 1 < list->count; ++i) {
        list->tasks[i] = list->tasks[i + 1];
    }
    list->count--;
    memset(&list->tasks[list->count], 0, sizeof(task_t));
    if (list->selected >= list->count) list->selected = 0;
    return true;
}

bool task_list_rename(task_list_t *list, uint8_t index, const char *name) {
    if (!list || index >= list->count || !name || name[0] == '\0') return false;
    copy_bounded(list->tasks[index].name, name, sizeof(list->tasks[index].name));
    return true;
}

bool task_list_start(task_list_t *list, uint8_t index, uint64_t now_ms, int64_t now_epoch) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->ended_today || task->state == TASK_STATE_RUNNING ||
        task->state == TASK_STATE_RESTING || task->state == TASK_STATE_READY) {
        return false; /* 进行中无需重复开始；READY 须走 task_list_continue 确认 */
    }
    /* 单任务专注：自动暂停其他进行中的任务 */
    for (uint8_t i = 0; i < list->count; ++i) {
        if (i != index) task_list_pause(list, i, now_ms);
    }
    /* 挂起中的任务：恢复计时而不是重开一轮 */
    if (task->state == TASK_STATE_PAUSED || task->state == TASK_STATE_REST_PAUSED) {
        return task_list_resume(list, index, now_ms);
    }
    if (task->started_at == TASK_TIME_UNKNOWN) task->started_at = now_epoch;
    task->remaining_sec = TASK_FOCUS_SEC;
    task->deadline_ms = now_ms + (uint64_t)TASK_FOCUS_SEC * 1000ULL;
    task->round_start_ms = now_ms;
    task->state = TASK_STATE_RUNNING;
    return true;
}

bool task_list_pause(task_list_t *list, uint8_t index, uint64_t now_ms) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->state == TASK_STATE_RUNNING) {
        task->remaining_sec = seconds_until(task->deadline_ms, now_ms);
        accumulate_running(task, now_ms);
        task->state = TASK_STATE_PAUSED;
        task->deadline_ms = 0;
        return true;
    }
    if (task->state == TASK_STATE_RESTING) {
        task->remaining_sec = seconds_until(task->deadline_ms, now_ms);
        task->state = TASK_STATE_REST_PAUSED;
        task->deadline_ms = 0;
        return true;
    }
    return false;
}

bool task_list_resume(task_list_t *list, uint8_t index, uint64_t now_ms) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->state == TASK_STATE_PAUSED && task->remaining_sec > 0) {
        task->state = TASK_STATE_RUNNING;
        task->deadline_ms = now_ms + (uint64_t)task->remaining_sec * 1000ULL;
        task->round_start_ms = now_ms;
        return true;
    }
    if (task->state == TASK_STATE_REST_PAUSED && task->remaining_sec > 0) {
        task->state = TASK_STATE_RESTING;
        task->deadline_ms = now_ms + (uint64_t)task->remaining_sec * 1000ULL;
        return true;
    }
    return false;
}

/* 休息到点后由按键调用：开启下一轮全新专注倒计时。 */
bool task_list_continue(task_list_t *list, uint8_t index, uint64_t now_ms) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->ended_today || task->state != TASK_STATE_READY) return false;
    /* 单任务专注：自动暂停其他进行中的任务（与 task_list_start 一致） */
    for (uint8_t i = 0; i < list->count; ++i) {
        if (i != index) task_list_pause(list, i, now_ms);
    }
    task->remaining_sec = TASK_FOCUS_SEC;
    task->deadline_ms = now_ms + (uint64_t)TASK_FOCUS_SEC * 1000ULL;
    task->round_start_ms = now_ms;
    task->state = TASK_STATE_RUNNING;
    return true;
}

bool task_list_end(task_list_t *list, uint8_t index, uint64_t now_ms, int64_t now_epoch) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->state == TASK_STATE_IDLE || task->ended_today) return false;
    accumulate_running(task, now_ms);
    task->ended_today = true;
    task->ended_at = now_epoch;
    task->state = TASK_STATE_IDLE;
    task->remaining_sec = 0;
    task->deadline_ms = 0;
    task->round_start_ms = 0;
    return true;
}

bool task_list_interrupt(task_list_t *list, uint8_t index, uint64_t now_ms) {
    if (!list || index >= list->count) return false;
    task_t *task = &list->tasks[index];
    if (task->state == TASK_STATE_IDLE || task->ended_today) return false;
    accumulate_running(task, now_ms);
    task->state = TASK_STATE_IDLE;
    task->remaining_sec = 0;
    task->deadline_ms = 0;
    task->round_start_ms = 0;
    return true;
}

task_event_t task_list_tick(task_list_t *list, uint8_t index, uint64_t now_ms) {
    if (!list || index >= list->count) return TASK_EVENT_NONE;
    task_t *task = &list->tasks[index];
    if (task->state == TASK_STATE_RUNNING) {
        task->remaining_sec = seconds_until(task->deadline_ms, now_ms);
        if (task->remaining_sec == 0) {
            accumulate_running(task, now_ms);
            task->rounds_done++;
            task->state = TASK_STATE_RESTING;
            task->remaining_sec = TASK_REST_SEC;
            task->deadline_ms = now_ms + (uint64_t)TASK_REST_SEC * 1000ULL;
            return TASK_EVENT_FOCUS_COMPLETE;
        }
    } else if (task->state == TASK_STATE_RESTING) {
        task->remaining_sec = seconds_until(task->deadline_ms, now_ms);
        if (task->remaining_sec == 0) {
            task->state = TASK_STATE_READY;
            task->remaining_sec = 0;
            task->deadline_ms = 0;
            return TASK_EVENT_REST_COMPLETE;
        }
    }
    return TASK_EVENT_NONE;
}

const task_t *task_list_get(const task_list_t *list, uint8_t index) {
    if (!list || index >= list->count) return NULL;
    return &list->tasks[index];
}

uint8_t task_list_visible_count(const task_list_t *list) {
    if (!list) return 0;
    uint8_t visible = 0;
    for (uint8_t i = 0; i < list->count; ++i) {
        if (!list->tasks[i].ended_today) visible++;
    }
    return visible;
}

bool task_list_select(task_list_t *list, int direction) {
    if (!list || list->count == 0 || direction == 0) return false;
    if (task_list_visible_count(list) == 0) return false;
    if (list->selected >= list->count) list->selected = 0;
    const int step = direction > 0 ? 1 : -1;
    int remaining = direction > 0 ? direction : -direction;
    int index = list->selected;
    bool moved = false;
    while (remaining-- > 0) {
        /* 跳过已结束任务；到列表头/尾即停，不循环 */
        int next = index + step;
        while (next >= 0 && next < list->count && list->tasks[next].ended_today) {
            next += step;
        }
        if (next < 0 || next >= list->count) break;
        index = next;
        moved = true;
    }
    if (!moved || index == list->selected) return false;
    list->selected = (uint8_t)index;
    return true;
}
