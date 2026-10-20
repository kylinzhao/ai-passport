// main/zcp_app.c —— zcode-pet 玩法：页面状态机（Boot/Pet/About + 清单浮层）与三键路由。
//
// 设备端职责 = 「状态镜像 + 帧播放器」：任务计数/终态永远来自 Mac 的 status 消息
// （zcp_bridge），本地零推断。UI 为本玩法全新设计——不复用 demo 测试菜单的外壳。
//
// 页面：
//   Boot    上电 → 标题 + perk 帧 1.2s → 自动进 Pet
//   Pet     connecting（BLE 栈启动中）→ waiting（设备名+等待秒数，60s 追加诊断；
//           本次开机曾 online 后掉线 → mid14）→ online（镜像 mode/mid/tier +
//           顶栏 任务N 未读M）；GATT 在线但 >10s 无 status → STALLED（mid14 兜底）
//   About   OK 长按进入；设备名/固件/电量/按键提示
//   浮层    UP 单击开/任意键关：任务 N · 未读 M（纯镜像数据）
//
// 线程约定：按键事件从 input task 进入（zcp_app_key），内部先取 LVGL 锁；状态/动画
// 定时器运行在 LVGL 任务；zcp_bridge 的快照读取自身带锁。
#include "zcp_app.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "zcp_bridge.h"
#include "zcp_mirror.h"
#include "zcp_pet.h"
#include "zcp_strings.h"
#include "zcp_zpf.h"

static const char *TAG = "zcp_app";

// ---- 视觉常量（深色底 = 帧包压平底色，见 zcp_mirror.h ZCP_BG_HEX）----
#define COL_BG ZCP_BG_HEX
#define COL_INK 0xF2F2F0
#define COL_DIM 0x9A9AA0
#define COL_ACCENT 0xF5B13F

#define BOOT_PERIOD_MS 1200
#define PAT_PERIOD_MS 2000
#define STATE_PERIOD_MS 500

typedef enum {
    PAGE_BOOT = 0,
    PAGE_PET,
    PAGE_ABOUT,
    PAGE_TASKS,
    PAGE_DETAIL,
} page_t;

LV_FONT_DECLARE(zcp_font_16);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);

// zcp_font_16 是生成字体（闭集码点）；数字/ASCII 走 Montserrat fallback，
// 保证运行期数字模板永不击穿闭集（见 docs/development/engineering/lvgl-chinese-fonts.md）。
static lv_font_t s_font_cjk;

static lv_obj_t *s_scr;
static lv_obj_t *s_boot_box, *s_boot_title;
static lv_obj_t *s_pet_box;
static lv_obj_t *s_topbar, *s_caption, *s_subline, *s_diag;
static lv_obj_t *s_about_box, *s_about_title, *s_about_name, *s_about_batt, *s_about_hint, *s_about_skin;
static lv_obj_t *s_list_box, *s_list_label;
static lv_obj_t *s_tasks_box, *s_tasks_title, *s_tasks_rows[ZCP_TASKS_MAX], *s_tasks_hint;
static lv_obj_t *s_detail_box, *s_detail_title, *s_detail_status, *s_detail_toast, *s_detail_hint;
static zcp_tasks_t s_tasks_view;                 // 页面渲染用的本地快照（state_timer 刷新）
static int64_t s_toast_until_ms;                 // 详情页提示文案截止时刻

static page_t s_page = PAGE_BOOT;
static bool s_list_open;
static lv_timer_t *s_anim_timer, *s_state_timer, *s_boot_timer;
static int64_t s_pat_until_ms;                       // 摸头本地播放截止时刻
static zcp_link_state_t s_last_link = ZCP_LINK_CONNECTING;
static int s_last_clip = -1;
static int64_t s_wait_since_ms;                      // 进入 waiting 的时刻（诊断计时）
static bool s_ever_online;                           // 本次开机曾收到 status（掉线 → mid14）
static bool s_batt_available;                        // CW2017 在位（main.c 探测）

static void refresh_about_batt(void);
static void refresh_about_skin(void);
static void refresh_tasks_page(void);
static void refresh_detail_page(void);

// ---- 小工具 ----

static void label_style(lv_obj_t *l, lv_color_t color, const lv_font_t *font) {
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
}

static void show_page(page_t page) {
    s_page = page;
    if (page == PAGE_BOOT) lv_obj_remove_flag(s_boot_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_boot_box, LV_OBJ_FLAG_HIDDEN);
    if (page == PAGE_PET) lv_obj_remove_flag(s_pet_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_pet_box, LV_OBJ_FLAG_HIDDEN);
    if (page == PAGE_ABOUT) {
        lv_obj_remove_flag(s_about_box, LV_OBJ_FLAG_HIDDEN);
        refresh_about_batt();
    } else {
        lv_obj_add_flag(s_about_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (page == PAGE_TASKS) {
        zcp_bridge_get_tasks(&s_tasks_view);
        refresh_tasks_page();
    } else {
        lv_obj_add_flag(s_tasks_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (page == PAGE_DETAIL) {
        refresh_detail_page();
    } else {
        lv_obj_add_flag(s_detail_box, LV_OBJ_FLAG_HIDDEN);
    }
    if (page != PAGE_PET) {
        lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
        s_list_open = false;
    }
    lv_obj_invalidate(s_scr);
}

// ---- 文案组装（数字模板：CJK 用子集字体，数字走 Montserrat fallback）----

// mid 7/8（%d 个任务并行/忙翻）需要把 status 帧里的 n 代入模板
static void set_caption_mid_n(int mid, int n) {
    const char *text = zcp_mid_text(mid);
    if (!text) { lv_label_set_text(s_caption, ""); return; }
    if (n >= 0 && strchr(text, '%')) {
        char buf[64];
        snprintf(buf, sizeof(buf), text, (unsigned)n);
        lv_label_set_text(s_caption, buf);
    } else {
        lv_label_set_text(s_caption, text);
    }
}

static void set_caption_mid(int mid) {
    set_caption_mid_n(mid, -1);
}

static void list_show(bool open) {
    s_list_open = open;
    if (open) {
        zcp_pet_state_t st;
        zcp_bridge_get_state(&st);
        char buf[64];
        snprintf(buf, sizeof(buf), zcp_str_list_fmt,
                 (unsigned)st.run, (unsigned)st.unread);
        lv_label_set_text(s_list_label, buf);
        lv_obj_remove_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(s_scr);
}

// ---- Pet 页 500ms 状态刷新 ----

static void refresh_pet_page(void) {
    bool ble_ready = zcp_bridge_ble_ready();
    bool gatt_connected = zcp_bridge_gatt_connected();
    int64_t since_status = zcp_bridge_ms_since_status();
    zcp_link_state_t link = zcp_link_state(ble_ready, gatt_connected, since_status);

    if (link == ZCP_LINK_ONLINE) s_ever_online = true;
    if (link != s_last_link) {
        s_last_link = link;
        if (link == ZCP_LINK_WAITING) s_wait_since_ms = esp_timer_get_time() / 1000;
    }

    // clip 选择：摸头本地覆盖 > 镜像 mid > 设备态 wait
    int clip;
    if (s_pat_until_ms > 0 && esp_timer_get_time() / 1000 < s_pat_until_ms) {
        clip = ZCP_CLIP_PAT;
    } else if (link == ZCP_LINK_ONLINE) {
        zcp_pet_state_t st;
        zcp_bridge_get_state(&st);
        clip = zcp_clip_for_mid(st.mid, st.tier);
    } else {
        clip = ZCP_CLIP_WAIT;                            // connecting/waiting/stalled → wait 帧
    }

    char buf[96];
    switch (link) {
    case ZCP_LINK_CONNECTING: {
        set_caption_mid(12);                           // 蓝牙启动中…
        lv_label_set_text(s_topbar, "");
        lv_label_set_text(s_subline, "");
        int err = zcp_bridge_ble_last_err();
        if (err != 0) {
            snprintf(buf, sizeof(buf), zcp_str_ble_fail_fmt, err);
            lv_label_set_text(s_diag, buf);
        } else {
            lv_label_set_text(s_diag, "");
        }
        break;
    }
    case ZCP_LINK_WAITING: {
        int64_t waited = (esp_timer_get_time() / 1000) - s_wait_since_ms;
        if (s_ever_online) {
            set_caption_mid(14);                       // Mac 掉线了…
        } else if (zcp_wait_diag_due(waited)) {
            set_caption_mid(16);                       // 未发现 Mac · 检查 Mac 蓝牙与守护进程
        } else {
            set_caption_mid(13);                       // 等待 Mac 连接…
        }
        lv_label_set_text(s_topbar, "");
        snprintf(buf, sizeof(buf), zcp_str_sub_fmt, (int)(waited / 1000));
        lv_label_set_text(s_subline, buf);
        lv_label_set_text(s_diag, "");
        break;
    }
    case ZCP_LINK_STALLED:
        set_caption_mid(14);                           // GATT 在线但 Mac 僵死 → 掉线文案兜底
        lv_label_set_text(s_topbar, "");
        lv_label_set_text(s_subline, "");
        lv_label_set_text(s_diag, "");
        break;
    case ZCP_LINK_ONLINE: {
        zcp_pet_state_t st;
        zcp_bridge_get_state(&st);
        set_caption_mid_n(st.mid, st.has_n ? (int)st.n : -1);
        int soc = s_batt_available ? bsp_battery_soc() : -1;
        if (soc >= 0) {
            snprintf(buf, sizeof(buf), zcp_str_topbar_batt_fmt,
                     (unsigned)st.run, (unsigned)st.unread, (unsigned)soc);
        } else {
            snprintf(buf, sizeof(buf), zcp_str_topbar_fmt,
                     (unsigned)st.run, (unsigned)st.unread);
        }
        lv_label_set_text(s_topbar, buf);
        lv_label_set_text(s_subline, "");
        lv_label_set_text(s_diag, "");
        break;
    }
    }

    if (clip != s_last_clip) {
        s_last_clip = clip;
        zcp_pet_set_clip(clip);
    }
}

static void state_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_page == PAGE_PET) refresh_pet_page();
    if (s_page == PAGE_ABOUT) refresh_about_batt();
    if (s_page == PAGE_TASKS || s_page == PAGE_DETAIL) {
        zcp_bridge_get_tasks(&s_tasks_view);   // 推送到达后下一拍自动刷新
        if (s_page == PAGE_TASKS) refresh_tasks_page();
        else refresh_detail_page();
    }
    // NimBLE 连续失败（≥3 次）：30s 定时自动重试
    if (zcp_bridge_ble_retry_due(esp_timer_get_time() / 1000)) {
        ESP_LOGW(TAG, "BLE 连续失败，30s 定时自动重试");
        zcp_bridge_restart();
    }
}

static void anim_timer_cb(lv_timer_t *t) {
    (void)t;
    zcp_pet_tick();
}

static void boot_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_page != PAGE_BOOT) return;
    show_page(PAGE_PET);
}

// ---- 任务列表页 / 详情页 ----

static const char *task_status_text(uint8_t st) {
    switch (st) {
    case ZCP_TASK_ST_RUNNING: return zcp_str_task_st1;
    case ZCP_TASK_ST_DONE:    return zcp_str_task_st2;
    case ZCP_TASK_ST_ERROR:   return zcp_str_task_st3;
    default:                  return zcp_str_task_st0;
    }
}

static void refresh_tasks_page(void) {
    if (s_tasks_view.count == 0) {
        lv_label_set_text(s_tasks_title, zcp_str_tasks_title);
        lv_label_set_text(s_tasks_rows[0], zcp_str_task_empty);
        lv_obj_set_style_text_color(s_tasks_rows[0], lv_color_hex(COL_DIM), 0);
        for (int i = 1; i < ZCP_TASKS_MAX; i++) lv_label_set_text(s_tasks_rows[i], "");
        return;
    }
    for (int i = 0; i < ZCP_TASKS_MAX; i++) {
        if (i < (int)s_tasks_view.count) {
            char buf[ZCP_TASK_TITLE_MAX + 8];
            snprintf(buf, sizeof(buf), "%s%s",
                     s_tasks_view.unread[i] ? zcp_str_unread_mark : " ",
                     s_tasks_view.title[i]);
            lv_label_set_text(s_tasks_rows[i], buf);
            lv_obj_set_style_text_color(s_tasks_rows[i],
                lv_color_hex(i == (int)s_tasks_view.sel ? COL_ACCENT : COL_INK), 0);
        } else {
            lv_label_set_text(s_tasks_rows[i], "");
        }
    }
}

static void refresh_detail_page(void) {
    if (s_tasks_view.sel >= s_tasks_view.count) return;
    uint8_t i = s_tasks_view.sel;
    lv_label_set_text(s_detail_title, s_tasks_view.title[i]);
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s%s",
             task_status_text(s_tasks_view.status[i]),
             s_tasks_view.unread[i] ? zcp_str_unread_mark : "",
             s_tasks_view.status[i] == ZCP_TASK_ST_RUNNING ? "" : "");
    lv_label_set_text(s_detail_status, buf);
    if (s_toast_until_ms == 0 || esp_timer_get_time() / 1000 > s_toast_until_ms) {
        lv_label_set_text(s_detail_toast, "");
    }
}

// ---- About 页 ----

static void refresh_about_skin(void) {
    const char *names[] = { zcp_str_skin_0, zcp_str_skin_1 };
    uint8_t sk = zcp_pet_skin();
    char buf[48];
    snprintf(buf, sizeof(buf), zcp_str_skin_fmt,
             sk < zcp_pet_skin_count() ? names[sk] : "?");
    lv_label_set_text(s_about_skin, buf);
}

static void refresh_about_batt(void) {
    if (!s_batt_available) {
        lv_label_set_text(s_about_batt, "?");
        return;
    }
    int soc = bsp_battery_soc();
    char buf[32];
    if (soc >= 0) {
        snprintf(buf, sizeof(buf), zcp_str_batt_fmt, soc);
    } else {
        snprintf(buf, sizeof(buf), "?");               // 读失败显示「?」（DOWN 可重试）
    }
    lv_label_set_text(s_about_batt, buf);
}

// ---- 按键路由 ----

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (s_page == PAGE_BOOT) return;                   // 1.2s 自动进 Pet，按键忽略

    if (s_page == PAGE_ABOUT) {
        if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) show_page(PAGE_PET);
        else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {          // v1.9：换皮肤（电量已常驻顶栏）
            uint8_t next = (uint8_t)((zcp_pet_skin() + 1) % zcp_pet_skin_count());
            zcp_pet_set_skin(next);
            zcp_app_save_skin(next);
            refresh_about_skin();
        }
        else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) zcp_bridge_restart();
        else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) show_page(PAGE_PET);
        return;
    }

    // Pet 页（浮层打开时任意单击先关闭）
    if (s_list_open) {
        if (ev == BSP_BTN_CLICK) list_show(false);
        return;
    }

    // 任务列表页：上下选择，OK 详情，OK 长按退出
    if (s_page == PAGE_TASKS) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            zcp_tasks_move(&s_tasks_view, -1);
            refresh_tasks_page();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            zcp_tasks_move(&s_tasks_view, 1);
            refresh_tasks_page();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_tasks_view.count > 0) show_page(PAGE_DETAIL);
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            show_page(PAGE_PET);
        }
        return;
    }

    // 详情页：OK 已读该任务，OK 长按在 Mac 打开（继续），上返回
    if (s_page == PAGE_DETAIL) {
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            show_page(PAGE_TASKS);
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_tasks_view.sel < s_tasks_view.count) {
                zcp_bridge_send_read_task(s_tasks_view.sel, s_tasks_view.lv);
                zcp_tasks_mark_read(&s_tasks_view, s_tasks_view.sel);
                lv_label_set_text(s_detail_toast, zcp_str_read_ok);
                s_toast_until_ms = esp_timer_get_time() / 1000 + 1500;
                refresh_detail_page();
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (s_tasks_view.sel < s_tasks_view.count) {
                zcp_bridge_send_go(s_tasks_view.sel, s_tasks_view.lv);
                lv_label_set_text(s_detail_toast, zcp_str_go_sent);
                s_toast_until_ms = esp_timer_get_time() / 1000 + 1500;
            }
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            // 下键在详情页无操作（保留给未来）
        }
        return;
    }

    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            if (zcp_bridge_ble_last_err() != 0) zcp_bridge_restart();   // BLE 失败 → UP=重试
            else list_show(true);
        }
        break;
    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            // 摸头：本地播 pat 2s，纯小情绪，不改计数语义
            s_pat_until_ms = esp_timer_get_time() / 1000 + PAT_PERIOD_MS;
            zcp_pet_set_clip(ZCP_CLIP_PAT);
        }
        break;
    case BSP_BTN_OK:
        if (ev == BSP_BTN_LONG) {
            show_page(PAGE_ABOUT);
        } else if (ev == BSP_BTN_CLICK) {
            if (zcp_bridge_ble_last_err() != 0) {
                zcp_bridge_restart();                  // BLE 失败 → OK=重试
            } else if (s_last_link == ZCP_LINK_ONLINE) {
                show_page(PAGE_TASKS);                 // v1.8：OK = 任务列表（逐任务已读在详情页）
            } else {
                zcp_bridge_drop_peer();                // 非 ONLINE → 蓝牙救援（断 GATT 回广播）
            }
        }
        break;
    default:
        break;
    }
}

void zcp_app_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_RELEASE || ev == BSP_BTN_DOUBLE) return;
    if (!bsp_lvgl_lock(500)) return;
    handle_key(btn, ev);
    bsp_lvgl_unlock();
}

// ---- UI 构建 ----

static lv_obj_t *make_label(lv_obj_t *parent, lv_align_t align, int x, int y,
                            lv_color_t color, const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_align(l, align, x, y);
    label_style(l, color, font);
    lv_label_set_text(l, "");
    return l;
}

// 皮肤选择持久化（NVS 命名空间 zcp；失败不阻塞，仅下次开机回默认）
void zcp_app_save_skin(uint8_t skin) {
    nvs_handle_t h;
    if (nvs_open("zcp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "skin", skin);
    nvs_commit(h);
    nvs_close(h);
}
static void load_saved_skin(void) {
    nvs_handle_t h;
    if (nvs_open("zcp", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t sk = 0;
    nvs_get_u8(h, "skin", &sk);
    nvs_close(h);
    zcp_pet_set_skin(sk);
}

void zcp_app_start(void) {
    load_saved_skin();
    // 字体：生成的闭集子集 + Montserrat fallback（浅拷贝描述符，应用期常驻）
    s_font_cjk = zcp_font_16;
    s_font_cjk.fallback = &lv_font_montserrat_14;

    // 【竞态修复试验】主任务构建 UI 必须持 LVGL 锁（AGENTS.md 约定；此前未拿锁，
    // 与 taskLVGL 的首次刷新竞态——放大版资产改变时序后必现，见 2026-10-20 排障）
    if (!bsp_lvgl_lock(2000)) { ESP_LOGE(TAG, "构建 UI 前拿锁失败"); }
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    // Boot 页
    s_boot_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_boot_box);
    lv_obj_set_size(s_boot_box, BSP_LCD_W, BSP_LCD_H);
    s_boot_title = lv_label_create(s_boot_box);
    lv_obj_align(s_boot_title, LV_ALIGN_CENTER, 0, -20);
    label_style(s_boot_title, lv_color_hex(COL_INK), &lv_font_montserrat_20);
    lv_label_set_text(s_boot_title, zcp_str_boot_title);

    // Pet 页
    s_pet_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_pet_box);
    lv_obj_set_size(s_pet_box, BSP_LCD_W, BSP_LCD_H);
    s_topbar = make_label(s_pet_box, LV_ALIGN_TOP_MID, 0, 8, lv_color_hex(COL_DIM), &s_font_cjk);
    zcp_pet_init(s_pet_box);
    s_caption = make_label(s_pet_box, LV_ALIGN_TOP_MID, 0, 246, lv_color_hex(COL_INK), &s_font_cjk);
    s_subline = make_label(s_pet_box, LV_ALIGN_TOP_MID, 0, 274, lv_color_hex(COL_DIM), &s_font_cjk);
    s_diag = make_label(s_pet_box, LV_ALIGN_TOP_MID, 0, 298, lv_color_hex(COL_ACCENT), &s_font_cjk);
    lv_obj_add_flag(s_pet_box, LV_OBJ_FLAG_HIDDEN);

    // About 页
    s_about_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_about_box);
    lv_obj_set_size(s_about_box, BSP_LCD_W, BSP_LCD_H);
    s_about_title = make_label(s_about_box, LV_ALIGN_TOP_MID, 0, 40, lv_color_hex(COL_ACCENT), &s_font_cjk);
    lv_label_set_text(s_about_title, zcp_str_about_title);
    s_about_name = lv_label_create(s_about_box);
    lv_obj_align(s_about_name, LV_ALIGN_TOP_MID, 0, 100);
    label_style(s_about_name, lv_color_hex(COL_INK), &s_font_cjk);
    lv_label_set_text_fmt(s_about_name, "%s fw1.0", zcp_str_boot_title);
    s_about_batt = make_label(s_about_box, LV_ALIGN_TOP_MID, 0, 140, lv_color_hex(COL_INK), &s_font_cjk);
    s_about_skin = lv_label_create(s_about_box);
    lv_obj_align(s_about_skin, LV_ALIGN_TOP_MID, 0, 180);
    label_style(s_about_skin, lv_color_hex(COL_INK), &s_font_cjk);
    refresh_about_skin();
    s_about_hint = lv_label_create(s_about_box);
    lv_obj_align(s_about_hint, LV_ALIGN_TOP_MID, 0, 240);
    label_style(s_about_hint, lv_color_hex(COL_DIM), &s_font_cjk);
    lv_label_set_text(s_about_hint, zcp_str_about_hint2);
    lv_obj_add_flag(s_about_box, LV_OBJ_FLAG_HIDDEN);

    // 任务列表页（8 行静态 label，选中行高亮橙色，未读前缀 ●）
    s_tasks_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_tasks_box);
    lv_obj_set_size(s_tasks_box, BSP_LCD_W, BSP_LCD_H);
    s_tasks_title = make_label(s_tasks_box, LV_ALIGN_TOP_MID, 0, 8, lv_color_hex(COL_ACCENT), &s_font_cjk);
    lv_label_set_text(s_tasks_title, zcp_str_tasks_title);
    for (int i = 0; i < ZCP_TASKS_MAX; i++) {
        s_tasks_rows[i] = make_label(s_tasks_box, LV_ALIGN_TOP_LEFT, 12, 40 + i * 28,
                                     lv_color_hex(COL_INK), &s_font_cjk);
        lv_label_set_text(s_tasks_rows[i], "");
    }
    s_tasks_hint = make_label(s_tasks_box, LV_ALIGN_TOP_MID, 0, 296, lv_color_hex(COL_DIM), &s_font_cjk);
    lv_label_set_text(s_tasks_hint, zcp_str_list_hint);
    lv_obj_add_flag(s_tasks_box, LV_OBJ_FLAG_HIDDEN);

    // 任务详情页：标题（自动换行）+ 状态 + 提示 + 底部操作栏
    s_detail_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_detail_box);
    lv_obj_set_size(s_detail_box, BSP_LCD_W, BSP_LCD_H);
    s_detail_title = make_label(s_detail_box, LV_ALIGN_TOP_LEFT, 12, 40, lv_color_hex(COL_INK), &s_font_cjk);
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail_title, BSP_LCD_W - 24);
    s_detail_status = make_label(s_detail_box, LV_ALIGN_TOP_LEFT, 12, 170, lv_color_hex(COL_DIM), &s_font_cjk);
    s_detail_toast = make_label(s_detail_box, LV_ALIGN_TOP_MID, 0, 210, lv_color_hex(COL_ACCENT), &s_font_cjk);
    s_detail_hint = make_label(s_detail_box, LV_ALIGN_TOP_MID, 0, 296, lv_color_hex(COL_DIM), &s_font_cjk);
    lv_label_set_text(s_detail_hint, zcp_str_detail_hint);
    lv_obj_add_flag(s_detail_box, LV_OBJ_FLAG_HIDDEN);

    // 任务清单浮层
    s_list_box = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_box, 200, 64);
    lv_obj_align(s_list_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(0x1A1A22), 0);
    lv_obj_set_style_bg_opa(s_list_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_list_box, 12, 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_scrollbar_mode(s_list_box, LV_SCROLLBAR_MODE_OFF);
    s_list_label = lv_label_create(s_list_box);
    lv_obj_center(s_list_label);
    label_style(s_list_label, lv_color_hex(COL_INK), &s_font_cjk);
    lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);

    s_boot_timer = lv_timer_create(boot_timer_cb, BOOT_PERIOD_MS, NULL);
    lv_timer_set_repeat_count(s_boot_timer, 1);
    s_anim_timer = lv_timer_create(anim_timer_cb, 1000 / ZCP_ZPF_FPS, NULL);
    s_state_timer = lv_timer_create(state_timer_cb, STATE_PERIOD_MS, NULL);
    bsp_lvgl_unlock();                       // UI 全部就绪，放锁再起 BLE

    // 服务（BLE 桥接常驻广播，无需配网/凭据；不碰 LVGL，无需持锁）
    zcp_bridge_start();

    ESP_LOGI(TAG, "zcp_app 就绪");
}

void zcp_app_set_battery_available(bool available) {
    s_batt_available = available;
}
