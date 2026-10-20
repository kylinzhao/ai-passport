// main/zcp_zpf.c —— ZPF2 解码实现（纯 C，主机与固件共用同一份）。
#include "zcp_zpf.h"

#include <string.h>

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int zcp_zpf_open(zcp_zpf_t *z, const uint8_t *data, size_t size) {
    if (!z || !data) return -1;
    memset(z, 0, sizeof(*z));
    if (size < 76) return -2;                             // 头 + 调色板
    if (memcmp(data, "ZPF4", 4) != 0) return -3;
    z->fps = data[4];
    if (z->fps != ZCP_ZPF_FPS) return -4;
    z->w = rd_u16(data + 6);
    z->h = rd_u16(data + 8);
    z->frames = rd_u16(data + 10);
    if (z->w != ZCP_ZPF_WIDTH || z->h != ZCP_ZPF_HEIGHT) return -5;
    if (z->frames == 0 || z->frames > ZCP_ZPF_MAX_FRAMES) return -6;
    for (int i = 0; i < 32; i++) {
        z->palette[i] = rd_u16(data + 12 + i * 2);
    }
    z->data = data;
    z->size = size;
    return 0;
}

// 帧定位（布局与烘焙端一致）：u32 maskRaw | u32 maskComp | zlib块 | u8 bandCount(=8)
// | 带块×8（每带 u32 raw | u32 comp | zlib块）。返回掩码块与带区位置；越界 -2。
static int frame_locate(const zcp_zpf_t *z, uint16_t frame_idx, size_t *mask_off,
                        size_t *mask_comp_len, size_t *mask_raw_len, size_t *bands_off) {
    const uint8_t *p = z->data + 76;
    const uint8_t *end = z->data + z->size;
    for (uint16_t f = 0; f <= frame_idx; f++) {
        if ((size_t)(end - p) < 8) return -2;
        uint32_t rl = rd_u32(p), cl = rd_u32(p + 4);
        p += 8;
        if ((size_t)(end - p) < cl + 1) return -2;    // zlib 块 + bandCount 字节
        const uint8_t nb = p[cl];
        if (f == frame_idx) {
            *mask_raw_len = rl;
            *mask_comp_len = cl;
            *mask_off = (size_t)(p - z->data);
            *bands_off = (size_t)(p - z->data) + cl + 1;
            if (nb != ZCP_ZPF_BANDS) return -2;       // 带数契约
            return 0;
        }
        p += cl + 1;                                  // 跳 zlib 块 + bandCount
        for (uint8_t b = 0; b < nb; b++) {
            if ((size_t)(end - p) < 8) return -2;
            uint32_t brl = rd_u32(p), bcl = rd_u32(p + 4);
            p += 8;
            if (brl > ZCP_ZPF_MAX_BAND_RAW || (size_t)(end - p) < bcl) return -2;
            p += bcl;
        }
    }
    return -1;
}

static zcp_inflate_fn s_inflate;                       // 使用方注入（固件 tinfl / 测试 zlib）
static uint8_t s_mask[ZCP_ZPF_MAX_MASK_RAW];           // 解压掩码游程
static uint8_t s_band[ZCP_ZPF_MAX_BAND_RAW];           // 当前带索引

void zcp_zpf_set_inflate(zcp_inflate_fn fn) { s_inflate = fn; }

int zcp_zpf_render_frame(const zcp_zpf_t *z, uint16_t frame_idx, uint16_t *dst, uint16_t bg565) {
    if (!z || !z->data || !dst || frame_idx >= z->frames) return -1;
    if (!s_inflate) return -5;                        // 未注入 inflate

    size_t mask_off, mask_cl, mask_rl, bands_off;
    int rc = frame_locate(z, frame_idx, &mask_off, &mask_cl, &mask_rl, &bands_off);
    if (rc != 0) return rc;
    if (mask_rl == 0 || mask_rl > sizeof(s_mask)) return -6;
    size_t got = 0;
    if (s_inflate(s_mask, sizeof(s_mask), z->data + mask_off, mask_cl, &got) != 0
        || got != mask_rl) return -7;

    const size_t total = (size_t)z->w * z->h;
    const int band_h = z->h / ZCP_ZPF_BANDS;
    const uint8_t *p = z->data + bands_off;

    // 逐带：解索引块 → 遍历掩码游程渲带内像素
    for (int b = 0; b < ZCP_ZPF_BANDS; b++) {
        if ((size_t)(z->data + z->size - p) < 8) return -2;
        uint32_t brl = rd_u32(p), bcl = rd_u32(p + 4);
        p += 8;
        if (brl > sizeof(s_band)) return -6;
        if (s_inflate(s_band, sizeof(s_band), p, bcl, &got) != 0 || got != brl) return -7;
        p += bcl;

        const int y0 = b * band_h;
        const int y1 = (b == ZCP_ZPF_BANDS - 1) ? z->h : (b + 1) * band_h;

        // 游程遍历：像素 pos 顺序累计；带内 opaque 像素消费带索引，透明写 bg
        size_t pos = 0;
        const uint8_t *mp = s_mask;
        const uint8_t *bi = s_band;
        const uint8_t *bi_end = s_band + brl;
        while (pos < total) {
            unsigned run = (unsigned)(*mp & 0x7F) + 1;
            bool opaque_run = (*mp & 0x80) != 0;
            mp++;
            int py0 = (int)(pos / z->w), py1 = (int)((pos + run - 1) / z->w);
            if (py1 < y0 || py0 >= y1) {              // 游程整段在带外
                pos += run;
                continue;
            }
            for (unsigned k = 0; k < run; k++) {
                size_t at = pos + k;
                int py = (int)(at / z->w);
                if (py < y0 || py >= y1) continue;    // 带外像素（游程跨界）
                if (opaque_run) {
                    if (bi >= bi_end) return -4;      // 带索引不足
                    uint8_t v = *bi++;
                    if (v == 0 || v >= 32) return -4;
                    dst[at] = z->palette[v];
                } else {
                    dst[at] = bg565;
                }
            }
            pos += run;
        }
        if (bi != bi_end) return -4;                  // 带索引有剩余（数据不一致）
    }
    return 0;
}
