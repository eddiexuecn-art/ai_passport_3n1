// main/task_store.h —— 任务快照持久化：store 分区 NVS + 后台保存任务。
// 采用 request-save 模式（仿官方 pomodoro demo）：调用方只标记脏数据，落盘异步完成。
#pragma once

#include <stdbool.h>

#include "task_model.h"

/* 初始化 store 分区 NVS 并恢复快照到 list（内部调用 task_list_restore 降级运行态）。
   首次启动或快照损坏时回落 defaults。 */
bool task_store_init(task_list_t *list);

/* 请求保存：立即序列化并交给后台任务异步写 NVS；可从任意任务/回调调用。 */
void task_store_request_save(const task_list_t *list);

/* 初始化或保存过程是否发生过错误。 */
bool task_store_has_error(void);
