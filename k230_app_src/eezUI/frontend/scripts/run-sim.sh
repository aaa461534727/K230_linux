#!/usr/bin/env bash
# run-sim.sh — 跑本机模拟器 (鼠标 = 触摸)
#
# 干什么: 开一个 480x800 窗口显示 K230 主界面
# 怎么用: bash scripts/run-sim.sh
#         K230UI_SOCK=/tmp/k230ui.sock bash scripts/run-sim.sh   # 指定后端 socket
# 依赖  : 先跑 scripts/build-sim.sh; 有 X/Wayland 桌面 (或 DISPLAY 指向 Xvfb)
# 在哪跑: 本机 (Ubuntu)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND="$(dirname "$HERE")"
BIN="$FRONTEND/build-sim/k230ui-sim"

if [ ! -x "$BIN" ]; then
    echo "[run-sim] 没有 $BIN, 先跑: bash scripts/build-sim.sh" >&2
    exit 1
fi

exec "$BIN" "$@"
