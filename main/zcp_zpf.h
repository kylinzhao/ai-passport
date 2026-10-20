// main/zcp_zpf.h —— ZPF2 帧包解码（纯 C，无 ESP-IDF/LVGL 依赖，主机可测）。
//
// ZPF2 布局（小端；与烘焙端 zcode-pet daemon/main.swift Esp32Exporter 的注释契约
// 逐字节一致，改任何字段两侧同步；ZPF1 已退役，magic 拒收）：
//   偏移 0  : magic "ZPF2"
//   偏移 4  : fps (u8)                     —— 8
//   偏移 5  : 保留 (u8) = 0
//   偏移 6  : width (u16 LE)               —— 236
//   偏移 8  : height (u16 LE)              —— 196
//   偏移 10 : frameCount (u16 LE)
//   偏移 12 : 调色板 32 × RGB565 (u16 LE)；index 0 = 透明占位（不出现在索引流），
//             index 31 = 压平底色 #1A1A1A（设备宠物页底色必须同色）
//   偏移 76 : 逐帧数据：u32 payloadLen (LE) + payload：
//               掩码 RLE（u8：bit7=不透明值，bit6..0=游程-1，游程覆盖 W×H 全部像素，
//               行优先自顶向下）+ 不透明像素 8bpp 调色板索引流（行优先，每像素 1 字节）。
//   全分辨率 236×196 直出（设备端 1:1 blit 上屏，无上采样），烘焙端带 1.25× 居中变焦；量化带 Bayer 抖动。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCP_ZPF_WIDTH 236
#define ZCP_ZPF_HEIGHT 196
#define ZCP_ZPF_FPS 8
#define ZCP_ZPF_MAX_FRAMES 512

typedef struct {
    const uint8_t *data;
    size_t size;
    uint16_t palette[32];   // RGB565
    uint16_t w, h;
    uint16_t frames;
    uint8_t fps;
} zcp_zpf_t;

// 校验头并解析常量字段；数据必须在全部生命周期内有效（flash 映射）。0=成功。
int zcp_zpf_open(zcp_zpf_t *z, const uint8_t *data, size_t size);

// 单帧解码出的位图（调用方提供缓冲）。
typedef struct {
    uint8_t mask[ZCP_ZPF_WIDTH * ZCP_ZPF_HEIGHT / 8];  // 行主序自顶向下，1=不透明
    uint8_t idx[ZCP_ZPF_WIDTH * ZCP_ZPF_HEIGHT];       // 不透明像素的调色板索引（按掩码顺序）
} zcp_zpf_frame_t;

// 解码第 frame_idx 帧；游程与索引经校验。0=成功。
int zcp_zpf_decode_frame(const zcp_zpf_t *z, uint16_t frame_idx, zcp_zpf_frame_t *out);

static inline bool zcp_zpf_mask_get(const zcp_zpf_frame_t *f, uint16_t x, uint16_t y) {
    size_t i = (size_t)y * ZCP_ZPF_WIDTH + x;
    return (f->mask[i >> 3] >> (i & 7)) & 1;
}

#ifdef __cplusplus
}
#endif
