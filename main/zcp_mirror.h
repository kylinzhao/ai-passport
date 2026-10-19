// main/zcp_mirror.h —— 设备端「状态镜像」纯逻辑（不依赖 ESP-IDF/LVGL，主机可测）。
//
// 设备永远镜像 Mac 侧 PetDisplayState 快照，零本地任务计数/终态推断（防语义分叉）。
// 协议与 zcode-pet 仓库 daemon/DeviceBridge.swift 的注释契约一一对应——改任何字段
// 必须两侧同步。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCP_PROTO_VERSION 1
// 设备宠物页底色（宠物帧的半透明像素按此色压平烘焙，两侧必须一致：
// 烘焙端 Esp32Exporter.bgRGB565 = #1A1A1A → RGB565 0x18C3，与桌面宠物深色卡同族）
#define ZCP_BG_HEX 0x1A1A1A

// Mac → 设备 status 的 mode 字段取值
typedef enum {
    ZCP_MODE_IDLE = 0,
    ZCP_MODE_WORKING,
    ZCP_MODE_CELEBRATE,
    ZCP_MODE_ERROR,
} zcp_mode_t;

// 镜像状态（来自 status 消息；全部字段由 Mac 给出，设备不做推断）
typedef struct {
    uint8_t mode;        // zcp_mode_t
    uint8_t tier;        // 0..3（working 档位）
    uint16_t run;        // 执行中任务数
    uint16_t unread;     // 完成未读数
    uint8_t mid;         // 闭集文案枚举 1..16（1-11/15=sync 区，12-14/16=设备区）
    bool has_n;          // 可选数字参数（mid7/8 并行任务数等）
    uint16_t n;
    uint32_t seq;        // 最近收到的 status 序号
    int64_t ts_ms;       // Mac 时间戳（毫秒，仅日志用）
    bool valid;          // 已收到过至少一帧 status
} zcp_pet_state_t;

// 动画 clip（与 EMBED_FILES 的 12 个 .zpf 一一对应，顺序即 id）
typedef enum {
    ZCP_CLIP_IDLE = 0,
    ZCP_CLIP_DROWSY,
    ZCP_CLIP_DEEP,
    ZCP_CLIP_WORKING1,
    ZCP_CLIP_WORKING2,
    ZCP_CLIP_WORKING3,
    ZCP_CLIP_CELEBRATE,
    ZCP_CLIP_ERROR,
    ZCP_CLIP_PAT,
    ZCP_CLIP_PERK,
    ZCP_CLIP_AFTERMATH,
    ZCP_CLIP_WAIT,
    ZCP_CLIP_COUNT,
} zcp_clip_t;

// mid → clip。映射依据 docs/zcode-pet.md 的状态表：
//   mid1 深睡 / mid2 余韵 / mid3 打盹 / mid4 休息；mid5-8 working 按档位
//   （不耐烦 mid5 无专属 clip，沿用档位 working 帧，语义由文案承载）；
//   mid9 兜底 working1；mid10/11 庆祝；mid15 出错；mid12/13/14/16 设备态 → wait。
// 返回负数表示 mid 非法（协议外，退回 idle）。
int zcp_clip_for_mid(int mid, uint8_t tier);

// mid 是否在闭集（1..16）内
bool zcp_mid_valid(int mid);

// 主页子态（BLE 链路的派生态，判定纯函数）
typedef enum {
    ZCP_LINK_CONNECTING = 0,   // BLE 栈启动中（init 失败/on_reset/sync 前）
    ZCP_LINK_WAITING,          // 广播中未连接；或已连接但从未收到 status
    ZCP_LINK_ONLINE,           // GATT 在线且 10s 内有 status
    ZCP_LINK_STALLED,          // GATT 在线但 >10s 无 status（Mac 僵死兜底 → mid14）
} zcp_link_state_t;

// 子态判定：ble_ready=NimBLE host 已 sync；gatt_connected=GATT 链路已连接；
// last_status_age_ms=最近一次 status 距今。断开即回 waiting（设备端常驻广播，
// 无 UDP 版的 offline 重试耗尽态）；连接但 ZCP_STATUS_TIMEOUT_MS 无 status 判
// STALLED（Mac 僵死兜底，显示 mid=14）。
#define ZCP_STATUS_TIMEOUT_MS 10000
zcp_link_state_t zcp_link_state(bool ble_ready, bool gatt_connected, int64_t last_status_age_ms);

// waiting 子态是否已超过 60s（追加 mid=16 诊断文案的阈值）
#define ZCP_WAIT_DIAG_MS 60000
bool zcp_wait_diag_due(int64_t waiting_age_ms);

#ifdef __cplusplus
}
#endif
