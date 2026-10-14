// main/main.c —— 重启隔离架构入口：上电读 NVS run_mode，只实例化选中业务。
//
// 模式（app_mode.h，存 netcfg/trio/mode）：
//   0 = 主控选择页（launcher，仅菜单；选中应用=写 NVS+重启）
//   1 = TODO 应用（番茄钟+独立计时器；含提示音/服务端同步）
//   2 = PODCAST 应用（播放器，enter 内自建 worker）
// 切换应用 = 应用内双击确定（写 mode 0 + 重启）→ 主控页选择 → 写 NVS + 重启。
// 非选中业务的代码只躺在 Flash，不创建任务/定时器/外设，互不抢占 SRAM。
//
// 按键语义(全局统一):
//   上/下 短按   页面内自定义(选择/调档)
//   确定  短按   页面内自定义(开始/暂停/恢复/确认)
//   确定  双击   应用模式=写 mode 0 + 重启回主控页；主控页无动作
//   确定  长按   页面内自定义(结束任务/取消计时/配网取消)
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "key_guard.h"
#include "launcher.h"
#include "rtttl_player.h"
#include "todo_sync.h"
#include "trio_app.h"
#include "trio_setup.h"
#include "wifi_link.h"

#include "app_framework.h"
#include "app_mode.h"
#include "app_podcast.h"
#include "app_todo.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "main";
#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static volatile bool s_ui_ready;
volatile uint32_t g_input_hwm; /* 输入任务栈高水位（诊断，心跳读取） */

static app_mode_t s_mode = APP_MODE_LAUNCHER;
static const trio_app_t *s_app; /* 当前模式的应用；NULL = 主控页 */

// 按键回调在 esp_timer 任务上:只入队,所有 UI 操作集中在 input_task 持锁执行。
// 配网运行期间(main 启动早期)按键改道 trio_setup,不触碰应用 UI。
static void process_input(const input_event_t *input) {
    if (trio_setup_active()) {
        trio_setup_key(input->btn, input->event);
        return;
    }
    if (!s_ui_ready) return;
    /* 双击确定=退出手势：仅应用模式生效，写回主控模式并强制 factory
     * 启动后整机重启（防 otadata 残留指向 ota_0 的小智固件）。 */
    if (s_app != NULL && input->btn == BSP_BTN_OK && input->event == BSP_BTN_DOUBLE) {
        ESP_LOGI(TAG, "双击确定:退出应用,重启回主控页");
        app_mode_reboot_into(APP_MODE_LAUNCHER);
        ESP_LOGE(TAG, "退出重启失败,留在当前应用");
        return;
    }
    if (!bsp_lvgl_lock(500)) return;
    if (s_app != NULL) s_app->key(input->btn, input->event);
    else launcher_key(input->btn, input->event);
    bsp_lvgl_unlock();
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        g_input_hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    /* 4096：enter 建屏已在 main 任务执行（8192 栈），本任务只做按键分发，
     * 实测峰值 ~2.1KB；放大栈会白白挤占 C3 本就紧张的堆。 */
    if (xTaskCreate(input_task, "trio_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold (500 ms here), so an unintended action fires almost immediately. Drop
// key actions until that press comes up; the deadline is only a safety net so a
// stuck key cannot leave the pad dead. The state machine lives in key_guard.h so it
// can be unit-tested without the rest of the demo.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

/* 250ms 心跳：分发应用 tick + 平台诊断（所有模式共用，main 持有）。 */
static void trio_tick_cb(lv_timer_t *timer) {
    (void)timer;
    static uint8_t beat;
    if (++beat >= 40) { /* 10s 心跳：证明 LVGL tick 存活（临时诊断） */
        beat = 0;
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGW(TAG, "alive: mode=%u heap=%u largest=%u lvgl.used=%u/%u input_hwm=%u",
                 (unsigned)s_mode,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
                 (unsigned)g_input_hwm);
    } else if (beat == 2) { /* ~5s：打印 WiFi IP（临时诊断，连通性排查用） */
        char ip[16];
        wifi_link_ip_str(ip, sizeof(ip));
        ESP_LOGW(TAG, "ip=%s", ip[0] ? ip : "none");
    }
    if (s_app != NULL) s_app->tick();
}

void app_main(void) {
    ESP_LOGI(TAG, "三合一应用启动(重启隔离架构)");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是本应用的 UI 载体,失败就打清日志后退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
    } else {
        s_input_ready = true; /* 事件流转即生效;UI 就绪由 s_ui_ready 单独把守 */
        if (s_key_guard.armed) {
            const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
            key_guard_sync_released(&s_key_guard, released);
            if (!s_key_guard.armed) {
                ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
            }
        }
    }
    // 音频初始化:播客播放与提示音共用;失败仅影响声音。
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败,声音不可用");
    }

    // 配网:首次无凭据或主控页请求过重启配网时,进入启动期阻塞式配网服务。
    // 需在 wifi_link 事件注册前独立接管射频;成功保存后清理,继续正常启动。
    char setup_ssid[33];
    char setup_pass[65];
    bool have_credentials = false;
    if (wifi_link_storage_prepare() == ESP_OK) {
        have_credentials = wifi_link_credentials_load(setup_ssid, sizeof(setup_ssid),
                                                      setup_pass, sizeof(setup_pass));
        if (!have_credentials || trio_setup_take_request()) {
            if (!trio_setup_run()) {
                ESP_LOGE(TAG, "配网服务启动失败,按无网模式继续");
            }
        }
    } else {
        ESP_LOGE(TAG, "netcfg 存储准备失败,跳过配网与联网");
    }

    // 应用模型与持久化(快照恢复/断电降级),失败时内部回落默认值。
    trio_app_init();

    // 后台自动联网校时;无凭据/失败不阻塞 UI。
    wifi_link_autostart();

    // 读回运行模式,只实例化选中业务(其余业务代码仅驻留 Flash)。
    s_mode = app_mode_get();
    switch (s_mode) {
    case APP_MODE_TODO:
        if (rtttl_player_start() != ESP_OK) {
            ESP_LOGE(TAG, "提示音播放器启动失败");
        }
        todo_sync_init(); /* 服务端任务同步:仅 TODO 模式需要 */
        s_app = &APP_TODO;
        break;
    case APP_MODE_PODCAST:
        s_app = &APP_PODCAST; /* enter 内 trio_podcast_init 自建 worker */
        break;
    default:
        s_mode = APP_MODE_LAUNCHER;
        break;
    }

    if (bsp_lvgl_lock(1000)) {
        if (s_app != NULL) s_app->enter();
        else launcher_enter();
        lv_timer_create(trio_tick_cb, 250, NULL);
        bsp_lvgl_unlock();
        s_ui_ready = true;
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,UI 未启动");
    }

    ESP_LOGI(TAG, "就绪 mode=%u", (unsigned)s_mode);
}
