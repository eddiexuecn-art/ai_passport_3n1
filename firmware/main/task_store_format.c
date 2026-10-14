#include "task_store_format.h"

#include <stddef.h>
#include <string.h>

/* 布局（小端固定宽度，紧凑：只写 count 个任务槽）：
 * 头 18 字节：u32 magic + u16 format_version + u16 model_version + date[8]
 *            + u8 count + u8 selected
 * 每任务 52 字节：name[24] + u8 state + u8 rounds_done + u8 ended_today + u8 pad
 *                + u32 remaining_sec + i64 started_at + i64 ended_at + u32 active_sec
 * 尾 4 字节：u32 crc32（覆盖之前全部字节） */

size_t task_store_format_size(void) {
    return task_store_format_len(TASK_LIST_MAX);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static void put_i64(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((u >> (8 * i)) & 0xFFU);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int64_t get_i64(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | p[i];
    return (int64_t)u;
}

/* 与 pomodoro demo 相同的 CRC32（IEEE 反射多项式）。 */
static uint32_t crc32_bytes(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

size_t task_store_format_serialize(const task_list_t *list, uint8_t *buf, size_t cap) {
    if (!list || !buf) return 0;
    if (list->count > TASK_LIST_MAX) return 0;
    if (cap < task_store_format_len(list->count)) return 0;

    uint8_t *p = buf;
    put_u32(p, TASK_STORE_MAGIC);
    put_u16(p + 4, TASK_STORE_FORMAT_VERSION);
    put_u16(p + 6, list->version);
    memset(p + 8, 0, 8);
    memcpy(p + 8, list->date, sizeof(list->date) - 1 < 8 ? sizeof(list->date) - 1 : 8);
    p[16] = list->count;
    p[17] = list->selected;
    p += TASK_STORE_HEADER_SIZE;

    for (uint8_t i = 0; i < list->count; ++i) {
        const task_t *task = &list->tasks[i];
        memset(p, 0, TASK_STORE_ENTRY_SIZE);
        memcpy(p, task->name, sizeof(task->name));
        p[24] = (uint8_t)task->state;
        p[25] = task->rounds_done;
        p[26] = task->ended_today ? 1u : 0u;
        put_u32(p + 28, task->remaining_sec);
        put_i64(p + 32, task->started_at);
        put_i64(p + 40, task->ended_at);
        put_u32(p + 48, task->active_sec);
        p += TASK_STORE_ENTRY_SIZE;
    }

    size_t payload = (size_t)(p - buf);
    put_u32(p, crc32_bytes(buf, payload));
    return payload + TASK_STORE_CRC_SIZE;
}

bool task_store_format_deserialize(task_list_t *list, const uint8_t *buf, size_t len) {
    if (!list || !buf) return false;
    if (len < TASK_STORE_HEADER_SIZE + TASK_STORE_CRC_SIZE) return false;
    if (get_u32(buf) != TASK_STORE_MAGIC) return false;
    if (get_u16(buf + 4) != TASK_STORE_FORMAT_VERSION) return false;

    uint8_t count = buf[16];
    if (count > TASK_LIST_MAX) return false;
    if (len != task_store_format_len(count)) return false;

    size_t payload = len - TASK_STORE_CRC_SIZE;
    if (get_u32(buf + payload) != crc32_bytes(buf, payload)) return false;

    task_list_t out;
    memset(&out, 0, sizeof(out));
    out.version = get_u16(buf + 6);
    memcpy(out.date, buf + 8, sizeof(out.date) - 1);
    out.date[sizeof(out.date) - 1] = '\0';
    out.count = count;
    out.selected = buf[17] < count ? buf[17] : 0;

    const uint8_t *p = buf + TASK_STORE_HEADER_SIZE;
    for (uint8_t i = 0; i < count; ++i) {
        task_t *task = &out.tasks[i];
        memcpy(task->name, p, sizeof(task->name));
        task->name[sizeof(task->name) - 1] = '\0';
        task->state = (task_state_t)p[24];
        task->rounds_done = p[25];
        task->ended_today = p[26] != 0u;
        task->remaining_sec = get_u32(p + 28);
        task->started_at = get_i64(p + 32);
        task->ended_at = get_i64(p + 40);
        task->active_sec = get_u32(p + 48);
        /* deadline_ms/round_start_ms 保持 0，由 task_list_restore 语义兜底 */
        p += TASK_STORE_ENTRY_SIZE;
    }
    *list = out;
    return true;
}
