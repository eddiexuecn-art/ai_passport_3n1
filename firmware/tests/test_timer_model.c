// tests/test_timer_model.c —— timer_model 纯逻辑主机测试（与 tools/validate.sh 一同运行）。
#include <assert.h>

#include "timer_model.h"

int main(void)
{
    timer_model_t timer;

    /* 默认值 */
    timer_model_defaults(&timer);
    assert(timer.version == TIMER_MODEL_VERSION);
    assert(timer.state == TIMER_IDLE);
    assert(timer.preset_sec == TIMER_DEFAULT_PRESET_SEC);
    assert(timer.remaining_sec == TIMER_DEFAULT_PRESET_SEC);
    assert(timer.started_at == TIMER_TIME_UNKNOWN);

    /* 调档：5 分钟步进，边界环绕 */
    assert(timer_model_select(&timer, 1));
    assert(timer.preset_sec == TIMER_DEFAULT_PRESET_SEC + TIMER_STEP_SEC);
    assert(timer_model_select(&timer, -1) && timer_model_select(&timer, -1));
    assert(timer.preset_sec == TIMER_DEFAULT_PRESET_SEC - TIMER_STEP_SEC);
    timer.preset_sec = TIMER_PRESET_MIN_SEC;
    assert(timer_model_select(&timer, -1));
    assert(timer.preset_sec == TIMER_PRESET_MAX_SEC);
    assert(timer_model_select(&timer, 1));
    assert(timer.preset_sec == TIMER_PRESET_MIN_SEC);
    timer_model_defaults(&timer);

    /* 运行中不可调档 */
    assert(timer_model_start(&timer, 1000, 1755028800));
    assert(timer.state == TIMER_RUNNING);
    assert(timer.started_at == 1755028800);
    assert(timer.ended_at == TIMER_TIME_UNKNOWN);
    assert(!timer_model_select(&timer, 1));

    /* 到点：FINISHED + 事件 + 封记录 */
    assert(timer_model_tick(&timer, 1000 + (uint64_t)TIMER_DEFAULT_PRESET_SEC * 1000ULL - 1,
                            1755031300) == TIMER_EVENT_NONE);
    assert(timer.remaining_sec == 1);
    assert(timer_model_tick(&timer, 1000 + (uint64_t)TIMER_DEFAULT_PRESET_SEC * 1000ULL,
                            1755031301) == TIMER_EVENT_COMPLETE);
    assert(timer.state == TIMER_FINISHED);
    assert(timer.completed);
    assert(timer.ended_at == 1755031301);

    /* 确认后回到空闲 */
    assert(timer_model_ack(&timer));
    assert(timer.state == TIMER_IDLE);
    assert(timer.started_at == TIMER_TIME_UNKNOWN);
    assert(timer.remaining_sec == timer.preset_sec);

    /* 暂停/恢复/取消 */
    assert(timer_model_start(&timer, 1000, 1755028800));
    assert(timer_model_pause(&timer, 61000));
    assert(timer.state == TIMER_PAUSED);
    assert(timer.remaining_sec == TIMER_DEFAULT_PRESET_SEC - 60);
    assert(timer_model_resume(&timer, 120000));
    assert(timer.state == TIMER_RUNNING);
    assert(timer.deadline_ms == 120000 + (uint64_t)(TIMER_DEFAULT_PRESET_SEC - 60) * 1000ULL);
    assert(timer_model_pause(&timer, 130000));
    assert(timer_model_cancel(&timer, 1755028900));
    assert(timer.state == TIMER_IDLE);
    assert(!timer.completed);
    assert(timer.ended_at == 1755028900);
    assert(timer.remaining_sec == timer.preset_sec);

    /* 断电恢复：运行中降级为暂停 */
    assert(timer_model_start(&timer, 1000, 1755028800));
    timer_model_restore(&timer);
    assert(timer.state == TIMER_PAUSED);
    assert(timer.deadline_ms == 0);

    /* 坏数据回退默认值 */
    timer.version = 99;
    timer_model_restore(&timer);
    assert(timer.state == TIMER_IDLE);
    assert(timer.preset_sec == TIMER_DEFAULT_PRESET_SEC);

    return 0;
}
