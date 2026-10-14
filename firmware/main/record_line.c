#include "record_line.h"

#include <stdarg.h>
#include <stdio.h>

/* 有界写缓冲：任何一次写入被截断即整体失败（返回 0），保证行完整有效。 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} wbuf_t;

static void wb_puts(wbuf_t *w, const char *s) {
    if (!w->ok) return;
    while (*s != '\0') {
        if (w->len + 2 >= w->cap) {
            w->ok = false;
            return;
        }
        w->buf[w->len++] = *s++;
    }
}

/* 带引号的 JSON 字符串：转义 " \ 与控制字符；>=0x7F 的 UTF-8 字节原样直通。 */
static void wb_put_json_str(wbuf_t *w, const char *s) {
    if (!w->ok) return;
    wb_puts(w, "\"");
    for (const char *p = s != NULL ? s : ""; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        const char *esc = NULL;
        char tmp[8];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20U) {
                snprintf(tmp, sizeof(tmp), "\\u%04X", (unsigned)c);
                esc = tmp;
            }
            break;
        }
        if (esc != NULL) {
            wb_puts(w, esc);
        } else {
            if (w->len + 2 >= w->cap) {
                w->ok = false;
                return;
            }
            w->buf[w->len++] = (char)c;
        }
    }
    wb_puts(w, "\"");
}

static void wb_printf(wbuf_t *w, const char *fmt, ...) {
    if (!w->ok) return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->cap - w->len) {
        w->ok = false;
        return;
    }
    w->len += (size_t)n;
}

static size_t wb_finish(wbuf_t *w) {
    if (!w->ok) return 0;
    w->buf[w->len] = '\0';
    return w->len;
}

size_t record_line_task_start(char *buf, size_t cap, const char *name, int64_t ts) {
    if (!buf || cap == 0) return 0;
    wbuf_t w = {buf, cap, 0, true};
    wb_puts(&w, "{\"v\":1,\"type\":\"task_start\",\"name\":");
    wb_put_json_str(&w, name);
    wb_printf(&w, ",\"ts\":%lld}", (long long)ts);
    return wb_finish(&w);
}

size_t record_line_task_end(char *buf, size_t cap, const char *name, int64_t ts,
                            uint32_t active_sec, uint8_t rounds) {
    if (!buf || cap == 0) return 0;
    wbuf_t w = {buf, cap, 0, true};
    wb_puts(&w, "{\"v\":1,\"type\":\"task_end\",\"name\":");
    wb_put_json_str(&w, name);
    wb_printf(&w, ",\"ts\":%lld,\"active_sec\":%lu,\"rounds\":%u}", (long long)ts,
              (unsigned long)active_sec, (unsigned)rounds);
    return wb_finish(&w);
}

size_t record_line_timer_start(char *buf, size_t cap, uint16_t minutes, int64_t ts) {
    if (!buf || cap == 0) return 0;
    wbuf_t w = {buf, cap, 0, true};
    wb_printf(&w, "{\"v\":1,\"type\":\"timer_start\",\"minutes\":%u,\"ts\":%lld}",
              (unsigned)minutes, (long long)ts);
    return wb_finish(&w);
}

size_t record_line_timer_done(char *buf, size_t cap, uint16_t minutes, int64_t ts,
                              bool completed) {
    if (!buf || cap == 0) return 0;
    wbuf_t w = {buf, cap, 0, true};
    wb_printf(&w, "{\"v\":1,\"type\":\"timer_done\",\"minutes\":%u,\"ts\":%lld,\"completed\":%s}",
              (unsigned)minutes, (long long)ts, completed ? "true" : "false");
    return wb_finish(&w);
}
