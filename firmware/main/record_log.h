// main/record_log.h —— 会话记录：fs 分区 LittleFS 上的按日 JSONL 文件。
// 文件 /fs/rec/<YYYYMMDD>.jsonl，一行一条记录（record_line.h 构造），
// 按天分文件使记录天然成为服务端同步单元。
#pragma once

#include <stdbool.h>

/* 挂载 fs 分区；首次挂载失败会格式化重试一次（新设备/空分区场景）。 */
bool record_log_init(void);

/* 追加一行到当日记录文件（自动补 '\n'）。date 必须是 8 位数字日期。 */
bool record_log_append(const char *date, const char *line);

/* 初始化或写入过程是否发生过错误。 */
bool record_log_has_error(void);
