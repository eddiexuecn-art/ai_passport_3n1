// tests/test_task_model.c —— task_model 纯逻辑主机测试（与 tools/validate.sh 一同运行）。
#include <assert.h>
#include <string.h>

#include "task_model.h"

int main(void)
{
    task_list_t list;

    /* 默认值 */
    task_list_defaults(&list);
    assert(list.version == TASK_MODEL_VERSION);
    assert(list.count == 0);
    assert(list.date[0] == '\0');

    /* 添加与上限 */
    assert(task_list_add(&list, "Reading"));
    assert(task_list_add(&list, "Coding"));
    assert(task_list_add(&list, "Workout"));
    assert(list.count == 3);
    assert(!task_list_add(&list, ""));
    assert(!task_list_add(NULL, "X"));
    for (int i = 3; i < (int)TASK_LIST_MAX; ++i) assert(task_list_add(&list, "T"));
    assert(!task_list_add(&list, "Overflow"));
    assert(list.count == TASK_LIST_MAX);
    while (list.count > 3) task_list_remove(&list, list.count - 1);

    /* 首次同步日期不算隔天 */
    assert(!task_list_needs_rollover(&list, "20260813"));
    task_list_apply_rollover(&list, "20260813");
    assert(!task_list_needs_rollover(&list, "20260813"));
    assert(task_list_needs_rollover(&list, "20260814"));

    /* 开始：单任务专注，自动暂停他人 */
    assert(task_list_start(&list, 0, 1000, 1755028800));
    const task_t *t0 = task_list_get(&list, 0);
    assert(t0->state == TASK_STATE_RUNNING);
    assert(t0->remaining_sec == TASK_FOCUS_SEC);
    assert(t0->started_at == 1755028800);
    assert(t0->ended_at == TASK_TIME_UNKNOWN);
    assert(task_list_start(&list, 1, 2000, 1755028800));
    assert(t0->state == TASK_STATE_PAUSED);
    const task_t *t1 = task_list_get(&list, 1);
    assert(t1->state == TASK_STATE_RUNNING);

    /* 结束中的任务不能再开始 */
    assert(!task_list_start(&list, 1, 3000, 1755028800));

    /* 暂停累计 active_sec，恢复重挂 deadline（起点 2000ms，暂停于 61000ms = 59s） */
    assert(task_list_pause(&list, 1, 61000));
    assert(t1->state == TASK_STATE_PAUSED);
    assert(t1->active_sec == 59);
    assert(t1->remaining_sec == TASK_FOCUS_SEC - 59);
    assert(task_list_resume(&list, 1, 120000));
    assert(t1->state == TASK_STATE_RUNNING);
    assert(t1->deadline_ms == 120000 + (uint64_t)(TASK_FOCUS_SEC - 59) * 1000ULL);

    /* 专注到点：事件 + 进入休息 */
    assert(task_list_tick(&list, 1, 120000 + (uint64_t)(TASK_FOCUS_SEC - 59) * 1000ULL - 1)
           == TASK_EVENT_NONE);
    assert(t1->remaining_sec == 1);
    assert(task_list_tick(&list, 1, 120000 + (uint64_t)(TASK_FOCUS_SEC - 59) * 1000ULL)
           == TASK_EVENT_FOCUS_COMPLETE);
    assert(t1->state == TASK_STATE_RESTING);
    assert(t1->remaining_sec == TASK_REST_SEC);
    assert(t1->rounds_done == 1);
    assert(t1->active_sec == TASK_FOCUS_SEC); /* 59 + 2641 */

    /* 休息可暂停/恢复（锚点随 TASK_FOCUS_SEC 推导，不依赖具体时长） */
    const uint64_t rest_start = 120000u + (uint64_t)(TASK_FOCUS_SEC - 59) * 1000ULL;
    assert(task_list_pause(&list, 1, rest_start + 1000));
    assert(t1->state == TASK_STATE_REST_PAUSED);
    assert(t1->remaining_sec == TASK_REST_SEC - 1);
    assert(task_list_resume(&list, 1, rest_start + 9000));
    assert(t1->state == TASK_STATE_RESTING);

    /* 休息到点：事件 + 进入 READY 等待态（不自动开下一轮） */
    const uint64_t rest_end = rest_start + 9000 + (uint64_t)(TASK_REST_SEC - 1) * 1000ULL;
    assert(task_list_tick(&list, 1, rest_end - 1) == TASK_EVENT_NONE);
    assert(task_list_tick(&list, 1, rest_end) == TASK_EVENT_REST_COMPLETE);
    assert(t1->state == TASK_STATE_READY);
    assert(t1->remaining_sec == 0);
    assert(t1->rounds_done == 1);

    /* 等待态下 resume/start 无效，必须按键确认 */
    assert(!task_list_resume(&list, 1, rest_end + 1000));
    assert(!task_list_start(&list, 1, rest_end + 1000, 1755032400));
    assert(t1->state == TASK_STATE_READY);
    assert(task_list_continue(&list, 1, rest_end + 30000));
    assert(t1->state == TASK_STATE_RUNNING);
    assert(t1->remaining_sec == TASK_FOCUS_SEC);
    assert(t1->deadline_ms == rest_end + 30000 + (uint64_t)TASK_FOCUS_SEC * 1000ULL);
    /* 非等待态不能确认 */
    assert(!task_list_continue(&list, 1, rest_end + 30001));

    /* 手动结束：隐藏并封记录，不可再启动 */
    int64_t end_epoch = 1755032400;
    assert(task_list_end(&list, 1, rest_end + 49000, end_epoch));
    assert(t1->ended_today);
    assert(t1->ended_at == end_epoch);
    assert(t1->state == TASK_STATE_IDLE);
    assert(t1->active_sec == TASK_FOCUS_SEC + 19); /* 最后一轮 19 秒 */
    assert(!task_list_start(&list, 1, 700000, end_epoch));
    assert(task_list_visible_count(&list) == 2);

    /* 光标在可见任务间移动，跳过已结束；到尾部即停不循环 */
    assert(task_list_select(&list, 1));
    assert(list.selected == 2);
    assert(!task_list_select(&list, 1));
    assert(list.selected == 2);
    /* 多步选择（整页翻页）回到列表头 */
    assert(task_list_select(&list, -6));
    assert(list.selected == 0);

    /* 隔天恢复：全部重置 */
    assert(task_list_needs_rollover(&list, "20260814"));
    task_list_apply_rollover(&list, "20260814");
    assert(strcmp(list.date, "20260814") == 0);
    assert(task_list_visible_count(&list) == 3);
    assert(!t1->ended_today);
    assert(t1->started_at == TASK_TIME_UNKNOWN);
    assert(t1->rounds_done == 0);

    /* 中断本轮：回 IDLE 但今日仍可见，累计保留，可重新开始 */
    task_list_t n;
    task_list_defaults(&n);
    assert(task_list_add(&n, "Inter"));
    assert(task_list_start(&n, 0, 1000, 1755028800));
    const task_t *tn = task_list_get(&n, 0);
    assert(task_list_pause(&n, 0, 31000));
    assert(tn->active_sec == 30);
    assert(task_list_interrupt(&n, 0, 31000));
    assert(tn->state == TASK_STATE_IDLE);
    assert(tn->remaining_sec == 0);
    assert(tn->active_sec == 30);          /* 累计保留 */
    assert(!tn->ended_today);              /* 今日仍可见 */
    assert(tn->started_at == 1755028800);  /* 今日已开始过的记录保留 */
    assert(task_list_visible_count(&n) == 1);
    assert(task_list_start(&n, 0, 32000, 1755028800)); /* 可重新开始 */
    assert(tn->state == TASK_STATE_RUNNING);
    assert(!task_list_interrupt(&n, 5, 33000));   /* 越界 */
    assert(task_list_end(&n, 0, 33000, 1755032400));
    assert(!task_list_interrupt(&n, 0, 33000));   /* 已结束不可中断 */

    /* 断电恢复：运行中降级为暂停，READY 等待态原样保留 */
    assert(task_list_start(&list, 2, 1000, 1755115200));
    const task_t *t2 = task_list_get(&list, 2);
    assert(t2->state == TASK_STATE_RUNNING);
    list.tasks[0].state = TASK_STATE_READY;
    list.version = TASK_MODEL_VERSION;
    task_list_restore(&list);
    assert(t2->state == TASK_STATE_PAUSED);
    assert(t2->deadline_ms == 0);
    assert(t0->state == TASK_STATE_READY);

    /* 时间未知时开始：记录保持 UNKNOWN */
    task_list_t u;
    task_list_defaults(&u);
    assert(task_list_add(&u, "Solo"));
    assert(task_list_start(&u, 0, 1000, TASK_TIME_UNKNOWN));
    assert(task_list_get(&u, 0)->started_at == TASK_TIME_UNKNOWN);

    /* 移除任务后索引收紧 */
    assert(task_list_remove(&u, 0));
    assert(u.count == 0);
    assert(task_list_get(&u, 0) == NULL);

    return 0;
}
