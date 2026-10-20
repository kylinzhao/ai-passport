// main/zcp_bridge.c —— BLE GATT 桥接实现（NimBLE peripheral，常驻广播）。
//
// 线程模型：GATT access 回调跑在 NimBLE host task——只做 cJSON 解析 + 互斥锁写快照 +
// 回 ack notify，绝不碰 LVGL（与 UDP 版 udp_task 同约定）；hello 通知由专属任务每 5s
// 发一次。广播状态机：GATT 断开/广播完成 → 立即重启广播，断开后 30s 内 60ms 快间隔
// （重连最坏 <0.5s）、之后 320ms 慢间隔（待机功耗取舍）。
//
// 生命周期照 main/demo_ble.c 的 init/host_task/on_sync/on_reset 模式；失败可整体重启
// （zcp_bridge_restart：nimble_port_stop→deinit→init），连续失败由应用层 30s 定时重试。
#include "zcp_bridge.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "zcp_bridge";

#define DEVICE_NAME "zcode-pet"
#define HELLO_PERIOD_US (5LL * 1000 * 1000)
#define RX_BUF_LEN 512                    // RX 特征 max_len（与 DeviceBridge.maxStatusBytes 对应）
#define TX_MAX_LEN 64                     // TX 特征 max_len（hello/ack/read 均 <64B）
#define ADV_FAST_ITVL 96                  // 60ms（0.625ms 单位）
#define ADV_SLOW_ITVL 512                 // 320ms
#define ADV_FAST_WINDOW_US (30LL * 1000 * 1000) // 断开后快间隔持续时间
#define BLE_FAIL_AUTO_RETRY_MS (30LL * 1000)
#define BLE_FAIL_AUTO_RETRY_COUNT 3
#define BLE_STOP_TIMEOUT_MS 2000
#define HOST_TASK_STACK 4096

// ---- UUID 三常量（与 zcode-pet daemon/DeviceBridge.swift 一一对应）----
// 128bit UUID 在 BLE 线上按小端序传输，BLE_UUID128_INIT 依线上序展开：
//   5A1C0001-7BCD-4E0F-9A2B-3D5E6F8091A2 → A2 91 80 6F 5E 3D 2B 9A 0F 4E CD 7B 01 00 1C 5A
#define ZCP_UUID128_INIT(tail) \
    BLE_UUID128_INIT(0xA2, 0x91, 0x80, 0x6F, 0x5E, 0x3D, 0x2B, 0x9A, \
                     0x0F, 0x4E, 0xCD, 0x7B, (tail), 0x00, 0x1C, 0x5A)
// tail 字节：Service=0x01 / TX=0x02 / RX=0x03（5A1C=zcode-pet Comms 助记，0001/0002/0003）
static const ble_uuid128_t s_svc_uuid = ZCP_UUID128_INIT(0x01);
static const ble_uuid128_t s_tx_uuid = ZCP_UUID128_INIT(0x02);
static const ble_uuid128_t s_rx_uuid = ZCP_UUID128_INIT(0x03);

// ---- 镜像快照 ----

static SemaphoreHandle_t s_lock;
static zcp_pet_state_t s_state;              // 镜像快照（s_lock）
static zcp_tasks_t s_tasks;                  // 任务列表镜像（s_lock）
static int64_t s_last_status_us;             // 最近 status 时刻（esp_timer 时钟）

void zcp_bridge_get_tasks(zcp_tasks_t *out) {
    if (!out) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_tasks;
        xSemaphoreGive(s_lock);
    }
}

void zcp_bridge_get_state(zcp_pet_state_t *out) {
    if (!out) return;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_state;
        xSemaphoreGive(s_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

int64_t zcp_bridge_ms_since_status(void) {
    if (s_last_status_us == 0) return -1;
    return (esp_timer_get_time() - s_last_status_us) / 1000;
}

// ---- BLE 链路状态 ----

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;                 // TX 特征 val handle（notify 目标）
static volatile bool s_ble_ready;            // host 已 sync（on_sync 后 true）
static volatile bool s_advertising;
static int64_t s_adv_started_us;             // 本次广播开始时刻（快/慢间隔切换）
static bool s_adv_fast;                      // 当前广播是否快间隔
static uint8_t s_addr_type;                  // on_sync 推断的 own addr type（广播用）

int gap_event_cb(struct ble_gap_event *event, void *arg);
static int start_advertising(bool fast);

// BLE 栈失败诊断（init 失败 / on_reset 记账；成功 sync 或 restart 成功清零）
static volatile int s_ble_last_err;
static volatile int s_ble_fail_count;
static int64_t s_last_fail_ms;
static int64_t s_last_auto_retry_ms;

bool zcp_bridge_ble_ready(void) { return s_ble_ready; }
bool zcp_bridge_gatt_connected(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE; }
int zcp_bridge_ble_last_err(void) { return s_ble_last_err; }
int zcp_bridge_ble_fail_count(void) { return s_ble_fail_count; }

bool zcp_bridge_ble_retry_due(int64_t now_ms) {
    if (s_ble_fail_count < BLE_FAIL_AUTO_RETRY_COUNT) return false;
    if (now_ms - s_last_fail_ms < BLE_FAIL_AUTO_RETRY_MS) return false;
    if (now_ms - s_last_auto_retry_ms < BLE_FAIL_AUTO_RETRY_MS) return false;
    s_last_auto_retry_ms = now_ms;
    return true;
}

static void ble_note_fail(int err) {
    s_ble_last_err = err;
    s_ble_fail_count++;
    s_last_fail_ms = esp_timer_get_time() / 1000;
    ESP_LOGE(TAG, "BLE 栈失败 err=%d（连续第 %d 次）", err, s_ble_fail_count);
}

// ---- GATT 服务表 ----

static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);
static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_tx_uuid.u,        // 设备→Mac notify（hello/ack/read）
                .access_cb = tx_access_cb,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_rx_uuid.u,        // Mac→设备 write-with-response（status）
                .access_cb = rx_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },                           // 特征表终止
        },
    },
    { 0 },                                   // 服务表终止
};

// TX 特征无读语义（仅 notify）；ATT 读回空值即可
static int tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    return (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) ? 0 : BLE_ATT_ERR_READ_NOT_PERMITTED;
}

// ---- 设备 → Mac notify（host task 或 LVGL 外的按键任务均可调）----

static void send_notify_json(const char *json) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, strlen(json));
    if (!om) {
        ESP_LOGW(TAG, "notify mbuf 分配失败");
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify rc=%d（断开竞态，忽略）", rc);
    }
}

void zcp_bridge_send_read(void) {
    send_notify_json("{\"v\":1,\"t\":\"read\"}");
    ESP_LOGI(TAG, "read 已 notify");
}

void zcp_bridge_send_read_task(uint8_t idx, uint16_t lv) {
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"read\",\"i\":%u,\"lv\":%u}",
             (unsigned)idx, (unsigned)lv);
    send_notify_json(buf);
    ESP_LOGI(TAG, "read(i=%u,lv=%u) 已 notify", idx, lv);
}

void zcp_bridge_send_gettasks(void) {
    // 诊断：notify 结果（rc!=0 = 未连接/未订阅，静默丢失是重连场景任务列表空的元凶候选）
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0) {
        ESP_LOGW(TAG, "gettasks 丢弃：未连接/无TX handle");
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat("{\"v\":1,\"t\":\"gettasks\"}", 22);
    int rc = om ? ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om) : BLE_HS_ENOMEM;
    if (rc != 0) ESP_LOGW(TAG, "gettasks notify rc=%d", rc);
}

void zcp_bridge_send_go(uint8_t idx, uint16_t lv) {
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"go\",\"i\":%u,\"lv\":%u}",
             (unsigned)idx, (unsigned)lv);
    send_notify_json(buf);
    ESP_LOGI(TAG, "go(i=%u,lv=%u) 已 notify", idx, lv);
}

// ---- status 解析（RX write；host task 上下文，只碰锁不碰 LVGL）----

static void handle_status(cJSON *obj) {
    cJSON *seq = cJSON_GetObjectItem(obj, "seq");
    zcp_pet_state_t st = {
        .valid = true,
        .seq = (uint32_t)(cJSON_IsNumber(seq) ? seq->valuedouble : 0),
    };
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "mode"));
    if (mode && strcmp(mode, "working") == 0) st.mode = ZCP_MODE_WORKING;
    else if (mode && strcmp(mode, "celebrate") == 0) st.mode = ZCP_MODE_CELEBRATE;
    else if (mode && strcmp(mode, "error") == 0) st.mode = ZCP_MODE_ERROR;
    else st.mode = ZCP_MODE_IDLE;
    cJSON *item;
    if ((item = cJSON_GetObjectItem(obj, "tier")) && cJSON_IsNumber(item))
        st.tier = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(obj, "run")) && cJSON_IsNumber(item))
        st.run = (uint16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(obj, "unread")) && cJSON_IsNumber(item))
        st.unread = (uint16_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(obj, "mid")) && cJSON_IsNumber(item))
        st.mid = (uint8_t)item->valuedouble;
    if ((item = cJSON_GetObjectItem(obj, "n")) && cJSON_IsNumber(item)) {
        st.has_n = true;
        st.n = (uint16_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(obj, "ts")) && cJSON_IsNumber(item))
        st.ts_ms = (int64_t)item->valuedouble;

    if (!zcp_mid_valid(st.mid)) {
        ESP_LOGW(TAG, "status mid=%u 越界，丢弃", st.mid);
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state = st;
        xSemaphoreGive(s_lock);
    }
    s_last_status_us = esp_timer_get_time();

    char ack[48];
    snprintf(ack, sizeof(ack), "{\"v\":%d,\"t\":\"ack\",\"seq\":%lu}",
             ZCP_PROTO_VERSION, (unsigned long)st.seq);
    send_notify_json(ack);
}

// tasks 解析（全量推送；host task 上下文）
static void handle_tasks(cJSON *obj) {
    zcp_tasks_t t;
    memset(&t, 0, sizeof(t));
    cJSON *item;
    if ((item = cJSON_GetObjectItem(obj, "lv")) && cJSON_IsNumber(item))
        t.lv = (uint16_t)item->valuedouble;
    cJSON *items = cJSON_GetObjectItem(obj, "items");
    if (cJSON_IsArray(items)) {
        cJSON *e;
        cJSON_ArrayForEach(e, items) {
            if (t.count >= ZCP_TASKS_MAX) break;
            cJSON *f;
            uint8_t st = ZCP_TASK_ST_OTHER;
            if ((f = cJSON_GetObjectItem(e, "s")) && cJSON_IsNumber(f))
                st = (uint8_t)f->valuedouble;
            bool unread = false;
            if ((f = cJSON_GetObjectItem(e, "u")) && cJSON_IsNumber(f))
                unread = f->valuedouble != 0;
            const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(e, "t"));
            if (title && title[0]) {
                strncpy(t.title[t.count], title, ZCP_TASK_TITLE_MAX - 1);
                t.status[t.count] = st <= ZCP_TASK_ST_ERROR ? st : ZCP_TASK_ST_OTHER;
                t.unread[t.count] = unread;
                t.count++;
            }
        }
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint8_t sel = s_tasks.sel;                  // 选择不因推送重置
        bool keep_sel = sel < t.count;
        s_tasks = t;
        s_tasks.sel = keep_sel ? sel : 0;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "tasks lv=%u count=%u 已镜像", t.lv, t.count);
}

// Mac→设备写 RX：NimBLE 已按 ATT long write 重装完整 buffer（preferred MTU 256，
// 更小 MTU 时 Prepare/Execute 自动分块），这里拿到的 om 链即完整 status 帧。
static int rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len <= 0 || len > RX_BUF_LEN) {
        ESP_LOGW(TAG, "RX 长度非法 %u", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    char buf[RX_BUF_LEN + 1];
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
    buf[len] = '\0';

    cJSON *obj = cJSON_Parse(buf);
    if (!obj) return 0;                                  // 坏包不回执，Mac 看门狗兜底
    cJSON *v = cJSON_GetObjectItem(obj, "v");
    cJSON *t = cJSON_GetObjectItem(obj, "t");
    if (cJSON_IsNumber(v) && (int)v->valuedouble == ZCP_PROTO_VERSION &&
        cJSON_IsString(t) && strcmp(t->valuestring, "status") == 0) {
        handle_status(obj);
    } else if (cJSON_IsNumber(v) && (int)v->valuedouble == ZCP_PROTO_VERSION &&
               cJSON_IsString(t) && strcmp(t->valuestring, "tasks") == 0) {
        handle_tasks(obj);
    }
    // 其它类型（hello/read 是设备发的，Mac 不发；未知类型忽略）
    cJSON_Delete(obj);
    return 0;
}

// ---- 广播状态机 ----

static int start_advertising(bool fast);

static void on_reset(int reason) {
    s_ble_ready = false;
    s_advertising = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_note_fail(reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = start_advertising(true);
    if (rc != 0) {
        ble_note_fail(rc);
        return;
    }
    s_ble_ready = true;
    s_ble_last_err = 0;
    s_ble_fail_count = 0;
    ESP_LOGI(TAG, "NimBLE synced，开始广播（%s）", DEVICE_NAME);
}

static int start_advertising(bool fast) {
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &s_svc_uuid;                          // adv 主包：flags(3)+uuid128(18)=21B ≤ 31
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return rc;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;             // 完整设备名 11B 放 scan response
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
        return rc;
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;            // 可连接广播
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = params.itvl_max = fast ? ADV_FAST_ITVL : ADV_SLOW_ITVL;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        return rc;
    }
    s_advertising = true;
    s_adv_fast = fast;
    s_adv_started_us = esp_timer_get_time();
    return 0;
}

int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false;
            ESP_LOGI(TAG, "GATT 已连接 handle=%u", s_conn_handle);
        } else {
            // 连接建立失败：立即重启广播（失败码仅日志，栈本身没坏）
            ESP_LOGW(TAG, "connect 失败 status=%d，重启广播", event->connect.status);
            start_advertising(true);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "GATT 断开 reason=%d，立即重启广播", event->disconnect.reason);
        start_advertising(true);                         // 断开即快间隔，最坏 <0.5s 可重连
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        start_advertising(true);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle && event->subscribe.cur_notify) {
            ESP_LOGI(TAG, "Mac 已订阅 TX notify，请求任务列表");
            send_notify_json("{\"v\":1,\"t\":\"gettasks\"}");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 协商 =%u（conn %u）", event->mtu.value, event->mtu.conn_handle);
        return 0;
    default:
        return 0;
    }
}

// 快/慢间隔切换：广播超过 30s 仍未连接 → 停掉换 320ms 慢间隔
static void adv_tune_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_advertising || !s_adv_fast) continue;
        if (esp_timer_get_time() - s_adv_started_us < ADV_FAST_WINDOW_US) continue;
        ble_gap_adv_stop();
        s_advertising = false;
        start_advertising(false);
        ESP_LOGI(TAG, "30s 未连接，广播转 320ms 慢间隔");
    }
}

// ---- hello 周期任务 ----

static void hello_task(void *arg) {
    (void)arg;
    int64_t last_hello = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) continue;
        if (now - last_hello < HELLO_PERIOD_US) continue;
        last_hello = now;
        send_notify_json("{\"v\":1,\"t\":\"hello\",\"name\":\"" DEVICE_NAME "\",\"fw\":\"1.0.0\"}");
    }
}

// ---- 栈启停（zcp_bridge_restart 的两半）----

static SemaphoreHandle_t s_host_stopped;
static TaskHandle_t s_host_task;
static volatile bool s_host_running;
static bool s_initialized;

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();                                   // 返回 = host 已停
    xSemaphoreGive(s_host_stopped);
    for (;;) vTaskSuspend(NULL);
}

static esp_err_t ble_stack_start(void) {
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ble_note_fail(err);
        return err;
    }
    s_initialized = true;
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        ble_note_fail(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc == 0) rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务注册失败 rc=%d", rc);
        ble_note_fail(rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    if (xTaskCreatePinnedToCore(host_task, "zcp_nimble", HOST_TASK_STACK,
                                NULL, configMAX_PRIORITIES - 4, &s_host_task, 0) != pdPASS) {
        ble_note_fail(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_host_running = true;
    return ESP_OK;
}

static void ble_stack_stop(void) {
    if (!s_initialized) return;
    s_ble_ready = false;
    s_advertising = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    if (s_host_task && s_host_running) {
        (void)ble_gap_adv_stop();
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop rc=%d", rc);
        } else {
            s_host_running = false;
        }
    }
    if (s_host_task && s_host_stopped) {
        if (xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(BLE_STOP_TIMEOUT_MS)) == pdTRUE) {
            vTaskDelete(s_host_task);
        } else {
            // 停止超时：不再等（任务悬空仅泄漏一次栈，不让救援路径卡死按键）
            ESP_LOGE(TAG, "等待 NimBLE host 停止超时，任务放弃回收");
        }
        s_host_task = NULL;
    }
    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit: %s", esp_err_to_name(err));
    }
    s_initialized = false;
}

// ---- 启动 / 重启 / 救援 ----

static bool s_started;

void zcp_bridge_start(void) {
    if (s_started) return;
    s_started = true;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "互斥量创建失败");
        return;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (ble_stack_start() == ESP_OK) {
        xTaskCreatePinnedToCore(hello_task, "zcp_hello", 3072, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(adv_tune_task, "zcp_advtune", 2560, NULL, 3, NULL, 0);
        ESP_LOGI(TAG, "桥接启动：BLE GATT 常驻广播，等 Mac 连接");
    }
}

void zcp_bridge_restart(void) {
    ESP_LOGW(TAG, "重启 BLE 栈（连接救援）");
    s_ble_fail_count = 0;
    s_ble_last_err = 0;
    ble_stack_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (ble_stack_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE 栈重启失败，保持失败态（按键可再试）");
    }
}

void zcp_bridge_drop_peer(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    ESP_LOGI(TAG, "蓝牙救援：断开 GATT 回广播态");
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_terminate rc=%d（断开事件稍后自然到达）", rc);
    }
}
