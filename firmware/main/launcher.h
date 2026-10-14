// main/launcher.h —— 主控选择页（重启隔离架构的模式 0）。
// 纯菜单：选中应用 = 写 NVS run_mode + esp_restart，不在本页动态打开应用。
#pragma once

#include "bsp_button.h"

/* 建主控页并载入（仅模式 0 调用；tick 定时器由 main 统一创建）。 */
void launcher_enter(void);

/* 按键入口（调用方保证已持 LVGL 键）。 */
void launcher_key(bsp_btn_t btn, bsp_btn_ev_t ev);
