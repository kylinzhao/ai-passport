// main/zcp_pet.c —— 帧播放器实现（LVGL 9.5 正统管线）。
//
// 内存设计：ZPF1 半分辨率帧（118×76）解码后以 2× 最近邻展开进一块静态 RGB565
// 缓冲（236×196，约 92KB 内部 RAM），作为 LV_IMAGE_SRC_VARIABLE 喂给 lv_image；
// 透明像素直接填压平底色 ZCP_BG_HEX 的 RGB565（0x18C3）——页面底色同色，视觉等价
// 于透明。LVGL 9.5 的绘制走任务队列（DRAW_MAIN 事件里直写像素会被先入队的底色
// 任务覆盖），所以必须走 image draw task：分块渲染缓冲（BSP 单缓冲 40 行）由
// SW draw unit 内部处理，应用内存恒定。切帧在 LVGL 任务里解码+展开（8fps 一次，
// 微秒级）。
#include "zcp_pet.h"

#include <string.h>

#include "bsp_pins.h"
#include "esp_log.h"
#include "zcp_mirror.h"
#include "zcp_zpf.h"

static const char *TAG = "zcp_pet";

#define PET_W ZCP_ZPF_WIDTH                       // 236（ZPF2 全分辨率，1:1 上屏）
#define PET_H ZCP_ZPF_HEIGHT                      // 152

// RGB565(0x1A1A1A)：烘焙压平底色，透明像素填它即与页面底色无缝
#define BG_RGB565 0x18C3

// EMBED_FILES 的 12 个帧包（CMake 逐个显式列出，顺序与 zcp_clip_t 一致；
// 符号名 = 文件名展平，规则见 esp-idf data_file_embed_asm.cmake）
extern const uint8_t ball_idle_zpf_start[] asm("_binary_ball_idle_zpf_start");
extern const uint8_t ball_idle_zpf_end[] asm("_binary_ball_idle_zpf_end");
extern const uint8_t ball_drowsy_zpf_start[] asm("_binary_ball_drowsy_zpf_start");
extern const uint8_t ball_drowsy_zpf_end[] asm("_binary_ball_drowsy_zpf_end");
extern const uint8_t ball_deep_zpf_start[] asm("_binary_ball_deep_zpf_start");
extern const uint8_t ball_deep_zpf_end[] asm("_binary_ball_deep_zpf_end");
extern const uint8_t ball_working1_zpf_start[] asm("_binary_ball_working1_zpf_start");
extern const uint8_t ball_working1_zpf_end[] asm("_binary_ball_working1_zpf_end");
extern const uint8_t ball_working2_zpf_start[] asm("_binary_ball_working2_zpf_start");
extern const uint8_t ball_working2_zpf_end[] asm("_binary_ball_working2_zpf_end");
extern const uint8_t ball_working3_zpf_start[] asm("_binary_ball_working3_zpf_start");
extern const uint8_t ball_working3_zpf_end[] asm("_binary_ball_working3_zpf_end");
extern const uint8_t ball_celebrate_zpf_start[] asm("_binary_ball_celebrate_zpf_start");
extern const uint8_t ball_celebrate_zpf_end[] asm("_binary_ball_celebrate_zpf_end");
extern const uint8_t ball_error_zpf_start[] asm("_binary_ball_error_zpf_start");
extern const uint8_t ball_error_zpf_end[] asm("_binary_ball_error_zpf_end");
extern const uint8_t ball_pat_zpf_start[] asm("_binary_ball_pat_zpf_start");
extern const uint8_t ball_pat_zpf_end[] asm("_binary_ball_pat_zpf_end");
extern const uint8_t ball_perk_zpf_start[] asm("_binary_ball_perk_zpf_start");
extern const uint8_t ball_perk_zpf_end[] asm("_binary_ball_perk_zpf_end");
extern const uint8_t ball_aftermath_zpf_start[] asm("_binary_ball_aftermath_zpf_start");
extern const uint8_t ball_aftermath_zpf_end[] asm("_binary_ball_aftermath_zpf_end");
extern const uint8_t ball_wait_zpf_start[] asm("_binary_ball_wait_zpf_start");
extern const uint8_t ball_wait_zpf_end[] asm("_binary_ball_wait_zpf_end");

extern const uint8_t ghost_idle_zpf_start[] asm("_binary_ghost_idle_zpf_start");
extern const uint8_t ghost_idle_zpf_end[] asm("_binary_ghost_idle_zpf_end");
extern const uint8_t ghost_drowsy_zpf_start[] asm("_binary_ghost_drowsy_zpf_start");
extern const uint8_t ghost_drowsy_zpf_end[] asm("_binary_ghost_drowsy_zpf_end");
extern const uint8_t ghost_deep_zpf_start[] asm("_binary_ghost_deep_zpf_start");
extern const uint8_t ghost_deep_zpf_end[] asm("_binary_ghost_deep_zpf_end");
extern const uint8_t ghost_working1_zpf_start[] asm("_binary_ghost_working1_zpf_start");
extern const uint8_t ghost_working1_zpf_end[] asm("_binary_ghost_working1_zpf_end");
extern const uint8_t ghost_working2_zpf_start[] asm("_binary_ghost_working2_zpf_start");
extern const uint8_t ghost_working2_zpf_end[] asm("_binary_ghost_working2_zpf_end");
extern const uint8_t ghost_working3_zpf_start[] asm("_binary_ghost_working3_zpf_start");
extern const uint8_t ghost_working3_zpf_end[] asm("_binary_ghost_working3_zpf_end");
extern const uint8_t ghost_celebrate_zpf_start[] asm("_binary_ghost_celebrate_zpf_start");
extern const uint8_t ghost_celebrate_zpf_end[] asm("_binary_ghost_celebrate_zpf_end");
extern const uint8_t ghost_error_zpf_start[] asm("_binary_ghost_error_zpf_start");
extern const uint8_t ghost_error_zpf_end[] asm("_binary_ghost_error_zpf_end");
extern const uint8_t ghost_pat_zpf_start[] asm("_binary_ghost_pat_zpf_start");
extern const uint8_t ghost_pat_zpf_end[] asm("_binary_ghost_pat_zpf_end");
extern const uint8_t ghost_perk_zpf_start[] asm("_binary_ghost_perk_zpf_start");
extern const uint8_t ghost_perk_zpf_end[] asm("_binary_ghost_perk_zpf_end");
extern const uint8_t ghost_aftermath_zpf_start[] asm("_binary_ghost_aftermath_zpf_start");
extern const uint8_t ghost_aftermath_zpf_end[] asm("_binary_ghost_aftermath_zpf_end");
extern const uint8_t ghost_wait_zpf_start[] asm("_binary_ghost_wait_zpf_start");
extern const uint8_t ghost_wait_zpf_end[] asm("_binary_ghost_wait_zpf_end");

typedef struct { const uint8_t *start, *end; } zcp_blob_t;

static const zcp_blob_t s_ball_clips[ZCP_CLIP_COUNT] = {
    [ZCP_CLIP_IDLE] = { ball_idle_zpf_start, ball_idle_zpf_end },
    [ZCP_CLIP_DROWSY] = { ball_drowsy_zpf_start, ball_drowsy_zpf_end },
    [ZCP_CLIP_DEEP] = { ball_deep_zpf_start, ball_deep_zpf_end },
    [ZCP_CLIP_WORKING1] = { ball_working1_zpf_start, ball_working1_zpf_end },
    [ZCP_CLIP_WORKING2] = { ball_working2_zpf_start, ball_working2_zpf_end },
    [ZCP_CLIP_WORKING3] = { ball_working3_zpf_start, ball_working3_zpf_end },
    [ZCP_CLIP_CELEBRATE] = { ball_celebrate_zpf_start, ball_celebrate_zpf_end },
    [ZCP_CLIP_ERROR] = { ball_error_zpf_start, ball_error_zpf_end },
    [ZCP_CLIP_PAT] = { ball_pat_zpf_start, ball_pat_zpf_end },
    [ZCP_CLIP_PERK] = { ball_perk_zpf_start, ball_perk_zpf_end },
    [ZCP_CLIP_AFTERMATH] = { ball_aftermath_zpf_start, ball_aftermath_zpf_end },
    [ZCP_CLIP_WAIT] = { ball_wait_zpf_start, ball_wait_zpf_end },
};

static const zcp_blob_t s_ghost_clips[ZCP_CLIP_COUNT] = {
    [ZCP_CLIP_IDLE] = { ghost_idle_zpf_start, ghost_idle_zpf_end },
    [ZCP_CLIP_DROWSY] = { ghost_drowsy_zpf_start, ghost_drowsy_zpf_end },
    [ZCP_CLIP_DEEP] = { ghost_deep_zpf_start, ghost_deep_zpf_end },
    [ZCP_CLIP_WORKING1] = { ghost_working1_zpf_start, ghost_working1_zpf_end },
    [ZCP_CLIP_WORKING2] = { ghost_working2_zpf_start, ghost_working2_zpf_end },
    [ZCP_CLIP_WORKING3] = { ghost_working3_zpf_start, ghost_working3_zpf_end },
    [ZCP_CLIP_CELEBRATE] = { ghost_celebrate_zpf_start, ghost_celebrate_zpf_end },
    [ZCP_CLIP_ERROR] = { ghost_error_zpf_start, ghost_error_zpf_end },
    [ZCP_CLIP_PAT] = { ghost_pat_zpf_start, ghost_pat_zpf_end },
    [ZCP_CLIP_PERK] = { ghost_perk_zpf_start, ghost_perk_zpf_end },
    [ZCP_CLIP_AFTERMATH] = { ghost_aftermath_zpf_start, ghost_aftermath_zpf_end },
    [ZCP_CLIP_WAIT] = { ghost_wait_zpf_start, ghost_wait_zpf_end },
};

#define ZCP_SKIN_COUNT 2
static const struct {
    const zcp_blob_t *clips;
    const char *name;
} s_skins[ZCP_SKIN_COUNT] = {
    { s_ball_clips, "ball" },
    { s_ghost_clips, "ghost" },
};
static uint8_t s_skin;                              // 当前皮肤下标（NVS 持久化由 app 层做）

static lv_obj_t *s_img;
static zcp_zpf_t s_zpf;                              // 当前 clip
static uint16_t s_up[PET_W * PET_H];                 // RGB565 帧缓冲（约 92KB 静态）
static lv_image_dsc_t s_img_dsc;
static uint16_t s_frame_idx;
static int s_clip_id = -1;
static bool s_ready;

int zcp_pet_clip_frames(int clip_id) {
    if (clip_id < 0 || clip_id >= ZCP_CLIP_COUNT) return 0;
    zcp_zpf_t tmp;
    const zcp_blob_t *tab = s_skins[s_skin].clips;
    const uint8_t *data = tab[clip_id].start;
    size_t size = (size_t)(tab[clip_id].end - tab[clip_id].start);
    if (zcp_zpf_open(&tmp, data, size) != 0) return 0;
    return tmp.frames;
}

static bool load_clip(int clip_id) {
    if (clip_id < 0 || clip_id >= ZCP_CLIP_COUNT) return false;
    if (clip_id == s_clip_id && s_ready) return true;
    const zcp_blob_t *tab = s_skins[s_skin].clips;
    int rc = zcp_zpf_open(&s_zpf, tab[clip_id].start, (size_t)(tab[clip_id].end - tab[clip_id].start));
    if (rc != 0) {
        ESP_LOGE(TAG, "clip %d 打开失败 rc=%d", clip_id, rc);
        s_ready = false;
        return false;
    }
    s_clip_id = clip_id;
    s_frame_idx = 0;
    s_ready = true;
    return true;
}

static bool decode_and_show(void) {
    // ZPF4 分带解压直渲 RGB565（透明=底色）
    int rc = zcp_zpf_render_frame(&s_zpf, s_frame_idx, s_up, BG_RGB565);
    if (rc != 0) {
        static int s_logged = 0;                       // 每种 rc 只记前几次，防刷屏
        if (s_logged++ < 8) ESP_LOGE(TAG, "clip %d 帧 %u 解码失败 rc=%d", s_clip_id, s_frame_idx, rc);
        return false;
    }
    return true;
}

void zcp_pet_init(lv_obj_t *parent) {
    if (!load_clip(ZCP_CLIP_IDLE)) {
        ESP_LOGE(TAG, "首帧包加载失败，宠物区将空白（检查 assets/pet/*.zpf）");
        return;
    }
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    // LVGL 9 图片头三件套，缺一个都被静默拒绝（真机排障三天换来的，勿删）：
    // 1) magic=0x19：缺失 → cf 被当 UNKNOWN，不渲染
    // 2) data_size：缺失（=0）→ lv_draw_buf_init 校验 stride*h>data_size 失败，
    //    解码器 open 拒开，"Failed to open image" 连日志都看不到
    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = PET_W;
    s_img_dsc.header.h = PET_H;
    s_img_dsc.header.stride = PET_W * 2;
    s_img_dsc.data = (const uint8_t *)s_up;
    s_img_dsc.data_size = sizeof(s_up);

    (void)decode_and_show();

    s_img = lv_image_create(parent);
    lv_image_set_src(s_img, &s_img_dsc);
    lv_obj_set_pos(s_img, (BSP_LCD_W - PET_W) / 2, 64);
    lv_obj_set_size(s_img, PET_W, PET_H);
}

void zcp_pet_set_clip(int clip_id) {
    if (!s_img || !s_ready || clip_id == s_clip_id) return;
    if (!load_clip(clip_id)) return;
    if (decode_and_show()) {
        lv_obj_invalidate(s_img);
    }
}

void zcp_pet_tick(void) {
    if (!s_img || !s_ready) return;
    s_frame_idx = (uint16_t)((s_frame_idx + 1) % s_zpf.frames);
    if (decode_and_show()) {
        lv_obj_invalidate(s_img);
    }
}


int zcp_pet_skin_count(void) { return ZCP_SKIN_COUNT; }

uint8_t zcp_pet_skin(void) { return s_skin; }

/// 切皮肤：强制重载当前 clip（帧缓冲直接重渲），失败回退 0 号
void zcp_pet_set_skin(uint8_t skin) {
    if (skin >= ZCP_SKIN_COUNT) return;
    if (skin == s_skin && s_ready) return;
    int keep_clip = s_clip_id;
    s_skin = skin;
    s_clip_id = -1;
    s_ready = false;
    if (keep_clip < 0) keep_clip = ZCP_CLIP_IDLE;
    if (!load_clip(keep_clip)) {
        ESP_LOGE(TAG, "皮肤 %u 加载失败", skin);
        s_skin = 0;
        s_clip_id = -1;
        load_clip(keep_clip);
    }
    if (decode_and_show()) lv_obj_invalidate(s_img);
}
