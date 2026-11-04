#!/usr/bin/env bash
# board-build.sh —— 给 K230 交叉编译 k230ui (LVGL 主界面可执行文件)
#
# 干什么: 用 SDK 自带的 riscv64 交叉工具链 + staging sysroot, 把
#         ui/*.c + src/{ui_actions,backend_client,main_board}.c 编成 k230ui,
#         链接板上已有的 /usr/lib/liblvgl.so (LVGL 9.6.0, 不重复编 LVGL)。
# 怎么用: bash tools/board-build.sh                 # 产物 build-board/k230ui
#         bash tools/board-build.sh --copy          # 顺手 scp 到板子 /usr/bin
# 依赖  : K230 SDK 已经编过一次 (output/<cfg>/staging 有 liblvgl*.so)
# 在哪跑: 本机 (Ubuntu) 交叉编译; 不占用板子
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND="$(dirname "$HERE")"
REPO="$(cd "$FRONTEND/../../../.." && pwd)"     # 仓库根

SDK="${K230_SDK:-$REPO/K230/sdk/k230_linux_sdk}"
CFG="${K230_SDK_CFG:-k230_canmv_01studio_defconfig}"
OUT="$SDK/output/$CFG"
CROSS_BIN="$OUT/host/bin"
SYSROOT="$OUT/staging"
CC="$CROSS_BIN/riscv64-unknown-linux-gnu-gcc"
BUILD="$FRONTEND/build-board"

# 板端 LVGL 配置: 优先用 SDK 现场生成的那份 (保证和 liblvgl.so 完全一致)
SDK_LV_CONF="$(ls -1 "$OUT"/build/lvgl-*/buildroot-build/lv_conf.h 2>/dev/null | head -1 || true)"
if [ -n "$SDK_LV_CONF" ]; then
    LV_CONF="$SDK_LV_CONF"
    echo "[board-build] lv_conf: $LV_CONF (SDK 现场生成)"
else
    LV_CONF="$FRONTEND/lv_conf_board.h"
    echo "[board-build] lv_conf: $LV_CONF (仓内副本)"
fi

for p in "$CC" "$SYSROOT/usr/lib/liblvgl.so"; do
    [ -e "$p" ] || { echo "[board-build] 缺 $p —— 先在 SDK 里跑一次完整编译" >&2; exit 1; }
done

mkdir -p "$BUILD"

SRCS=(
    "$FRONTEND/src/main_board.c"
    "$FRONTEND/src/ui_actions.c"
    "$FRONTEND/src/backend_client.c"
    "$FRONTEND/ui/screens.c"
    "$FRONTEND/ui/styles.c"
    "$FRONTEND/ui/images.c"
    "$FRONTEND/ui/ui.c"
)

echo "[board-build] 编译 ${#SRCS[@]} 个源文件 -> $BUILD/k230ui"
"$CC" -O2 -g -Wall -Wno-unused-parameter \
    -DLV_CONF_PATH="\"$LV_CONF\"" \
    -I"$FRONTEND" -I"$FRONTEND/src" -I"$FRONTEND/ui" \
    -I"$SYSROOT/usr/include" \
    -I"$SYSROOT/usr/include/lvgl" \
    "${SRCS[@]}" \
    -L"$SYSROOT/usr/lib" \
    -Wl,-rpath-link,"$SYSROOT/usr/lib" \
    -llvgl_linux -llvgl -ldisplay -lv4l2-drm -ldrm -lpthread -lm \
    -o "$BUILD/k230ui"

echo "[board-build] OK -> $BUILD/k230ui"
"$CROSS_BIN/riscv64-unknown-linux-gnu-readelf" -h "$BUILD/k230ui" | grep -E "Class|Machine" || true
ls -lh "$BUILD/k230ui"

if [ "${1:-}" = "--copy" ]; then
    BOARD="${BOARD_HOST:-root@10.0.102.101}"
    echo "[board-build] scp -> $BOARD:/usr/bin/k230ui"
    scp "$BUILD/k230ui" "$BOARD:/usr/bin/k230ui"
    echo "[board-build] done"
fi
