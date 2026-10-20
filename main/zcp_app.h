// main/zcp_app.h —— zcode-pet 玩法入口（页面状态机 + 三键路由，UI 全新设计，
// 不复用 demo 测试菜单外壳）。生命周期仿 demo.h 的 enter/exit/key 约定：
// 所有 lv_* 调用发生在 LVGL 任务或持 bsp_lvgl_lock() 的调用方。
#pragma once

#include <stdbool.h>

#include "bsp_button.h"

// BSP/LVGL 就绪后由 main.c 调用一次：建 UI、启动动画/状态定时器、拉起 BLE 桥接
// （NimBLE 常驻广播）。内部失败（资源不足等）只记日志，不重启系统。
void zcp_app_start(void);
void zcp_app_save_skin(uint8_t skin);

// 按键事件入口（main.c 的 input task 调用；内部自行获取/释放 LVGL 锁）。
void zcp_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 电池探测结果（main.c 在 bsp_battery_init 后调用；false 时 About 页电量行显示「?」）。
void zcp_app_set_battery_available(bool available);
