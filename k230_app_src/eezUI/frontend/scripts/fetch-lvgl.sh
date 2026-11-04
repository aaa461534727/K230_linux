#!/usr/bin/env bash
# fetch-lvgl.sh — 把 LVGL 9.6.0 拉到 frontend/third_party/lvgl (本机模拟器编译用)
#
# 干什么: 浅克隆 lvgl v9.6.0 到 third_party/lvgl, 版本对不上就重新拉。
# 怎么用: bash scripts/fetch-lvgl.sh            # 已存在且版本对 -> 直接跳过
#         bash scripts/fetch-lvgl.sh --force    # 强制重拉
# 依赖  : git + 网络 (或先把 lvgl-9.6.0 目录放到 third_party/lvgl)
# 在哪跑: 本机 (Ubuntu, 模拟器); 板端不用 —— 板子用 SDK 自带 liblvgl.so
# 产出  : frontend/third_party/lvgl/  (不入仓, 已在 .gitignore 排除)
set -euo pipefail

LVGL_VERSION="v9.6.0"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND="$(dirname "$HERE")"
DEST="$FRONTEND/third_party/lvgl"

force=0
[ "${1:-}" = "--force" ] && force=1

if [ -f "$DEST/lv_version.h" ] && [ "$force" -eq 0 ]; then
    have="$(awk -F'[ .]' '/LVGL_VERSION_MAJOR/{m=$3} /LVGL_VERSION_MINOR/{n=$3}
        END{printf "v%d.%d", m, n}' "$DEST/lv_version.h")"
    if [ "$have" = "$(echo "$LVGL_VERSION" | cut -d. -f1,2)" ]; then
        echo "[fetch-lvgl] 已有 $DEST ($have), 跳过 (要重拉加 --force)"
        exit 0
    fi
    echo "[fetch-lvgl] $DEST 版本是 $have, 需要 $LVGL_VERSION, 重新拉"
    rm -rf "$DEST"
fi

mkdir -p "$FRONTEND/third_party"
rm -rf "$DEST"
echo "[fetch-lvgl] cloning lvgl $LVGL_VERSION -> $DEST"
git clone --depth 1 --branch "$LVGL_VERSION" https://github.com/lvgl/lvgl.git "$DEST"

echo "[fetch-lvgl] done:"
grep -E 'LVGL_VERSION_(MAJOR|MINOR|PATCH)' "$DEST/lv_version.h"
