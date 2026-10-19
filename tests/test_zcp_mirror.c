// tests/test_zcp_mirror.c —— zcp_mirror 纯逻辑主机测试（镜像映射与子态判定）。
// 跑法见 tools/validate.sh --static：cc -Imain tests/test_zcp_mirror.c main/zcp_mirror.c
#include "zcp_mirror.h"

#include <assert.h>
#include <stdio.h>

static void test_clip_for_mid(void) {
    // idle 子态：deep > aftermath > drowsy > idle（与桌面 update() 判定顺序同源的镜像结果）
    assert(zcp_clip_for_mid(1, 0) == ZCP_CLIP_DEEP);
    assert(zcp_clip_for_mid(2, 0) == ZCP_CLIP_AFTERMATH);
    assert(zcp_clip_for_mid(3, 0) == ZCP_CLIP_DROWSY);
    assert(zcp_clip_for_mid(4, 0) == ZCP_CLIP_IDLE);
    // working 档位帧（mid5 不耐烦沿用档位 working 帧——方案内声明的形象级缺口）
    assert(zcp_clip_for_mid(5, 1) == ZCP_CLIP_WORKING1);
    assert(zcp_clip_for_mid(6, 1) == ZCP_CLIP_WORKING1);
    assert(zcp_clip_for_mid(7, 2) == ZCP_CLIP_WORKING2);
    assert(zcp_clip_for_mid(8, 3) == ZCP_CLIP_WORKING3);
    assert(zcp_clip_for_mid(5, 0) == ZCP_CLIP_WORKING1);  // tier=0 兜底
    // 庆祝 / 出错
    assert(zcp_clip_for_mid(10, 0) == ZCP_CLIP_CELEBRATE);
    assert(zcp_clip_for_mid(11, 0) == ZCP_CLIP_CELEBRATE);
    assert(zcp_clip_for_mid(15, 0) == ZCP_CLIP_ERROR);
    // 设备态全部落 wait
    assert(zcp_clip_for_mid(12, 0) == ZCP_CLIP_WAIT);
    assert(zcp_clip_for_mid(13, 0) == ZCP_CLIP_WAIT);
    assert(zcp_clip_for_mid(14, 0) == ZCP_CLIP_WAIT);
    assert(zcp_clip_for_mid(16, 0) == ZCP_CLIP_WAIT);
    // 协议外 mid 拒绝
    assert(zcp_clip_for_mid(0, 0) < 0);
    assert(zcp_clip_for_mid(17, 0) < 0);
    assert(zcp_clip_for_mid(-3, 0) < 0);
}

static void test_mid_valid(void) {
    assert(zcp_mid_valid(1) && zcp_mid_valid(16));
    assert(!zcp_mid_valid(0) && !zcp_mid_valid(17));
}

static void test_link_state(void) {
    // BLE 栈未就绪（init 失败/on_reset/sync 前）：一律 connecting，不看 GATT/age
    assert(zcp_link_state(false, false, -1) == ZCP_LINK_CONNECTING);
    assert(zcp_link_state(false, true, -1) == ZCP_LINK_CONNECTING);
    assert(zcp_link_state(false, false, 0) == ZCP_LINK_CONNECTING);
    // 广播中未连接（设备端常驻广播，无 UDP 版 offline 态）→ waiting
    assert(zcp_link_state(true, false, -1) == ZCP_LINK_WAITING);
    assert(zcp_link_state(true, false, 0) == ZCP_LINK_WAITING);
    assert(zcp_link_state(true, false, 60000) == ZCP_LINK_WAITING);
    // 已连接未收到 status（age=-1）→ waiting（订阅成功但 status 未到）
    assert(zcp_link_state(true, true, -1) == ZCP_LINK_WAITING);
    // 刚收到 → online
    assert(zcp_link_state(true, true, 0) == ZCP_LINK_ONLINE);
    assert(zcp_link_state(true, true, 9999) == ZCP_LINK_ONLINE);
    // 10s 看门狗：GATT 在线但无 status → stalled（Mac 僵死，mid=14 兜底）
    assert(zcp_link_state(true, true, 10000) == ZCP_LINK_STALLED);
    assert(zcp_link_state(true, true, 60000) == ZCP_LINK_STALLED);
}

static void test_wait_diag(void) {
    assert(!zcp_wait_diag_due(0));
    assert(!zcp_wait_diag_due(59999));
    assert(zcp_wait_diag_due(60000));
    assert(zcp_wait_diag_due(120000));
}

int main(void) {
    test_clip_for_mid();
    test_mid_valid();
    test_link_state();
    test_wait_diag();
    printf("test_zcp_mirror: PASS\n");
    return 0;
}
