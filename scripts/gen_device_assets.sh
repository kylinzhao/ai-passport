#!/usr/bin/env bash
# gen_device_assets.sh —— 从 zcode-pet 的矢量管线生成 zcode-pet 玩法的设备资产：
#   1. 编译 zcode-pet daemon（只读引用，不改对方仓库）→ 跑 --export-esp32 烘焙
#      12 段 .zpf 帧包到 main/assets/pet/
#   2. 解析 zcode-pet/scripts/device_strings.txt（两节闭集文案）→ 生成 main/zcp_strings.c/.h
#   3. 用两节全文码点 + ASCII 可打印集调 lv_font_conv 生成 assets/fonts/zcp_font_16.c
#      （--no-compress：sdkconfig 未启用 CONFIG_LV_USE_FONT_COMPRESSED，压缩产物会渲染失败）
#
# 依赖：macOS swiftc、node 的 lv_font_conv（未装：npm install -g lv_font_conv）、
#       中文 TTF（FONT_TTF 环境变量，默认思源黑体/Noto Sans CJK SC，
#       缺失时见报错里的下载说明）。
# 产物全部入库提交，保证固件可独立复现构建；本脚本不烧录、不依赖设备。
set -euo pipefail

PASSPORT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZCODE_PET_ROOT="${ZCODE_PET_ROOT:-$(cd "$PASSPORT_ROOT/.." && pwd)/zcode-pet}"
STRINGS_SRC="$ZCODE_PET_ROOT/scripts/device_strings.txt"
PET_OUT="$PASSPORT_ROOT/main/assets/pet"
FONT_OUT="$PASSPORT_ROOT/assets/fonts/zcp_font_16.c"
FONT_TTF="${FONT_TTF:-$HOME/.zcode-pet/fonts/NotoSansCJKsc-Regular.otf}"
LV_FONT_CONV="${LV_FONT_CONV:-lv_font_conv}"

die() { echo "ERROR: $*" >&2; exit 1; }

echo "== [1/5] 环境与依赖守卫 =="
[[ -d "$ZCODE_PET_ROOT/daemon" ]] || die "找不到 zcode-pet 仓库（${ZCODE_PET_ROOT}）；可用 ZCODE_PET_ROOT 指定"
[[ -f "$STRINGS_SRC" ]] || die "找不到闭集文案来源 $STRINGS_SRC"
[[ -f "$FONT_TTF" ]] || die "找不到中文字体 TTF/OTF（FONT_TTF=${FONT_TTF}）。
  获取 Noto Sans CJK SC（思源黑体同源，SIL OFL 1.1，可再分发）：
  curl -L -o ~/.zcode-pet/fonts/NotoSansCJKsc-Regular.otf \\
    https://fastly.jsdelivr.net/gh/notofonts/noto-cjk@main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"
command -v swiftc >/dev/null || die "swiftc 不可用（需 Xcode CLT）"
command -v "$LV_FONT_CONV" >/dev/null || die "lv_font_conv 未安装：npm install -g lv_font_conv"
LV_FONT_CONV_VERSION="$("$LV_FONT_CONV" -v 2>&1 || true)"
echo "  lv_font_conv version: ${LV_FONT_CONV_VERSION:-unknown}"
echo "  font: $FONT_TTF"

echo "== [2/5] 编译 zcode-pet daemon（只读引用） =="
TMP="$(mktemp -d /tmp/zcp-gen-assets.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT
# 编译清单 = daemon/build.sh 与 scripts/install.sh 的同一清单（DeviceBridge.swift，
# BLE 桥接；check_esp32_bridge.sh [1/4] 校验同一等价性）
swiftc -O "$ZCODE_PET_ROOT/daemon/main.swift" "$ZCODE_PET_ROOT/daemon/DeviceBridge.swift" \
    -o "$TMP/zcp-exporter"

echo "== [3/5] 烘焙 .zpf 帧包 → $PET_OUT =="
"$TMP/zcp-exporter" --export-esp32 "$PET_OUT"

echo "== [4/5] 校验 EMBED 清单与资产一致 =="
# 单一事实来源：直接解析 main/CMakeLists.txt 的 EMBED_FILES 显式清单
EMBED_MISSING=0
for f in $(sed -n -E 's/.*"assets\/pet\/([^"]+\.zpf)".*/\1/p' "$PASSPORT_ROOT/main/CMakeLists.txt"); do
    if [[ ! -f "$PET_OUT/$f" ]]; then
        echo "  ✗ 缺 $PET_OUT/$f"
        EMBED_MISSING=1
    fi
done
[[ "$EMBED_MISSING" == 0 ]] || die "CMakeLists EMBED_FILES 与烘焙产物不一致；检查 --export-esp32 的 clip 清单"
echo "  EMBED_FILES 12 个 .zpf 全部就位"

echo "== [5/5] 生成 zcp_strings.c/.h 与中文字体子集 =="
perl - "$STRINGS_SRC" "$PASSPORT_ROOT/main" "$TMP/codepoints.txt" <<'PERL_EOF'
use strict; use warnings;
use utf8;
use open ':encoding(UTF-8)';
binmode STDOUT, ':encoding(UTF-8)';

my ($src, $out_dir, $cp_file) = @ARGV;

# 必需的 mid（1-16 全覆盖）与设备符号（别名 → 规范 C 符号；key 归一化=小写去空白/下划线）
my @mids = (1..16);
my %alias = (
    boottitle    => 'boot_title',
    listfmt      => 'list_fmt',
    listtitle    => 'list_fmt',
    topbarfmt    => 'topbar_fmt',
    topbarrun    => 'topbar_fmt',
    subfmt       => 'sub_fmt',
    abouttitle   => 'about_title',
    abouthint    => 'about_hint',
    battfmt      => 'batt_fmt',
    blefailfmt   => 'ble_fail_fmt',
    blerescue    => 'ble_rescue',
);

open my $in, '<', $src or die "读不到 $src: $!";
my (%mid, %sym, @cp);
my $section = 0;
while (my $line = <$in>) {
    $line =~ s/[\r\n]+$//;
    if    ($line =~ /^#\s*sync:/)   { $section = 1; next; }
    elsif ($line =~ /^#\s*device:/) { $section = 2; next; }
    next if $line =~ /^\s*#/ || $line =~ /^\s*$/;
    # 兼容两种分隔（key: 值 / key = 值）——契约主格式为 "key: 值"
    my ($k, $v);
    if    ($line =~ /^([^:=]+):\s*(.+)$/) { ($k, $v) = ($1, $2); }
    elsif ($line =~ /^([^:=]+)=\s*(.+)$/) { ($k, $v) = ($1, $2); }
    else   { die "无法解析的行 $.: $line"; }
    $k =~ s/^\s+|\s+$//g; $v =~ s/^\s+|\s+$//g;
    die "空文案（$.）" if $v eq '';
    if ($section == 1 && $k =~ /^(?:mid)?(\d+)$/i) { $mid{$1} = $v; }
    elsif ($section == 2 && $k =~ /^(?:mid)?(\d+)$/i) { $mid{$1} = $v; }
    elsif ($section == 2) {
        my $key = lc $k; $key =~ s/[\s_-]//g;
        die "未知设备符号 $k（别名表外）" unless exists $alias{$key};
        $sym{$alias{$key}} = $v;
    } else {
        die "sync 区出现非 mid 键 $.: $k";
    }
    push @cp, split //, $v;
}
close $in;

my @miss_mid = grep { !exists $mid{$_} } @mids;
die "缺 mid 文案: @miss_mid（device_strings.txt 两节须覆盖 mid 1-16）" if @miss_mid;
my @miss_sym = grep { !exists $sym{$_} } sort values %alias;
if (@miss_sym) {
    die "device_strings.txt 缺设备符号: @miss_sym\n" .
        "固件引用这些键（格式 `key: 文案`，# device: 区），请补齐后重跑";
}

# 模板占位 {n} → %d；既有 %d/%s 保留，其余孤立 % 转义；引号/反斜杠转义
sub cstr {
    my $s = shift;
    $s =~ s/\{n\}/%d/g;
    $s =~ s/%(?![ds])/%%/g;
    $s =~ s/\\/\\\\/g;
    $s =~ s/"/\\"/g;
    return $s;
}

open my $h, '>', "$out_dir/zcp_strings.h" or die $!;
print $h <<'EOF';
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

#ifdef __cplusplus
}
#endif
EOF
close $h;

open my $c, '>', "$out_dir/zcp_strings.c" or die $!;
print $c "// zcp_strings.c —— 自动生成（scripts/gen_device_assets.sh），勿手改。\n";
print $c "// 源：zcode-pet/scripts/device_strings.txt；UTF-8 字面量，编码 CONFIG_LV_TXT_ENC_UTF8。\n\n";
print $c "#include \"zcp_strings.h\"\n\n";
print $c "const char *const zcp_mid_str[ZCP_MID_MAX + 1] = {\n";
for my $m (1..16) {
    printf $c "    [%d] = \"%s\",\n", $m, cstr($mid{$m});
}
print $c "};\n\n";
print $c "const char *zcp_mid_text(int mid) {\n";
print $c "    return (mid >= 1 && mid <= ZCP_MID_MAX) ? zcp_mid_str[mid] : (const char *)0;\n";
print $c "}\n\n";
for my $key (sort keys %sym) {
    printf $c "const char *zcp_str_%s = \"%s\";\n", $key, cstr($sym{$key});
}
close $c;

# 码点集：两节全文 + ASCII 可打印（设备渲染模板里的数字/空格/字母全覆盖）
my %seen;
my $syms = '';
for my $ch (0x20..0x7E) { my $c = chr($ch); next if $seen{$c}++; $syms .= $c; }
for my $ch (@cp) { next if $seen{$ch}++; $syms .= $ch; }
open my $out, '>', $cp_file or die $!;
print $out $syms;
close $out;
print "  zcp_strings.c/.h 生成：", scalar(grep { exists $mid{$_} } 1..16), " 个 mid + ",
      scalar(keys %sym), " 个设备符号；字体码点 ", length($syms), " 个\n";
PERL_EOF

mkdir -p "$PASSPORT_ROOT/assets/fonts"
SYMS="$(cat "$TMP/codepoints.txt")"
# 码点登记落库：build_pet.sh [2/4] 字体覆盖检查的基准（zcp_strings.c 码点 ⊆ 登记 ∪ ASCII）
printf '%s' "$SYMS" > "$PASSPORT_ROOT/assets/fonts/zcp_font_16.codepoints.txt"
"$LV_FONT_CONV" \
    --font "$FONT_TTF" \
    --size 16 --bpp 4 --format lvgl --no-compress \
    --lv-font-name zcp_font_16 --lv-include lvgl.h \
    --symbols "$SYMS" \
    -o "$FONT_OUT"
# 覆盖断言：生成文件头部的 Opts 行必须记录了同样的码点集（防漏提导致真机 tofu）
grep -q -- "--symbols $SYMS " "$FONT_OUT" || \
    grep -qF -- "--symbols $SYMS" "$FONT_OUT" || die "字体子集码点与请求不一致（检查 $FONT_OUT 头部 Opts）"

grep -q "zcp_font_16" "$PASSPORT_ROOT/assets/README.md" || \
    die "assets/README.md 未登记 zcp_font_16（来源/授权/命令/字符清单），见 docs 规范"

echo "完成："
echo "  $PET_OUT/ball-*.zpf（12 段，EMBED 进固件）"
echo "  $PASSPORT_ROOT/main/zcp_strings.c/.h"
echo "  $FONT_OUT"
echo "  $PASSPORT_ROOT/assets/fonts/zcp_font_16.codepoints.txt（码点登记，build_pet.sh 覆盖检查基准）"
