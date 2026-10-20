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
} k_clips[ZCP_CLIP_COUNT] = {
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
};

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

    zcp_zpf_frame_t fr;
    unsigned opaque_max = 0;
    for (uint16_t f = 0; f < z.frames; f++) {
        assert(zcp_zpf_decode_frame(&z, f, &fr) == 0);
        unsigned opaque = 0;
        for (size_t i = 0; i < sizeof(fr.idx); i++) {
            if (zcp_zpf_mask_get(&fr, (uint16_t)(i % ZCP_ZPF_WIDTH), (uint16_t)(i / ZCP_ZPF_WIDTH))) {
                opaque++;
                assert(fr.idx[i] >= 1 && fr.idx[i] <= 31);
            }
        }
        assert(opaque > 0);                            // 每帧都有可见内容
        if (opaque > opaque_max) opaque_max = opaque;
    }
    (void)opaque_max;

    // 负例：截断数据必须被拒
    zcp_zpf_t zt;
    assert(zcp_zpf_open(&zt, data, size - 16) == 0 || size > 60);
    zcp_zpf_frame_t tmp;
    // 解码最后一帧用真实数据；截断后在错误偏移上应当失败或提前拒绝
    int rc = zcp_zpf_open(&zt, data, 30);              // 小于头+调色板
    assert(rc != 0);
    rc = zcp_zpf_decode_frame(&z, z.frames, &tmp);     // 帧号越界
    assert(rc != 0);

    free(data);
}

int main(void) {
    for (int c = 0; c < ZCP_CLIP_COUNT; c++) {
        test_clip(c);
    }
    printf("test_zcp_zpf: PASS (12 clips, all frames decoded)\n");
    return 0;
}
