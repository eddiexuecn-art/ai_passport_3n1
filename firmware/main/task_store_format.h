// main/task_store_format.h —— task_list_t 与 NVS 快照 blob 的紧凑序列化（纯逻辑，可主机单测）。
// 只编码持久化字段；运行时字段（deadline_ms/round_start_ms）不写入。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "task_model.h"

#define TASK_STORE_MAGIC 0x5452494FU /* "TRIO"（小端字节序） */
#define TASK_STORE_FORMAT_VERSION 1u
#define TASK_STORE_SNAPSHOT_MAX 600u /* 序列化缓冲上限（10 任务 ~542 字节） */

#define TASK_STORE_HEADER_SIZE 18u /* magic4 + fmtver2 + modelver2 + date8 + count1 + selected1 */
#define TASK_STORE_ENTRY_SIZE 52u
#define TASK_STORE_CRC_SIZE 4u

/* count 个任务序列化后的实际长度（紧凑格式，未用槽不占空间）。 */
static inline size_t task_store_format_len(uint8_t count) {
    return (size_t)(TASK_STORE_HEADER_SIZE + TASK_STORE_ENTRY_SIZE * count +
                    TASK_STORE_CRC_SIZE);
}

/* 缓冲区建议上限（满 8 任务），恒定值。 */
size_t task_store_format_size(void);

/* 序列化任务列表。返回写入长度；缓冲不足返回 0。 */
size_t task_store_format_serialize(const task_list_t *list, uint8_t *buf, size_t cap);

/* 反序列化任务列表：只恢复持久化字段，运行时字段清零，不调用 restore 降级。
   magic/版本/长度/CRC/任务数任一校验失败返回 false。 */
bool task_store_format_deserialize(task_list_t *list, const uint8_t *buf, size_t len);
