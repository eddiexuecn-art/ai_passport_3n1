// main/app_todo.h —— TODO 应用：任务列表 + 番茄钟 + 独立计时器（含菜单共 4 屏）。
// 生命周期见 app_framework.h；exit 只清 UI 与运行态显示，任务模型常驻
// （task_store NVS 快照保证断电安全，重进即恢复）。
#pragma once

#include "app_framework.h"

extern const trio_app_t APP_TODO;
