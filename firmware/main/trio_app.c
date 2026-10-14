#include "trio_app.h"

#include <string.h>
#include <time.h>

#include "clock_sync.h"
#include "clock_time.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "record_line.h"
#include "record_log.h"
#include "rtttl_player.h"
#include "task_store.h"
#include "todo_sync.h"
#include "trio_sounds.h"

static const char *TAG = "trio_app";
static task_list_t s_tasks;
static timer_model_t s_timer;

static uint64_t now_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000);
}

int64_t trio_app_now_epoch(void) {
    if (!clock_sync_synced()) return TASK_TIME_UNKNOWN;
    time_t now;
    time(&now);
    return (int64_t)now > CLOCK_TIME_MIN_VALID ? (int64_t)now : TASK_TIME_UNKNOWN;
}

/* 首次启动的演示任务：让设备开箱即可体验完整任务流程；
   服务端同步接通后真实数据会覆盖（存满 10 个上限后不可再加）。 */
static const char *DEMO_TASKS[] = {
    "Reading", "Coding", "Workout", "English", "Piano",
    "Drawing", "Homework", "Cleaning", "Meditation", "Review",
};
#define DEMO_TASK_COUNT (sizeof(DEMO_TASKS) / sizeof(DEMO_TASKS[0]))

static void seed_demo_tasks(void) {
    if (s_tasks.count > 0) return;
    for (size_t i = 0; i < DEMO_TASK_COUNT; ++i) {
        if (!task_list_add(&s_tasks, DEMO_TASKS[i])) break;
    }
    ESP_LOGI(TAG, "已预置 %u 个演示任务", (unsigned)s_tasks.count);
}

void trio_app_init(void) {
    task_store_init(&s_tasks); /* 失败/首次启动回落 defaults，内部已 restore */
    seed_demo_tasks();
    timer_model_defaults(&s_timer);
    timer_model_restore(&s_timer);
}

static void save_snapshot(void) {
    task_store_request_save(&s_tasks);
}

static void append_record(const char *line) {
    char date[9];
    if (!clock_sync_synced() || !clock_time_now_date(date)) {
        ESP_LOGW(TAG, "时间未同步，记录暂不落盘");
        return;
    }
    if (!record_log_append(date, line)) {
        ESP_LOGE(TAG, "记录写入失败");
    } else {
        todo_sync_note_record(); /* 通知同步 worker 有新记录待上传 */
    }
}

void trio_app_tick(void) {
    todo_sync_tick(); /* 网络就绪触发同步；应用服务端任务列表（在本 tick 内完成） */

    /* 隔天滚动：校时后日期变化 → 重置列表（记录已按日文件实时落盘，无需导出）。 */
    char date[9];
    if (clock_sync_synced() && clock_time_now_date(date) &&
        task_list_needs_rollover(&s_tasks, date)) {
        task_list_apply_rollover(&s_tasks, date);
        save_snapshot();
        ESP_LOGI(TAG, "新的一天 %s：任务列表已重置", date);
    }

    /* 任务模型：单任务专注，选中任务即进行中的那个。
     * 主控架构下应用独占运行期：TODO 活跃时播客必然已 exit，喇叭无争用。 */
    for (uint8_t i = 0; i < s_tasks.count; ++i) {
        task_event_t ev = task_list_tick(&s_tasks, i, now_ms());
        if (ev == TASK_EVENT_FOCUS_COMPLETE) {
            rtttl_player_play(TRIO_SOUND_REST);
            save_snapshot();
        } else if (ev == TASK_EVENT_REST_COMPLETE) {
            /* 进入 READY：等按键确认（task_list_continue），不自动开下一轮。 */
            rtttl_player_play(TRIO_SOUND_RESUME);
            save_snapshot();
        }
    }

    /* 独立计时器：到点封记录 + 提示音。 */
    timer_event_t tev = timer_model_tick(&s_timer, now_ms(), trio_app_now_epoch());
    if (tev == TIMER_EVENT_COMPLETE) {
        char line[RECORD_LINE_MAX];
        if (record_line_timer_done(line, sizeof(line), s_timer.preset_sec / 60u,
                                   trio_app_now_epoch(), true) > 0) {
            append_record(line);
        }
        rtttl_player_play(TRIO_SOUND_TIMER);
    }
}

bool trio_app_start_task(uint8_t index) {
    bool ok = task_list_start(&s_tasks, index, now_ms(), trio_app_now_epoch());
    if (!ok) return false;
    char line[RECORD_LINE_MAX];
    if (record_line_task_start(line, sizeof(line),
                               task_list_get(&s_tasks, index)->name,
                               trio_app_now_epoch()) > 0) {
        append_record(line);
    }
    save_snapshot();
    return true;
}

bool trio_app_continue_task(uint8_t index) {
    /* 下一轮开启不产生新记录行：记录粒度是"今日任务会话"（start/end 封口）。 */
    bool ok = task_list_continue(&s_tasks, index, now_ms());
    if (ok) save_snapshot();
    return ok;
}

bool trio_app_end_task(uint8_t index) {
    const task_t *task = task_list_get(&s_tasks, index);
    if (!task) return false;
    char name[TASK_NAME_MAX];
    strlcpy(name, task->name, sizeof(name));
    uint32_t active_sec = task->active_sec;
    uint8_t rounds = task->rounds_done;

    bool ok = task_list_end(&s_tasks, index, now_ms(), trio_app_now_epoch());
    if (!ok) return false;
    char line[RECORD_LINE_MAX];
    if (record_line_task_end(line, sizeof(line), name, trio_app_now_epoch(),
                             active_sec, rounds) > 0) {
        append_record(line);
    }
    save_snapshot();
    return true;
}

bool trio_app_interrupt_task(uint8_t index) {
    const task_t *task = task_list_get(&s_tasks, index);
    if (!task) return false;
    char name[TASK_NAME_MAX];
    strlcpy(name, task->name, sizeof(name));
    uint32_t active_sec = task->active_sec;
    uint8_t rounds = task->rounds_done;

    bool ok = task_list_interrupt(&s_tasks, index, now_ms());
    if (!ok) return false;
    char line[RECORD_LINE_MAX];
    if (record_line_task_end(line, sizeof(line), name, trio_app_now_epoch(),
                             active_sec, rounds) > 0) {
        append_record(line);
    }
    save_snapshot();
    return true;
}

bool trio_app_timer_start(void) {
    bool ok = timer_model_start(&s_timer, now_ms(), trio_app_now_epoch());
    if (!ok) return false;
    char line[RECORD_LINE_MAX];
    if (record_line_timer_start(line, sizeof(line), s_timer.preset_sec / 60u,
                                trio_app_now_epoch()) > 0) {
        append_record(line);
    }
    return true;
}

bool trio_app_timer_cancel(void) {
    timer_state_t state = s_timer.state;
    if (state == TIMER_FINISHED) {
        /* FINISHED 确认：记录已在 tick 封存，仅复位 */
        return timer_model_ack(&s_timer);
    }
    if (state == TIMER_IDLE) return false;
    if (state == TIMER_RUNNING) {
        timer_model_pause(&s_timer, now_ms());
    }
    char line[RECORD_LINE_MAX];
    if (record_line_timer_done(line, sizeof(line), s_timer.preset_sec / 60u,
                               trio_app_now_epoch(), false) > 0) {
        append_record(line);
    }
    return timer_model_cancel(&s_timer, trio_app_now_epoch());
}

task_list_t *trio_app_tasks(void) {
    return &s_tasks;
}

timer_model_t *trio_app_timer(void) {
    return &s_timer;
}
