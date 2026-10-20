// main/zcp_zpf.h —— ZPF4 帧包解码（纯 C；inflate 由使用方注入：固件=ROM tinfl，主机测试=zlib）。
//
// ZPF4 布局（小端；与烘焙端 zcode-pet daemon/main.swift Esp32Exporter 注释契约逐字节一致；
// ZPF1-3 已退役，magic 拒收）：
//   偏移 0  : magic "ZPF4"
//   偏移 4  : fps (u8) —— 8 | 偏移 5: 保留 0
//   偏移 6  : width/height (u16×2) —— 236×196 | 偏移 10: frameCount (u16)
//   偏移 12 : 调色板 32×RGB565；index0 = 透明占位
//   偏移 76 : 每帧：u32 maskRawLen | u32 maskCompLen | zlib(掩码 RLE)
//             u8 bandCount(=8) | 每带：u32 rawLen | u32 compLen | zlib(带索引流)
//             带边界 = 等高行带（height/8 行）；带索引只含该带不透明像素（行优先）
//   8 行带分块使设备端静态解压缓冲仅需 4KB/带 + 2KB 掩码（ZPF3 整帧 24KB 缓冲会把
//   BLE 挤到 3KB 堆；ZPF1/2 无压缩体积超分区）。
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

#define ZCP_ZPF_MAX_MASK_RAW (2 * 1024)     // 掩码段解压上限（=烘焙端 maxMaskRaw）
#define ZCP_ZPF_MAX_BAND_RAW (4 * 1024)     // 单带索引解压上限（=烘焙端 maxBandRaw）
#define ZCP_ZPF_BANDS 8                     // 行带数（=烘焙端 bandCount）

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

// 注入式整块 inflate：zlib 流解到 ≥rawLen 的缓冲，out_len=实际长度；0=成功。
// 固件实现必须用静态解压器（ROM tinfl 的 mem_to_mem 把 ~10KB 解压器放调用者
// 栈上，main/LVGL 任务栈必爆——真机 Guru Stack protection fault，2026-10-20）。
typedef int (*zcp_inflate_fn)(uint8_t *dst, size_t dst_cap, const uint8_t *src,
                              size_t src_len, size_t *out_len);
void zcp_zpf_set_inflate(zcp_inflate_fn fn);

// 解压+直渲第 frame_idx 帧（RGB565；透明像素写 bg565，不透明写调色板色）。
// 掩码游程与带索引经校验，失败返回非 0。0=成功。
int zcp_zpf_render_frame(const zcp_zpf_t *z, uint16_t frame_idx, uint16_t *dst, uint16_t bg565);

#ifdef __cplusplus
}
#endif
