// main/app_framework.h —— 应用容器契约（重启隔离架构）。
// 同一时刻只运行一个应用：主控页/应用选择 = 写 NVS 模式 + esp_restart，
// 上电由 main 按 app_mode 只实例化选中业务。应用之间互不可见、零交叉引用：
// 网络/凭据/音频驱动/记录等平台能力由 main 直接初始化的服务层提供，
// 应用只按需调用，不在应用间横向借用。
#pragma once

#include "bsp_button.h"

typedef struct {
    void (*enter)(void);      /* 建屏并载入首屏（上电仅调用一次） */
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  /* 按键（持 LVGL 锁） */
    void (*tick)(void);       /* 由 main 的 250ms 定时器调（持 LVGL 锁） */
} trio_app_t;
