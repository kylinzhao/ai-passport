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
    if (memcmp(data, "ZPF2", 4) != 0) return -3;
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

// 帧数据的起始偏移（u32 payloadLen 前缀）；越界返回 -2。
static int frame_offset(const zcp_zpf_t *z, uint16_t frame_idx, size_t *offset, size_t *len) {
    const uint8_t *p = z->data + 76;
    const uint8_t *end = z->data + z->size;
    for (uint16_t f = 0; f <= frame_idx; f++) {
        if ((size_t)(end - p) < 4) return -2;
        uint32_t plen = rd_u32(p);
        p += 4;
        if ((size_t)(end - p) < plen) return -2;
        if (f == frame_idx) {
            *offset = (size_t)(p - z->data);
            *len = plen;
            return 0;
        }
        p += plen;
    }
    return -1;
}

int zcp_zpf_render_frame(const zcp_zpf_t *z, uint16_t frame_idx, uint16_t *dst, uint16_t bg) {
    if (!z || !z->data || !dst || frame_idx >= z->frames) return -1;

    size_t off, len;
    int rc = frame_offset(z, frame_idx, &off, &len);
    if (rc != 0) return rc;
    const uint8_t *payload = z->data + off;
    const uint8_t *end = payload + len;
    const size_t total = (size_t)z->w * z->h;

    // 帧内布局：先整段掩码 RLE（每游程 1 字节，覆盖全部像素），后接 8bpp 索引流。
    // 第一遍只走 RLE 找到索引流起点；第二遍重走 RLE，不透明游程逐像素消费索引。
    const uint8_t *rle = payload;
    const uint8_t *p = rle;
    size_t covered = 0;
    while (covered < total) {
        if (p >= end) return -2;
        unsigned run = (unsigned)(*p & 0x7F) + 1;
        p++;
        if (covered + run > total) return -3;             // 游程越界
        covered += run;
    }
    const uint8_t *idx = p;

    p = rle;
    covered = 0;
    while (covered < total) {
        unsigned run = (unsigned)(*p & 0x7F) + 1;
        bool opaque_run = (*p & 0x80) != 0;
        p++;
        if (opaque_run) {
            for (unsigned k = 0; k < run; k++) {
                if (idx >= end) return -2;                // 索引流不足
                uint8_t v = *idx++;
                if (v == 0 || v >= 32) return -4;         // index0=透明占位，不入索引流
                dst[covered + k] = z->palette[v];
            }
        } else {
            for (unsigned k = 0; k < run; k++) dst[covered + k] = bg;
        }
        covered += run;
    }
    return 0;
}
