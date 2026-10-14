#include "clock_time.h"

#include <stdio.h>
#include <time.h>

bool clock_time_format_date(int64_t epoch, char out[9]) {
    if (!out || epoch < 0) return false;
    time_t t = (time_t)epoch;
    struct tm local;
    if (localtime_r(&t, &local) == NULL) return false;
    if (strftime(out, 9, "%Y%m%d", &local) != 8) return false;
    return true;
}

bool clock_time_now_date(char out[9]) {
    if (!out) return false;
    time_t now;
    time(&now);
    if ((int64_t)now < CLOCK_TIME_MIN_VALID) return false;
    return clock_time_format_date((int64_t)now, out);
}
