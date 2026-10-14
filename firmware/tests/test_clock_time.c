// tests/test_clock_time.c —— 本地日期格式化主机测试（与 tools/validate.sh 一同运行）。
// setenv/tzset 需 POSIX 扩展；必须先于任何 libc 头（含经 clock_time.h 引入者）。
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock_time.h"

int main(void)
{
    /* 设备时区固定 UTC+8；宿主测试显式设置同样的 TZ */
    setenv("TZ", "CST-8", 1);
    tzset();

    char date[9];

    /* 跨日边界：2025-08-12 16:00 UTC → 本地 2025-08-13 00:00 */
    assert(clock_time_format_date(1755028800, date));
    assert(strcmp(date, "20250813") == 0);

    /* 同日稍后：2025-08-12 16:30 UTC → 本地 2025-08-13 00:30 */
    assert(clock_time_format_date(1755030600, date));
    assert(strcmp(date, "20250813") == 0);

    /* 2026-10-13 00:00 UTC（= 2026-01-01 1767225600 + 285 天）→ 本地 08:00 同日 */
    assert(clock_time_format_date(1791849600, date));
    assert(strcmp(date, "20261013") == 0);

    /* 1970-01-01 00:00 UTC → 本地 1970-01-01 08:00 */
    assert(clock_time_format_date(0, date));
    assert(strcmp(date, "19700101") == 0);

    /* 非法输入 */
    assert(!clock_time_format_date(-1, date));
    assert(!clock_time_format_date(1755028800, NULL));

    /* now_date：宿主机系统时间必然可信（>2020），成功时必为 8 位数字 */
    if (clock_time_now_date(date)) {
        assert(strlen(date) == 8);
        for (int i = 0; i < 8; ++i) assert(date[i] >= '0' && date[i] <= '9');
    }

    return 0;
}
