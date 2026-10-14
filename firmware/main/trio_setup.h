// main/trio_setup.h —— 启动期一次性 SoftAP 配网服务。
// 模式照搬 podcast_setup：阻塞运行（此时应用主循环/LVGL tick 未启动）、
// 自建屏幕展示热点信息、RAM 试连成功才落 netcfg NVS；长按 OK 取消。
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

/* 配网服务是否在运行（main 按键分发据此改道）。 */
bool trio_setup_active(void);

/* 配网期间的按键入口：确定长按 = 取消意图（已有配置时可退出重启）。 */
void trio_setup_key(bsp_btn_t button, bsp_btn_ev_t event);

/* 阻塞运行配网。成功保存（或取消重启）后返回 true；
 * 初始化失败返回 false，main 按无网模式继续启动。 */
bool trio_setup_run(void);

/* 运行中的应用请求重启进入配网：置 netcfg 标志，下次启动生效。 */
void trio_setup_request(void);

/* 读取并清除配网请求标志（启动时调用一次）。 */
bool trio_setup_take_request(void);
