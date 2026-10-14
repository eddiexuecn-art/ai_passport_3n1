// main/trio_app.h —— 三合一应用层：模型管家 + 记录落盘 + 提示音。
// UI（ui_trio）只做显示与按键翻译；业务动作都经本层，保证记录与存储一致。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "task_model.h"
#include "timer_model.h"

void trio_app_init(void);
task_list_t *trio_app_tasks(void);
timer_model_t *trio_app_timer(void);

/* 周期驱动（建议 250ms，LVGL 定时器内调用）：
 * 模型 tick → 到点事件（提示音 + 快照保存）→ 隔天滚动 → 请求落盘。 */
void trio_app_tick(void);

/* 当前 epoch：校时后 time()，未校时为 TASK_TIME_UNKNOWN（记录里表现为 -1）。 */
int64_t trio_app_now_epoch(void);

/* 动作入口（含记录落盘）。 */
bool trio_app_start_task(uint8_t index);
bool trio_app_continue_task(uint8_t index); /* 休息结束 READY 态：按键确认开下一轮 */
bool trio_app_end_task(uint8_t index);
bool trio_app_interrupt_task(uint8_t index); /* 中断本轮：任务回 IDLE 仍可见，记录落盘 */
bool trio_app_timer_start(void);
bool trio_app_timer_cancel(void);
