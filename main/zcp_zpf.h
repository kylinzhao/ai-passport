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

// 直渲第 frame_idx 帧进调用方提供的 RGB565 缓冲（W×H×2 字节，行优先）：
// 不透明像素写调色板色，透明像素写 bg。跳过 mask/idx 中间结构——无 PSRAM 的 C3
// 上省下 ~52KB 内部 RAM（2026-10-20 NimBLE gatts_init 断言排障的根治项）。
// 游程与索引流经校验，越界/损坏数据返回非 0。0=成功。
int zcp_zpf_render_frame(const zcp_zpf_t *z, uint16_t frame_idx, uint16_t *dst, uint16_t bg);

#ifdef __cplusplus
}
#endif
