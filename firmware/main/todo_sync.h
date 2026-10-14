// main/todo_sync.h —— 服务端任务同步客户端（最小版）。
// 拉取：服务端任务模板（/api/todo/tasks，按名字合并保留本地进度，重置周期字段暂不参与设备逻辑）；
// 推送：/fs/rec/<date>.jsonl 记录行批量上传（NVS 记录每日已传行数，服务端按追加去重交给上层）。
// 前提：todo_cfg 已配置 server/token；记录经 trio_app append_record → todo_sync_note_record。
#pragma once

#include <stdbool.h>

#include "task_model.h"

#define TODO_SYNC_TASK_MAX 12u /* 单次拉取应用的任务数上限（>TASK_LIST_MAX 便于检测截断） */

/* 读 todo_cfg 并启动同步 worker；未配置 server/token 时为空转。 */
void todo_sync_init(void);

/* append_record 落盘成功后调用：通知 worker 有新记录待上传（非阻塞）。 */
void todo_sync_note_record(void);

/* 立即唤醒 worker（播客等模块遇到未配对/401 时加速 claim 重试；非阻塞）。 */
void todo_sync_kick(void);

/* trio_app_tick 周期调用：网络就绪时触发一次同步；消费 worker 拉取的任务并应用。 */
void todo_sync_tick(void);
