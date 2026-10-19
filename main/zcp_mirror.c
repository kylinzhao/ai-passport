// main/zcp_mirror.c —— zcp_mirror.h 的纯逻辑实现（无 ESP-IDF/LVGL 依赖，主机可测）。
#include "zcp_mirror.h"

bool zcp_mid_valid(int mid) {
    return mid >= 1 && mid <= 16;
}

int zcp_clip_for_mid(int mid, uint8_t tier) {
    if (!zcp_mid_valid(mid)) return -1;
    switch (mid) {
    case 1:  return ZCP_CLIP_DEEP;       // 呼呼大睡
    case 2:  return ZCP_CLIP_AFTERMATH;  // 有点委屈
    case 3:  return ZCP_CLIP_DROWSY;     // 打盹中 zZ
    case 4:  return ZCP_CLIP_IDLE;       // 休息中
    case 5:  // 任务跑了好久了…（impatient 无专属 clip，沿用档位 working 帧，
             // 「跑了很久」的语义由文案承载——见方案 outOfScope 的已知缺口）
    case 6:
    case 7:
    case 8:
    case 9: {
        // working 档位帧：tier1/2/3 ← runningCount 1/2-3/≥4（Mac 侧同式归档）。
        // tier=0（理论不出现）兜底 working1。
        if (tier <= 1) return ZCP_CLIP_WORKING1;
        if (tier == 2) return ZCP_CLIP_WORKING2;
        return ZCP_CLIP_WORKING3;
    }
    case 10:
    case 11: return ZCP_CLIP_CELEBRATE;  // 任务完成！/ 连击完成
    case 15: return ZCP_CLIP_ERROR;      // 任务出错了
    case 12:  // 蓝牙启动中…
    case 13:  // 等待 Mac 连接…
    case 14:  // Mac 掉线了…
    case 16:  // 未发现 Mac 诊断
        return ZCP_CLIP_WAIT;
    default: return ZCP_CLIP_IDLE;
    }
}

zcp_link_state_t zcp_link_state(bool ble_ready, bool gatt_connected, int64_t last_status_age_ms) {
    if (!ble_ready) return ZCP_LINK_CONNECTING;
    if (!gatt_connected) return ZCP_LINK_WAITING;
    if (last_status_age_ms < 0) return ZCP_LINK_WAITING;
    if (last_status_age_ms >= ZCP_STATUS_TIMEOUT_MS) return ZCP_LINK_STALLED;
    return ZCP_LINK_ONLINE;
}

bool zcp_wait_diag_due(int64_t waiting_age_ms) {
    return waiting_age_ms >= ZCP_WAIT_DIAG_MS;
}
