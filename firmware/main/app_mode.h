// main/app_mode.h —— 运行模式持久化（重启隔离架构）。
// 主控页选中应用 = 写 NVS + esp_restart；上电读回模式，只实例化选中业务。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_MODE_LAUNCHER = 0, /* 主控选择页 */
    APP_MODE_TODO    = 1,  /* TODO（番茄钟 + 独立计时器） */
    APP_MODE_PODCAST = 2,  /* 播客播放器 */
    APP_MODE_AI      = 3,  /* AI 对话：重启进 ota_0 分区的小智固件（双固件方案） */
} app_mode_t;

/* 读当前模式；NVS 不可用/值非法时回落主控页。 */
app_mode_t app_mode_get(void);

/* 写模式并落盘；true=成功（调用方随后 esp_restart）。 */
bool app_mode_set(app_mode_t mode);

/* 写模式 + 切换启动分区 + 重启（不返回）：
 *   APP_MODE_AI → ota_0（小智固件）；其余 → factory（三合一）。
 * 任一步失败返回 false，调用方留在当前页面（此时模式可能已写入，
 * 三合一上电对未知模式回落主控页，不会卡死）。 */
bool app_mode_reboot_into(app_mode_t mode);
