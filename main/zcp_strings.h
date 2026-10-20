// zcp_strings.h —— 自动生成（scripts/gen_device_assets.sh，源：
// zcode-pet/scripts/device_strings.txt 两节闭集文案）。勿手改；文案变更走源文件再生成。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ZCP_MID_MAX 16

// mid → 文案（1-11/15 = sync 区桌面投影；12/13/14/16 = 设备区）。空缺位为 NULL。
extern const char *const zcp_mid_str[ZCP_MID_MAX + 1];
const char *zcp_mid_text(int mid);

// 设备区具名字符串（模板 %d 由 snprintf 填数字；数字字形走 Montserrat fallback）
extern const char *zcp_str_boot_title;
extern const char *zcp_str_list_fmt;
extern const char *zcp_str_topbar_fmt;
extern const char *zcp_str_sub_fmt;
extern const char *zcp_str_about_title;
extern const char *zcp_str_about_hint;
extern const char *zcp_str_batt_fmt;
extern const char *zcp_str_ble_fail_fmt;
extern const char *zcp_str_ble_rescue;
extern const char *zcp_str_topbar_batt_fmt;
extern const char *zcp_str_tasks_title;
extern const char *zcp_str_task_empty;
extern const char *zcp_str_task_st0;
extern const char *zcp_str_task_st1;
extern const char *zcp_str_task_st2;
extern const char *zcp_str_task_st3;
extern const char *zcp_str_list_hint;
extern const char *zcp_str_detail_hint;
extern const char *zcp_str_go_sent;
extern const char *zcp_str_read_ok;
extern const char *zcp_str_unread_mark;

#ifdef __cplusplus
}
#endif
