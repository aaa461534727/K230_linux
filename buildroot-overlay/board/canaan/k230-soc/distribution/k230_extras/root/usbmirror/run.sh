#!/bin/sh
# 起「安卓 USB 有线投屏」: usb_mirror(adb 取流) | v4l2_dec(VPU 硬解) | 常驻 drmplay
#
# 前置: k230-screen.service 必须在跑 (它独占 drmplay + /run/wfd-player.fifo)
# 用法: sh run.sh [--touch] [--bit-rate 8000000]
#
# ★ 2026-11-03 三个提速开关 (实测数据见 README「延迟从哪来」):
#   WFD_SHM=/run/wfd-frame  帧体走共享内存两槽, 管道里只剩 16 B 帧头 + 4 B 槽号。
#       不开的时候 FIFO 定容 4 MB, 720p 一帧 1.35 MB ⇒ 管道里能囤 2~3 帧 = 40~60 ms
#       纯排队延迟; 开了以后 dec 最多领先 1 帧 (槽头有读序号回压), 排队延迟 ~1 帧。
#   DEC_AU=2                一个完整 AU 就投给解码器, 不攒满 64 KB chunk 再切
#       ⇒ 少一个「等下一个 NAL 到齐」的延迟。
#   -s <屏宽> <屏高>        VPU 解码时**顺便**做硬件下采样 (MVE DEC_DOWNSCALE)。
#       这是本板最便宜的缩放器 (比 AI2D 便宜: 不用先把 NV12 转 RGB)。
#       码流缩到屏尺寸后, drmplay 走 NV12 1:1 直贴快路径:
#         每帧 CPU 12.6 ms → 3.8 ms, 转换 12.7 ms → 3.1 ms (见 README 实测表)
set -u
DIR=$(cd "$(dirname "$0")" && pwd)
DEC=${DEC_BIN:-/usr/bin/v4l2_dec}
FIFO=${PLAYER_FIFO:-/run/wfd-player.fifo}
SHM=${WFD_SHM:-/run/wfd-frame}
# 屏尺寸 (drmplay -w/-h 用的那组); 设 DSL=0 可关掉 VPU 缩放
PANEL_W=${USBMIRROR_PANEL_W:-480}
PANEL_H=${USBMIRROR_PANEL_H:-800}
ARGS="$*"

[ -p "$FIFO" ] || { echo "✗ 没有 $FIFO (k230-screen 没起?)"; exit 1; }
[ -x "$DIR/usb_mirror" ] || { echo "✗ 先跑 ./build.sh"; exit 1; }

DEC_OPTS="-H -D"
[ -n "$SHM" ] && export WFD_SHM="$SHM"
export DEC_AU=${DEC_AU:-2}
# usb_mirror 自己有 --size, 但默认按屏算; 这里显式传一遍, 保证和 -s 一致
if [ "${USBMIRROR_DSL:-1}" = 1 ]; then
    VIW=${USBMIRROR_VIDEO_W:-448}
    VIH=${USBMIRROR_VIDEO_H:-800}
    ARGS="$ARGS --size ${VIW}x${VIH}"
    DEC_OPTS="$DEC_OPTS -s $PANEL_W $PANEL_H"
fi

while :; do
    "$DIR/usb_mirror" $ARGS | "$DEC" /dev/stdin "$FIFO" $DEC_OPTS
    echo "[run] 管线退出, 1 s 后重来"
    sleep 1
done
