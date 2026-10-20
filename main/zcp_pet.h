// main/zcp_pet.h —— .zpf 帧播放器（解码 + LVGL draw-event 流式 blit）。
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "zcp_mirror.h"

int zcp_pet_skin_count(void);
uint8_t zcp_pet_skin(void);
void zcp_pet_set_skin(uint8_t skin);   // 切皮肤并重载当前动画（NVS 持久化由调用方做）

#ifdef __cplusplus
extern "C" {
#endif

// 建宠物 image 对象（236×152，半分辨率帧 2× 最近邻上采样进静态 RGB565 缓冲，
// 以 LV_IMAGE_SRC_VARIABLE 喂给 lv_image；透明像素填压平底色，与页面底色同色）。
// 渲染走 LVGL 9.5 的 image draw task：分块渲染缓冲（BSP 单缓冲 40 行）由 SW
// draw unit 内部处理，应用侧内存恒定。
void zcp_pet_init(lv_obj_t *parent);

// 切换 clip（zcp_clip_t）。相同 clip 是空操作；会立即解码第 0 帧并刷新。
void zcp_pet_set_clip(int clip_id);

// 8fps 调一次（LVGL timer）：前进一帧并 invalidate 宠物区。
void zcp_pet_tick(void);

// 当前 clip 的帧数（Boot 页定时播 perk 1.2s 等场景用）。
int zcp_pet_clip_frames(int clip_id);

#ifdef __cplusplus
}
#endif
