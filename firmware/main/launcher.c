// main/launcher.c —— 主控选择页：TODO / PODCAST / AI / SETUP 四项。
// 重启隔离：OK 选中 = app_mode_reboot_into(N)（写 NVS + 切启动分区 + 重启），
// 上电只实例化选中业务；AI 项切换到 ota_0 分区的小智固件（双固件方案）。
// SETUP 项置配网请求标志后重启进 SoftAP（沿用原「列表页长按上」语义）。
#include "launcher.h"

#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "trio_setup.h"

#include "app_mode.h"

#define C_BG 0xFFFFFF
#define C_INK 0x1F2328
#define C_MUTED 0x8A9299
#define C_LINE 0xE5E1D8
#define C_FOCUS 0xE0533D
#define C_BLUE 0x1567F6
#define C_DONE 0xB9BDC2

#define ROW_H 44u
#define LIST_TOP 46u
#define BAR_H 34u

#define F_SMALL &lv_font_montserrat_14
#define F_TITLE &lv_font_montserrat_28

typedef struct {
    const char *name;
    const char *hint;     /* 选中时底部条文案；NULL = "OK OPEN" */
    bool enabled;
    app_mode_t mode;         /* 应用项：选中即写 NVS 重启 */
    void (*action)(void);    /* 非应用动作（如进配网）；与 mode 二选一 */
} launch_item_t;

static void setup_action(void) {
    trio_setup_request(); /* 重启进入 SoftAP 配网 */
    esp_restart();
}

static const launch_item_t ITEMS[] = {
    { "TODO",    NULL,                  true,  APP_MODE_TODO,     NULL },
    { "PODCAST", NULL,                  true,  APP_MODE_PODCAST,  NULL },
    { "AI",      NULL,                  true,  APP_MODE_AI,       NULL },
    { "SETUP",   "OK REBOOT TO CONFIG", true,  APP_MODE_LAUNCHER, setup_action },
};
#define ITEM_COUNT (sizeof(ITEMS) / sizeof(ITEMS[0]))

static lv_obj_t *s_scr;
static lv_obj_t *s_row_name[4];
static lv_obj_t *s_row_mark[4];
static lv_obj_t *s_bar;
static uint8_t s_sel;

static const launch_item_t *cur(void) { return &ITEMS[s_sel]; }

static void launcher_refresh(void) {
    for (size_t i = 0; i < ITEM_COUNT; ++i) {
        const bool selected = (int)i == s_sel;
        const lv_coord_t y = (lv_coord_t)(LIST_TOP + i * ROW_H);
        lv_obj_align(s_row_mark[i], LV_ALIGN_TOP_LEFT, 10, y + 5);
        lv_obj_align(s_row_name[i], LV_ALIGN_TOP_LEFT, 24, y + 8);
        lv_label_set_text(s_row_name[i], ITEMS[i].name);
        /* 灰色占位项整行置灰，选中也不高亮 */
        const uint32_t color = !ITEMS[i].enabled ? C_DONE
                               : selected ? C_FOCUS : C_INK;
        lv_obj_set_style_text_color(s_row_name[i], lv_color_hex(color), 0);
        lv_obj_set_style_bg_color(s_row_mark[i],
                                  lv_color_hex(selected && ITEMS[i].enabled ? C_FOCUS : C_LINE), 0);
        lv_obj_set_style_bg_opa(s_row_mark[i], LV_OPA_COVER, 0);
    }
    lv_label_set_text(s_bar, cur()->hint != NULL ? cur()->hint : "OK OPEN");
}

static void open_selected(void) {
    if (!cur()->enabled) return;
    if (cur()->action != NULL) {
        cur()->action(); /* SETUP：不返回 */
        return;
    }
    /* 写模式 + 切启动分区（AI→ota_0 小智固件，其余→factory）+ 重启；
     * 失败则留在主控页。 */
    if (!app_mode_reboot_into(cur()->mode)) {
        ESP_LOGE("launcher", "进入模式 %d 失败，留在主控页", (int)cur()->mode);
    }
}

void launcher_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (btn == BSP_BTN_UP) s_sel = (uint8_t)((s_sel + ITEM_COUNT - 1) % ITEM_COUNT);
        else s_sel = (uint8_t)((s_sel + 1) % ITEM_COUNT);
        launcher_refresh();
    } else if (btn == BSP_BTN_OK) {
        open_selected();
    }
}

/* ---------- 构建 ---------- */

static void launcher_page_build(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t *head = lv_label_create(scr);
    lv_label_set_text(head, "TRIO");
    lv_obj_set_style_text_font(head, F_TITLE, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(C_BLUE), 0);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 14, 6);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 240, BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar_line = lv_obj_create(scr);
    lv_obj_set_size(bar_line, 240, 1);
    lv_obj_set_style_bg_color(bar_line, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(bar_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_line, 0, 0);
    lv_obj_set_style_radius(bar_line, 0, 0);
    lv_obj_align(bar_line, LV_ALIGN_BOTTOM_MID, 0, -(BAR_H + 1));

    for (size_t i = 0; i < ITEM_COUNT; ++i) {
        s_row_mark[i] = lv_obj_create(scr);
        lv_obj_set_size(s_row_mark[i], 3, ROW_H - 8);
        lv_obj_set_style_radius(s_row_mark[i], 2, 0);
        lv_obj_set_style_border_width(s_row_mark[i], 0, 0);

        s_row_name[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(s_row_name[i], F_TITLE, 0);
        lv_obj_set_width(s_row_name[i], 200);
        lv_label_set_long_mode(s_row_name[i], LV_LABEL_LONG_DOT);
    }

    s_bar = lv_label_create(bar);
    lv_obj_set_style_text_font(s_bar, F_SMALL, 0);
    lv_obj_set_style_text_color(s_bar, lv_color_hex(C_INK), 0);
    lv_obj_center(s_bar);

    s_scr = scr;
}

void launcher_enter(void) {
    launcher_page_build();
    launcher_refresh();
    lv_screen_load(s_scr);
}
