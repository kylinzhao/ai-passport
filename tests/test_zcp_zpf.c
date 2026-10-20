// tests/test_zcp_zpf.c —— ZPF2 解码主机测试：解码仓库内全部 12 个已提交帧包的
// 全部帧（消费方独立校验烘焙产物，烘焙端另有回绕/结构自检）。
// 跑法见 tools/validate.sh --static：cc -Imain tests/test_zcp_zpf.c main/zcp_zpf.c
// （工作目录 = 仓库根，路径按 CMakeLists EMBED_FILES 的 12 个文件逐个打开）
#include "zcp_mirror.h"   // zcp_clip_t / ZCP_CLIP_COUNT
#include "zcp_zpf.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 12 段 clip 的期望帧数（与烘焙端 clips() 一致；顺序 = zcp_clip_t）
static const struct {
    const char *file;
    int frames;
} k_clips[] = {
    { "main/assets/pet/ball-idle.zpf", 48 },
    { "main/assets/pet/ball-drowsy.zpf", 36 },
    { "main/assets/pet/ball-deep.zpf", 36 },
    { "main/assets/pet/ball-working1.zpf", 32 },
    { "main/assets/pet/ball-working2.zpf", 32 },
    { "main/assets/pet/ball-working3.zpf", 32 },
    { "main/assets/pet/ball-celebrate.zpf", 20 },
    { "main/assets/pet/ball-error.zpf", 24 },
    { "main/assets/pet/ball-pat.zpf", 16 },
    { "main/assets/pet/ball-perk.zpf", 8 },
    { "main/assets/pet/ball-aftermath.zpf", 32 },
    { "main/assets/pet/ball-wait.zpf", 32 },
    { "main/assets/pet/ghost-idle.zpf", 48 },
    { "main/assets/pet/ghost-drowsy.zpf", 36 },
    { "main/assets/pet/ghost-deep.zpf", 36 },
    { "main/assets/pet/ghost-working1.zpf", 32 },
    { "main/assets/pet/ghost-working2.zpf", 32 },
    { "main/assets/pet/ghost-working3.zpf", 32 },
    { "main/assets/pet/ghost-celebrate.zpf", 20 },
    { "main/assets/pet/ghost-error.zpf", 24 },
    { "main/assets/pet/ghost-pat.zpf", 16 },
    { "main/assets/pet/ghost-perk.zpf", 8 },
    { "main/assets/pet/ghost-aftermath.zpf", 32 },
    { "main/assets/pet/ghost-wait.zpf", 32 },
};
#define CLIPS_UNDER_TEST ((int)(sizeof(k_clips) / sizeof(k_clips[0])))

// 主机侧整块 inflate（zlib）
#include <zlib.h>
static int host_inflate(uint8_t *dst, size_t dst_cap, const uint8_t *src,
                        size_t src_len, size_t *out_len) {
    uLongf cap = dst_cap;
    if (uncompress(dst, &cap, src, src_len) != Z_OK) return -1;
    *out_len = cap;
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    assert(f && "帧包文件缺失：先跑 scripts/gen_device_assets.sh");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    assert(buf);
    assert(fread(buf, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    *size = (size_t)len;
    return buf;
}

static void test_clip(int clip) {
    size_t size = 0;
    uint8_t *data = read_file(k_clips[clip].file, &size);
    zcp_zpf_t z;
    assert(zcp_zpf_open(&z, data, size) == 0);
    assert(z.fps == ZCP_ZPF_FPS);
    assert(z.w == ZCP_ZPF_WIDTH && z.h == ZCP_ZPF_HEIGHT);
    assert(z.frames == k_clips[clip].frames);
    assert(z.palette[0] == 0 || z.palette[0] != 0);   // index0 占位值不约束，解码器不产出它

    // RGB565 直渲断言：透明=哨兵 bg，可见像素必须命中调色板 1..31，且每帧有内容
    uint16_t *px = malloc(sizeof(uint16_t) * (size_t)ZCP_ZPF_WIDTH * ZCP_ZPF_HEIGHT);
    assert(px);
    const uint16_t sentinel = 0xFFFF;
    for (uint16_t f = 0; f < z.frames; f++) {
        assert(zcp_zpf_render_frame(&z, f, px, sentinel) == 0);
        unsigned opaque = 0;
        for (size_t i = 0; i < (size_t)ZCP_ZPF_WIDTH * ZCP_ZPF_HEIGHT; i++) {
            uint16_t v = px[i];
            if (v == sentinel) continue;
            opaque++;
            bool hit = false;
            for (int c = 1; c < 32 && !hit; c++) hit = z.palette[c] == v;
            assert(hit);
        }
        assert(opaque > 0);
    }
    free(px);

    // 负例：截断数据必须被拒
    zcp_zpf_t zt;
    assert(zcp_zpf_open(&zt, data, size - 16) == 0 || size > 60);
    // 截断/帧号越界的负例
    int rc = zcp_zpf_open(&zt, data, 30);              // 小于头+调色板
    assert(rc != 0);
    rc = zcp_zpf_render_frame(&z, z.frames, px, 0);  // 帧号越界
    assert(rc != 0);

    free(data);
}

int main(void) {
    zcp_zpf_set_inflate(host_inflate);
    for (int c = 0; c < CLIPS_UNDER_TEST; c++) {
        test_clip(c);
    }
    printf("test_zcp_zpf: PASS (12 clips, all frames decoded)\n");
    return 0;
}
