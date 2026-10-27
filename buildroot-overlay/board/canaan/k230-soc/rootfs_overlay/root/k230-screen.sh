#!/bin/sh
# k230-screen —— 屏的所有者: 只负责让 DRM 上永远有一层画面 (空闲黑屏)
#
# 为什么单独一个单元:
#   Miracast 与 AirPlay 共用同一个常驻播放器 (drmplay -F /run/wfd-player.fifo)。
#   如果播放器由 Miracast 的启停管理, 那么「联动停 Miracast」会把播放器一起杀掉,
#   屏上就回到「无 plane = 全屏紫」; 切到 AirPlay 又要重新起播放器, 中间那次紫闪
#   就是主人反复抱怨的那一下。所以播放器独立成服务, 投屏两端只做"往管道里写"。
set -u

FIFO="${WFD_PLAYER_FIFO:-/run/wfd-player.fifo}"
PIDFILE="${WFD_PLAYER_PID:-/run/wfd-player.pid}"
OPTS="${WFD_PLAYER_OPTS:--H -p nv12 -w 480 -h 800}"
LOG="${SCREEN_LOG:-/var/log/k230-screen.log}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$LOG"; }

if [ ! -x /usr/bin/drmplay ]; then
    log "✗ /usr/bin/drmplay 不存在, 屏无法接管"
    # 不退出: 退出去只会被 systemd 反复拉起刷日志
    sleep 3600
    exit 1
fi

# 先把老的 -B 黑屏占位让出来 (它持 DRM master, 新的会 SETCRTC: Permission denied)
pkill -9 -x drmplay 2>/dev/null || true
sleep 0.3
rm -f "$FIFO"
mkfifo "$FIFO" 2>/dev/null || true
chmod 666 "$FIFO" 2>/dev/null || true

log "接管屏: $FIFO (空闲 800 ms 自动刷黑)"
# 前台运行, 让 systemd 管生命周期 (Restart=always 兜住意外退出)
exec /usr/bin/drmplay $OPTS -F "$FIFO"
