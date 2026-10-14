// tests/test_task_store_format.c —— 任务快照序列化主机测试（与 tools/validate.sh 一同运行）。
#include <assert.h>
#include <string.h>

#include "task_store_format.h"

static void fill_demo(task_list_t *list)
{
    task_list_defaults(list);
    strcpy(list->date, "20260813");
    list->version = TASK_MODEL_VERSION;
    assert(task_list_add(list, "Reading"));
    assert(task_list_add(list, "Coding \"C++\" \\x\n"));
    assert(task_list_add(list, "Workout"));
    list->selected = 1;

    /* 任务 0：进行中专注，运行时字段有值（不应入 blob） */
    task_t *t0 = &list->tasks[0];
    assert(task_list_start(list, 0, 1000, 1755028800));
    t0->deadline_ms = 99999;
    t0->round_start_ms = 88888;

    /* 任务 1：已结束隐藏，含极值记录字段 */
    task_t *t1 = &list->tasks[1];
    t1->state = TASK_STATE_IDLE;
    t1->ended_today = true;
    t1->started_at = 1755028800;
    t1->ended_at = 1755032400;
    t1->active_sec = 0xFFFFFFFFU;
    t1->rounds_done = 255;
    t1->remaining_sec = 4294967295U;

    /* 任务 2：休息结束等待确认态 */
    task_t *t2 = &list->tasks[2];
    t2->state = TASK_STATE_READY;
    t2->remaining_sec = 0;
}

int main(void)
{
    static uint8_t buf[TASK_STORE_SNAPSHOT_MAX];
    task_list_t list;

    /* 缓冲不足 / 非法参数 */
    fill_demo(&list);
    assert(task_store_format_serialize(&list, buf, task_store_format_len(3) - 1) == 0);
    assert(task_store_format_serialize(NULL, buf, sizeof(buf)) == 0);

    /* 序列化 → 反序列化往返 */
    size_t len = task_store_format_serialize(&list, buf, sizeof(buf));
    assert(len == task_store_format_len(3)); /* 紧凑格式：只占 3 个任务槽 */
    assert(len <= TASK_STORE_SNAPSHOT_MAX);

    task_list_t out;
    assert(task_store_format_deserialize(&out, buf, len));
    assert(out.version == TASK_MODEL_VERSION);
    assert(strcmp(out.date, "20260813") == 0);
    assert(out.count == 3);
    assert(out.selected == 1);
    assert(strcmp(out.tasks[0].name, "Reading") == 0);
    assert(out.tasks[0].state == TASK_STATE_RUNNING);
    assert(out.tasks[0].started_at == 1755028800);
    assert(out.tasks[0].ended_at == TASK_TIME_UNKNOWN);
    assert(out.tasks[0].remaining_sec == TASK_FOCUS_SEC);
    assert(out.tasks[0].deadline_ms == 0); /* 运行时字段不入 blob */
    assert(out.tasks[0].round_start_ms == 0);
    assert(strcmp(out.tasks[1].name, "Coding \"C++\" \\x\n") == 0);
    assert(out.tasks[1].ended_today);
    assert(out.tasks[1].ended_at == 1755032400);
    assert(out.tasks[1].active_sec == 0xFFFFFFFFU);
    assert(out.tasks[1].rounds_done == 255);
    assert(out.tasks[1].remaining_sec == 4294967295U);
    assert(out.tasks[2].state == TASK_STATE_READY);

    /* selected 越界被钳制到 0 */
    list.selected = 200;
    len = task_store_format_serialize(&list, buf, sizeof(buf));
    assert(task_store_format_deserialize(&out, buf, len));
    assert(out.selected == 0);

    /* 损坏检测：magic / payload / 长度 */
    fill_demo(&list);
    len = task_store_format_serialize(&list, buf, sizeof(buf));
    buf[0] ^= 0xFFU;
    assert(!task_store_format_deserialize(&out, buf, len));
    buf[0] ^= 0xFFU;
    buf[20] ^= 0x01U; /* 任务条目内单比特翻转 → CRC 失败 */
    assert(!task_store_format_deserialize(&out, buf, len));
    assert(!task_store_format_deserialize(&out, buf, len - 1));
    assert(!task_store_format_deserialize(&out, buf, len + 1));
    assert(!task_store_format_deserialize(NULL, buf, len));

    /* 满 8 任务往返，长名字截断语义与模型一致 */
    task_list_defaults(&list);
    assert(task_list_add(&list, "12345678901234567890123"));
    for (int i = 1; i < (int)TASK_LIST_MAX; ++i) assert(task_list_add(&list, "T"));
    len = task_store_format_serialize(&list, buf, sizeof(buf));
    assert(task_store_format_deserialize(&out, buf, len));
    assert(out.count == TASK_LIST_MAX);
    assert(strcmp(out.tasks[0].name, "12345678901234567890123") == 0);

    return 0;
}
