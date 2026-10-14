// main/record_line.h —— 会话记录 JSONL 行构造（纯逻辑，可主机单测）。
// 每行一条记录、无换行符（调用方追加 '\n'）。ts 为 epoch 秒，未知写 -1。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECORD_LINE_MAX 256u /* 名字全转义时仍够用的单行上限 */

/* 返回写入长度（不含 NUL）；缓冲不足返回 0。 */
size_t record_line_task_start(char *buf, size_t cap, const char *name, int64_t ts);
size_t record_line_task_end(char *buf, size_t cap, const char *name, int64_t ts,
                            uint32_t active_sec, uint8_t rounds);
size_t record_line_timer_start(char *buf, size_t cap, uint16_t minutes, int64_t ts);
size_t record_line_timer_done(char *buf, size_t cap, uint16_t minutes, int64_t ts,
                              bool completed);
