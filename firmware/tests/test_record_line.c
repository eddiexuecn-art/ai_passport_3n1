// tests/test_record_line.c —— 会话记录 JSONL 行构造主机测试（与 tools/validate.sh 一同运行）。
#include <assert.h>
#include <string.h>

#include "record_line.h"
#include "task_model.h"

int main(void)
{
    char line[RECORD_LINE_MAX];

    /* 常规任务开始 */
    assert(record_line_task_start(line, sizeof(line), "Reading", 1755028800) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"task_start\",\"name\":\"Reading\",\"ts\":1755028800}")
           == 0);

    /* 时间未知写 -1 */
    assert(record_line_task_start(line, sizeof(line), "Solo", TASK_TIME_UNKNOWN) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"task_start\",\"name\":\"Solo\",\"ts\":-1}") == 0);

    /* JSON 转义：引号、反斜杠、控制字符；0x7F 以上字节直通 */
    assert(record_line_task_start(line, sizeof(line), "He said \"hi\"\\a\n\t", 100) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"task_start\",\"name\":"
                  "\"He said \\\"hi\\\"\\\\a\\n\\t\",\"ts\":100}") == 0);
    assert(record_line_task_start(line, sizeof(line), "C\x01\x1f", 1) > 0);
    assert(strstr(line, "\\u0001") != NULL);
    assert(strstr(line, "\\u001F") != NULL);

    /* 空名合法 */
    assert(record_line_task_start(line, sizeof(line), "", 1) > 0);
    assert(strcmp(line, "{\"v\":1,\"type\":\"task_start\",\"name\":\"\",\"ts\":1}") == 0);

    /* 任务结束：完整字段 */
    assert(record_line_task_end(line, sizeof(line), "Reading", 1755032400, 2719, 3) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"task_end\",\"name\":\"Reading\",\"ts\":1755032400,"
                  "\"active_sec\":2719,\"rounds\":3}") == 0);

    /* 计时器开始/完成 */
    assert(record_line_timer_start(line, sizeof(line), 5, 1755028800) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"timer_start\",\"minutes\":5,\"ts\":1755028800}") == 0);
    assert(record_line_timer_done(line, sizeof(line), 60, 1755032400, true) > 0);
    assert(strcmp(line,
                  "{\"v\":1,\"type\":\"timer_done\",\"minutes\":60,\"ts\":1755032400,"
                  "\"completed\":true}") == 0);
    assert(record_line_timer_done(line, sizeof(line), 15, -1, false) > 0);
    assert(strstr(line, "\"completed\":false") != NULL);
    assert(strstr(line, "\"ts\":-1") != NULL);

    /* 缓冲不足：整体失败返回 0，调用方不得使用该行 */
    assert(record_line_task_end(line, 64,
                                "a-very-long-task-name-that-needs-escaping", 1, 1, 1) == 0);
    assert(record_line_task_start(NULL, sizeof(line), "X", 1) == 0);
    assert(record_line_task_start(line, 0, "X", 1) == 0);

    /* 名字字段最大转义长度也能放下（23 字节全转义） */
    assert(record_line_task_end(line, sizeof(line), "\"\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\",
                                1, 0, 0) > 0);

    return 0;
}
