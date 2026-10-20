// zcp_strings.c —— 自动生成（scripts/gen_device_assets.sh），勿手改。
// 源：zcode-pet/scripts/device_strings.txt；UTF-8 字面量，编码 CONFIG_LV_TXT_ENC_UTF8。

#include "zcp_strings.h"

const char *const zcp_mid_str[ZCP_MID_MAX + 1] = {
    [1] = "呼呼大睡",
    [2] = "有点委屈",
    [3] = "打盹中 zZ",
    [4] = "休息中",
    [5] = "任务跑了好久了…",
    [6] = "专注工作中…",
    [7] = "%d 个任务并行中…",
    [8] = "%d 个任务！忙翻了",
    [9] = "执行中…",
    [10] = "任务完成！",
    [11] = "连击完成",
    [12] = "蓝牙启动中…",
    [13] = "等待 Mac 连接…",
    [14] = "Mac 掉线了…",
    [15] = "任务出错了",
    [16] = "未发现 Mac · 检查 Mac 蓝牙与守护进程",
};

const char *zcp_mid_text(int mid) {
    return (mid >= 1 && mid <= ZCP_MID_MAX) ? zcp_mid_str[mid] : (const char *)0;
}

const char *zcp_str_about_hint = "OK 重连蓝牙 · 长按返回";
const char *zcp_str_about_hint2 = "下键换皮肤 OK 重连蓝牙";
const char *zcp_str_about_title = "关于";
const char *zcp_str_batt_fmt = "电量 %d%%";
const char *zcp_str_ble_fail_fmt = "蓝牙启动失败(%d)";
const char *zcp_str_ble_rescue = "重启蓝牙中…";
const char *zcp_str_boot_title = "zcode-pet";
const char *zcp_str_detail_hint = "OK已读 长按继续 上返回";
const char *zcp_str_go_sent = "已在Mac打开任务";
const char *zcp_str_list_fmt = "任务 %d · 未读 %d";
const char *zcp_str_list_hint = "上下选择 OK详情";
const char *zcp_str_read_ok = "已读";
const char *zcp_str_skin_0 = "球球";
const char *zcp_str_skin_1 = "幽幽";
const char *zcp_str_skin_fmt = "皮肤 %s";
const char *zcp_str_sub_fmt = "zcode-pet · %ds";
const char *zcp_str_task_empty = "暂无任务";
const char *zcp_str_task_st0 = "空闲";
const char *zcp_str_task_st1 = "运行中";
const char *zcp_str_task_st2 = "已完成";
const char *zcp_str_task_st3 = "出错";
const char *zcp_str_tasks_title = "任务列表";
const char *zcp_str_topbar_batt_fmt = "任务%d 未读%d %d%%%%";
const char *zcp_str_topbar_fmt = "任务%d 未读%d";
const char *zcp_str_unread_mark = "●";
