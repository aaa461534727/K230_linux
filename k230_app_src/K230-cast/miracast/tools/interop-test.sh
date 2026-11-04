#!/usr/bin/env bash
# =============================================================================
# interop-test.sh — 阶段 1 验收: Sink(本仓新写) ↔ Source(git d878b0a 参考实现) 对打
#
# 判定 (全满足才算 PASS):
#   1. sink 输出 "RESULT: PASS"      —— M1~M7 握手 + 收到 SPS/PPS + 收到完整图像
#   2. sink 输出 "rtp_seq_lost=0"    —— 本机链路零丢包 (排除"我们收不过来"的可能)
#   3. dump 出来的 H.264 能被 ffprobe 解码, 且帧数 > 0、无解码错误行
#
# 用法: ./interop-test.sh [持续秒数, 默认 20]
# =============================================================================
set -uo pipefail

SECS="${1:-20}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SINK_DIR="$HERE/../sink"
SRC_DIR="${WFD_SOURCE_DIR:-/tmp/wfd-source-ref/miracast/src/miracast-source}"
DUMP=/tmp/wfd-interop.h264
LOG=/tmp/wfd-sink-interop.log
RTP_PORT=27460

command -v ffprobe >/dev/null || { echo "✗ 需要 ffprobe"; exit 1; }

if [ ! -x "$SRC_DIR/wfd-source" ]; then
    echo "== 参考 Source 不在, 先拉取..."
    "$HERE/fetch-source-ref.sh" >/dev/null
fi

make -C "$SINK_DIR" >/dev/null || { echo "✗ sink 编译失败"; exit 1; }
rm -f "$DUMP" "$LOG"

echo "== 1/3 启动 Sink (RTSP 7236, RTP $RTP_PORT)"
"$SINK_DIR/wfd-sink" -p 7236 -r "$RTP_PORT" -d "$DUMP" -t $((SECS + 8)) > "$LOG" 2>&1 &
SINK_PID=$!
sleep 1

echo "== 2/3 启动 Source (直连 127.0.0.1, 720p30, ${SECS}s)"
timeout "$SECS" "$SRC_DIR/wfd-source" stream -ip 127.0.0.1 -rtsp 7236 -rtp 49152 -720 \
    > /tmp/wfd-source-interop.log 2>&1
wait "$SINK_PID" 2>/dev/null

echo "== 3/3 判定"
rc=0
grep -q "RESULT: PASS" "$LOG" || { echo "✗ sink 未通过握手/收流"; rc=1; }
grep -q "rtp_seq_lost=0" "$LOG" || { echo "✗ 有 RTP 丢包 (本机链路)"; rc=1; }

grep -E "SUMMARY|rtp_video_pkts|frames=|seq_lost|RESULT|resolution|SPS|PPS" "$LOG"

if command -v ffprobe >/dev/null && [ -s "$DUMP" ]; then
    err=$(ffprobe -hide_banner -v error -count_frames -select_streams v:0 \
          -show_entries stream=nb_read_frames,width,height -of default=nw=1 "$DUMP" 2>&1)
    echo "--- ffprobe:"; echo "$err"
    echo "$err" | grep -q "^nb_read_frames=[1-9]" || { echo "✗ dump 无法解码"; rc=1; }
    echo "$err" | grep -qi "error\|corrupt" && { echo "✗ dump 有解码错误"; rc=1; }
else
    echo "✗ 没有 dump 文件"; rc=1
fi

[ $rc -eq 0 ] && echo "✅ 阶段 1 对打测试 PASS" || echo "❌ 阶段 1 对打测试 FAIL"
exit $rc
