// main/clock_time.h —— 校时后 epoch → 本地日期（纯逻辑，可主机单测）。
// 设备无 RTC：开机到 SNTP 校时成功前，系统时间从 1970 起算，不可当作真实日期。
// localtime_r 需 POSIX 扩展；此宏必须先于任何 libc 头定义（glibc features.h 语义）。
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 系统时间可信下限（2020-01-01）：低于它视为尚未校时。 */
#define CLOCK_TIME_MIN_VALID ((int64_t)1577836800)

/* epoch 秒 → 本地日期（TZ=CST-8，即 UTC+8）"YYYYMMDD"。缓冲不足/非法输入返回 false。 */
bool clock_time_format_date(int64_t epoch, char out[9]);

/* 当前系统时间的日期；未校时（低于可信下限）返回 false。 */
bool clock_time_now_date(char out[9]);
