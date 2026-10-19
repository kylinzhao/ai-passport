#!/usr/bin/env bash
# deploy_pet.sh —— zcode-pet 玩法固件部署：探测 USB 串口 → idf.py flash 分段烧录。
#
# 前置：先跑 scripts/build_pet.sh 出 build/FoloToy-AI-Passport-full.bin 与 ELF。
# 分段烧录（bootloader+partition+app 各自 0x… 地址）而非 0x0 整片：不重置 NVS。
# 本脚本只烧录，不构建；设备不在或烧录失败一律非零退出并打印原因。
set -euo pipefail

PASSPORT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESP_IDF_PATH="${ESP_IDF_PATH:-$HOME/esp/esp-idf-v5.5.3}"
BUILD_DIR="$PASSPORT_ROOT/build"

die() { echo "ERROR: $*" >&2; exit 1; }

echo "== [1/3] ESP-IDF 环境守卫 =="
if [[ ! -f "$ESP_IDF_PATH/export.sh" ]]; then
    die "未找到 ESP-IDF v5.5.3（$ESP_IDF_PATH/export.sh 不存在）；见 build_pet.sh 的安装指引"
fi
# shellcheck disable=SC1091
source "$ESP_IDF_PATH/export.sh" >/dev/null
idf.py --version 2>/dev/null | grep -q "5\.5\.3" || die "激活的 ESP-IDF 不是 5.5.3"

echo "== [2/3] 产物与串口探测 =="
[[ -f "$BUILD_DIR/FoloToy-AI-Passport-full.bin" ]] || \
    die "未找到合并固件（$BUILD_DIR/FoloToy-AI-Passport-full.bin）：先跑 scripts/build_pet.sh"
APP_BIN="$(ls -t "$BUILD_DIR"/firmware/*/FoloToy-AI-Passport.bin 2>/dev/null | head -1 || true)"
[[ -n "$APP_BIN" ]] || die "未找到分段 app 镜像（build/firmware/<ts>/FoloToy-AI-Passport.bin）：重跑 build_pet.sh"
STAMP_DIR="$(dirname "$APP_BIN")"                       # 同一归档内取配套镜像，防新旧错配
BOOT_BIN="$STAMP_DIR/bootloader/bootloader.bin"
PART_BIN="$STAMP_DIR/partition_table/partition-table.bin"
[[ -f "$BOOT_BIN" && -f "$PART_BIN" ]] || die "未找到 bootloader/partition-table 镜像（${STAMP_DIR}）：重跑 build_pet.sh"

PORTS="$(ls /dev/cu.* 2>/dev/null | grep -Ei 'usb(a|serial|modem)' || true)"
[[ -n "$PORTS" ]] || die "未探测到 USB 串口（/dev/cu.usb*）：确认设备已插上 Mac 的数据线 USB 口，再重试"
PORT_COUNT="$(echo "$PORTS" | wc -l | tr -d ' ')"
if [[ "$PORT_COUNT" -gt 1 ]]; then
    echo "  探测到多个串口，取第一个（可用 PORT=… 覆盖）："
    echo "$PORTS" | sed 's/^/    /'
fi
PORT="${PORT:-$(echo "$PORTS" | head -1)}"
echo "  串口: $PORT"

echo "== [3/3] 分段烧录（不重置 NVS） =="
cd "$PASSPORT_ROOT"
esptool.py --chip esp32c3 --port "$PORT" --baud 921600 \
    --before default_reset --after hard_reset write_flash \
    0x0 "$BOOT_BIN" 0x8000 "$PART_BIN" 0x10000 "$APP_BIN" \
    || die "烧录失败（端口被占用/设备未进下载模式？拔插后重试；或关闭占用串口的 monitor）"
echo ""
echo "deploy_pet: 完成 ✅  设备已复位重启（Boot 页 → Pet 页 → 等待 Mac 连接）"
echo "  看日志: idf.py -B build monitor（退出 Ctrl+]）"
