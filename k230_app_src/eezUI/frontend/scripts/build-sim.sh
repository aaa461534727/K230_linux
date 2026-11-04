#!/usr/bin/env bash
# build-sim.sh — 编本机模拟器 k230ui-sim (LVGL 9.6.0 + SDL2)
#
# 干什么: cmake 配置 + 编译, 产物在 frontend/build-sim/k230ui-sim
# 怎么用: bash scripts/build-sim.sh            # 增量编
#         bash scripts/build-sim.sh --clean    # 删 build-sim 重来
# 依赖  : gcc, cmake, libsdl2-dev, 已跑过 scripts/fetch-lvgl.sh
# 在哪跑: 本机 (Ubuntu); 板端交叉编译不走这里
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND="$(dirname "$HERE")"
BUILD="$FRONTEND/build-sim"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$BUILD"
fi

if [ ! -f "$FRONTEND/third_party/lvgl/CMakeLists.txt" ]; then
    echo "[build-sim] 缺 LVGL, 先拉: bash scripts/fetch-lvgl.sh" >&2
    exit 1
fi

cmake -S "$FRONTEND" -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD" -j"$(nproc)"

echo "[build-sim] OK -> $BUILD/k230ui-sim"
