#!/usr/bin/env bash
# =============================================================================
# fetch-source-ref.sh — 取出「参考 Source」并打补丁 (阶段 1 对打测试用)
#
# 为什么不在仓里存一份: Source 是 2026-09/10 写的现成实现, 已从主仓移除,
# 源码仍在 git 历史 `d878b0a: miracast/src/miracast-source/`。这里按需取,
# 保持主仓精简, 但测试可完全复现。
#
# 用法:
#   ./fetch-source-ref.sh [目标目录]      默认 /tmp/wfd-source-ref
# 产出:
#   <目标目录>/wfd-source ...           已编译的参考 Source (含下面那条修复)
# =============================================================================
set -euo pipefail

DEST="${1:-/tmp/wfd-source-ref}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$HERE" rev-parse --show-toplevel)"
COMMIT="d878b0a"

rm -rf "$DEST"
mkdir -p "$DEST"
git -C "$REPO_ROOT" archive "$COMMIT" miracast/src/miracast-source | tar -x -C "$DEST"

SRC_DIR="$DEST/miracast/src/miracast-source"

# 修参考 Source 的两个 RFC 6184 bug:
#  1) FU-A 的 E 位在每一片上都置位 (chunk 先判 last 后截断) → 收端把每片当完整 NAL
#  2) 一帧最后一个 NAL 不发 RTP marker 位
# 不修的话, 任何标准 Sink (含本目录 sink/) 收到的 H.264 都是坏的。
patch -p1 -d "$SRC_DIR" < "$HERE/0001-fix-fu-a-end-bit-and-marker.patch"

make -C "$SRC_DIR" -j"$(nproc)"

echo "== 参考 Source 就绪: $SRC_DIR/wfd-source"
echo "   用法: $SRC_DIR/wfd-source stream -ip 127.0.0.1 -rtsp 7236 -rtp 49152 -720 -v"
