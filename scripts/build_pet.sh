#!/usr/bin/env bash
# build_pet.sh —— zcode-pet 玩法固件的固定构建入口：
#   (1) 环境守卫：ESP-IDF v5.5.3（与已有其它版本并存，见报错提示）
#   (2) 资产守卫：12 个 .zpf 帧包 + 闭集字体 zcp_font_16.c（缺则先跑 gen_device_assets.sh）
#   (3) 官方构建+检查：tools/validate.sh --firmware
#       （临时目录干净构建 → idf.py merge-bin 产出 FoloToy-AI-Passport-full.bin
#         → tools/verify_firmware.py 校验 → 归档并安装到 build/）
#   (4) 打印产物路径、大小与烧录命令示例。本脚本绝不执行烧录。
#
# 日常增量开发用 `idf.py -B build build`；门禁以本脚本为准。
set -euo pipefail

PASSPORT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESP_IDF_PATH="${ESP_IDF_PATH:-$HOME/esp/esp-idf-v5.5.3}"
FULL_BIN="$PASSPORT_ROOT/build/FoloToy-AI-Passport-full.bin"

die() { echo "ERROR: $*" >&2; exit 1; }

echo "== [1/4] ESP-IDF 环境守卫 =="
if [[ ! -f "$ESP_IDF_PATH/export.sh" ]]; then
    die "未找到 ESP-IDF v5.5.3（$ESP_IDF_PATH/export.sh 不存在）。
  本玩法要求 5.5.3；与机器上已有版本（如 ~/esp/esp-idf = v5.3.2）并存安装：
    git clone --depth 1 --branch v5.5.3 --recursive --shallow-submodules \\
        https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v5.5.3
    bash ~/esp/esp-idf-v5.5.3/install.sh esp32c3
  或用 ESP_IDF_PATH 环境变量指向已有安装。"
fi
# shellcheck disable=SC1091
source "$ESP_IDF_PATH/export.sh" >/dev/null
idf.py --version 2>/dev/null | grep -q "5\.5\.3" || {
    idf.py --version >&2 || true
    die "激活的 ESP-IDF 不是 5.5.3（对齐 tools/validate.sh 的版本要求）"
}
echo "  $(idf.py --version 2>/dev/null | head -1)"

echo "== [2/4] 资产守卫 =="
MISSING=0
# 注意：组内 assets\/pet\/ 的斜杠必须转义——否则 BSD sed 在 assets 后截断 s 命令报
# "parentheses not balanced"，检查静默架空（for 循环拿到空输入直接通过）。
# 分组含 assets/pet/ 前缀：下面按 "$PASSPORT_ROOT/main/$f" 拼路径。
for f in $(sed -n -E 's/.*"(assets\/pet\/[^"]+\.zpf)".*/\1/p' "$PASSPORT_ROOT/main/CMakeLists.txt"); do
    if [[ ! -f "$PASSPORT_ROOT/main/$f" ]]; then
        echo "  ✗ 缺 main/$f"
        MISSING=1
    fi
done
[[ "$MISSING" == 0 ]] || die ".zpf 帧包不全：先在任一目录跑 ai-passport/scripts/gen_device_assets.sh（需 zcode-pet 仓库）"
for f in main/zcp_strings.c assets/fonts/zcp_font_16.c assets/fonts/zcp_font_16.codepoints.txt; do
    [[ -f "$PASSPORT_ROOT/$f" ]] || die "缺 ${f}：先跑 scripts/gen_device_assets.sh"
done
# 字体覆盖检查（对齐 docs/development/engineering/lvgl-chinese-fonts.md 的交付前检查）：
# zcp_strings.c 全部字符串字面量码点 ⊆ codepoints 登记 ∪ 可打印 ASCII——
# 防止改了文案没重跑 gen_device_assets.sh 导致真机 tofu。
python3 - "$PASSPORT_ROOT" <<'PY' || die "字体覆盖检查失败：文案码点超出字体子集——重跑 scripts/gen_device_assets.sh"
import re, sys
root = sys.argv[1]
src = open(f"{root}/main/zcp_strings.c", encoding="utf-8").read()
registered = open(f"{root}/assets/fonts/zcp_font_16.codepoints.txt", encoding="utf-8").read()
covered = set(registered) | {chr(c) for c in range(0x20, 0x7F)}
bad = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    i = 0
    while i < len(lit):
        if lit[i] == '\\' and i + 1 < len(lit):
            i += 2                                       # \" \\ 等转义序列落在 ASCII 内
            continue
        if lit[i] not in covered:
            bad.add(lit[i])
        i += 1
if bad:
    print("✗ 超出字体子集的字符:", " ".join(f"U+{ord(c):04X}「{c}」" for c in sorted(bad)))
    sys.exit(1)
print(f"  字体覆盖检查 OK（登记 {len(set(registered))} 个码点）")
PY
echo "  12 个 .zpf + zcp_strings.c + zcp_font_16.c（含码点登记）就位"

echo "== [3/4] 官方构建 + 合并固件（tools/validate.sh --firmware） =="
cd "$PASSPORT_ROOT"
./tools/validate.sh --firmware

echo "== [4/4] 产物 =="
# 变量后紧跟全角标点必须加 ${}：macOS bash 3.2（C locale）会把全角括号的 UTF-8
# 首字节并进变量名，报 "FULL_BIN<garbage>: unbound variable"。
[[ -f "$FULL_BIN" ]] || die "合并固件未生成（${FULL_BIN}）"
ELF="$(ls "$PASSPORT_ROOT"/build/firmware/*/zcode_pet.elf "$PASSPORT_ROOT"/build/firmware/*/*.elf 2>/dev/null | head -1 || true)"
ls -l "$FULL_BIN"
[[ -n "$ELF" ]] && ls -l "$ELF"
echo "  合并固件: ${FULL_BIN}（可从 0x0 整片刷写）"
echo "  烧录命令示例（本脚本绝不执行）:"
echo "    esptool.py --chip esp32c3 write_flash 0x0 $FULL_BIN"
echo "  注意：0x0 整片镜像可能重置 NVS（已配网设备建议 idf.py -B build flash 分段保凭据）"
