// main/zcp_bridge.h —— BLE GATT 桥接（NimBLE peripheral，常驻广播）。
//
// 协议契约见 zcode-pet 仓库 daemon/DeviceBridge.swift 头注释；本端实现：
//   · Mac=中心（CoreBluetooth），设备=外设；不配对不加密（数据仅计数与闭集文案 id）
//   · 收 status（RX write ≤512B）→ 更新镜像快照 + notify ack{seq}；10s 无 status 由
//     zcp_link_state 判 STALLED
//   · 每 5s notify hello；OK 键 → zcp_bridge_send_read() notify
//   · 广播：adv=flags+128bit 服务 UUID，scan response=完整设备名 "zcode-pet"；
//     断开后 30s 用 60ms 快间隔、之后 320ms 慢间隔
//
// UUID 三常量与 Swift 侧逐字节一致（scripts/check_esp32_bridge.sh [4/4] 把关，勿单侧改动）：
//   Service 5A1C0001-7BCD-4E0F-9A2B-3D5E6F8091A2
//   TX      5A1C0002-7BCD-4E0F-9A2B-3D5E6F8091A2（设备→Mac，NOTIFY，max_len 64）
//   RX      5A1C0003-7BCD-4E0F-9A2B-3D5E6F8091A2（Mac→设备，WRITE 带响应，max_len 512）
//
// 消息（单行 JSON，v 固定 1，与 UDP 版字段完全一致）：
//   hello  设备→Mac notify（每 5s）
//          {"v":1,"t":"hello","name":"zcode-pet","fw":"1.0.0"}
//   status Mac→设备 write RX（每 2s 周期兜底 + 变化即时补发 0.25s 节流）
//          {"v":1,"t":"status","seq":N,"mode":"idle|working|celebrate|error",
//           "tier":0..3,"run":N,"unread":N,"mid":M,"n":可选数字,"ts":毫秒}
//          mode/tier/run/unread/mid/n 全部来自 PetDisplayState 快照（与宠物文案同源）
//   ack    设备→Mac notify {"v":1,"t":"ack","seq":N}（回执最近一次收到的 status）
//   read   设备→Mac notify {"v":1,"t":"read"}（OK 键 = 已读确认，走 ReadWatermark 水位）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "zcp_mirror.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动桥接（NVS init + NimBLE init + GATT 服务注册 + host task）。幂等；
// 初始化失败不 panic，失败状态经 zcp_bridge_ble_fail_* 供页面诊断与重试。
void zcp_bridge_start(void);

// 镜像快照（锁内拷贝）。valid=false 表示尚未收到任何 status。
void zcp_bridge_get_state(zcp_pet_state_t *out);

// BLE 链路输入（zcp_link_state 的入参）
bool zcp_bridge_ble_ready(void);          // NimBLE host 已 sync（可广播）
bool zcp_bridge_gatt_connected(void);     // GATT 链路已连接
int64_t zcp_bridge_ms_since_status(void); // 距最近一次 status 的毫秒数；从未收到=-1

// BLE 栈失败诊断与恢复
int zcp_bridge_ble_last_err(void);        // 最近一次 init/on_reset 失败码；0=正常
int zcp_bridge_ble_fail_count(void);      // 连续失败次数（订阅成功/重试成功后清零）
bool zcp_bridge_ble_retry_due(int64_t now_ms); // 连续失败≥3 后每 30s 到期一次（自动重试）
void zcp_bridge_restart(void);            // 重启 BLE 栈（stop→deinit→init；OK 键救援路径）

// 主动断开当前 GATT 连接并重启广播（Pet 页非 ONLINE 时 OK 键的「蓝牙救援」）
void zcp_bridge_drop_peer(void);

// 设备 → Mac：已读确认（OK 键触发，notify {"v":1,"t":"read"}）
void zcp_bridge_send_read(void);

#ifdef __cplusplus
}
#endif
