#!/bin/sh
# k230-screen —— 屏的所有者: 只负责让 DRM 上永远有一层画面 (空闲黑屏)
#
# 为什么单独一个单元:
#   Miracast 与 AirPlay 共用同一个常驻播放器 (drmplay -F /run/wfd-player.fifo)。
#   如果播放器由 Miracast 的启停管理, 那么「联动停 Miracast」会把播放器一起杀掉,
#   屏上就回到「无 plane = 全屏紫」; 切到 AirPlay 又要重新起播放器, 中间那次紫闪
#   就是主人反复抱怨的那一下。所以播放器独立成服务, 投屏两端只做"往管道里写"。
#
# ★ 2026-10-27 修「手机提示投屏成功但 K230 黑屏」(现场 16:00):
#   真实原因是**三个地方各写了一份「起播放器」逻辑** (本文件 / k230-airplay /
#   wfd-p2p.sh), 都是"先看没有玩家 → rm -f 管道 → mkfifo → setsid drmplay"。
#   开机或一起重启时三者同时进入这段 ⇒ 后到的那个把**别人刚建好的管道 inode 删掉**,
#   自己再建一个新的; 先到的玩家还活着, 但它的 fd 指向**已被删除的旧 inode**
#   (`/proc/<pid>/fd/0 -> /run/wfd-player.fifo (deleted)`), 而当前路径上的新管道
#   **没有读者** ⇒ v4l2_dec 的 `fopen(fifo,"wb")` 永久阻塞在 open() 上 (连日志都不打)
#   ⇒ 手机端连接/RTSP/收流全正常, 屏上却一个像素都没有。
#   实证: `for p in /proc/*/fd/*; do readlink; done` 只剩一个 drmplay 拿着 (deleted) 管道,
#        `awk '{print $3}' /proc/<dec>/stat` = S + `wchan = wait_for_partner` (卡在 open 写端)。
#
#   现在的规矩 (**唯一所有者**):
#     · **只有本文件能 rm/mkfifo 这个管道** (而且一定先 pkill 掉所有玩家);
#     · 起玩家这一段加 flock, 别的启动器不会插进来;
#     · 本文件写 pidfile (用 $$, exec 后 pid 不变) ⇒ 外面能可靠判断"玩家是谁";
#     · k230-airplay / wfd-p2p.sh 一律**不自己起玩家**, 只体检 + `systemctl restart k230-screen`。
set -u

FIFO="${WFD_PLAYER_FIFO:-/run/wfd-player.fifo}"
PIDFILE="${WFD_PLAYER_PID:-/run/wfd-player.pid}"
LOCK="${WFD_PLAYER_LOCK:-/run/wfd-player.lock}"
OPTS="${WFD_PLAYER_OPTS:--H -p nv12 -w 480 -h 800}"
LOG="${SCREEN_LOG:-/var/log/k230-screen.log}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$LOG"; }

if [ ! -x /usr/bin/drmplay ]; then
    log "✗ /usr/bin/drmplay 不存在, 屏无法接管"
    # 不退出: 退出去只会被 systemd 反复拉起刷日志
    sleep 3600
    exit 1
fi

# 互斥: 「杀玩家 + 重建管道 + 写 pidfile + 起玩家」必须原子
# ⚠ 不要写 `exec 9>"$LOCK" 2>/dev/null` —— `exec` 只带重定向时是**改本 shell 自己的 fd**,
#   那会把 stderr 永久接到 /dev/null, 后面 exec 出去的 drmplay 就再也打不出日志了
#   (2026-10-27 16:05 我亲手踩的: 屏黑 + journal 里一行 drmplay 都没有, 完全瞎)。
exec 9>"$LOCK" || exec 9>/dev/null
if command -v flock >/dev/null 2>&1; then
    flock -w 5 9 2>/dev/null || log "⚠ 取锁超时 (另一个启动器在跑), 继续 (后面有 pkill 兜底)"
fi

# 先把老的玩家让出来 (它持 DRM master, 新的会 SETCRTC: Permission denied)
pkill -9 -x drmplay 2>/dev/null || true
sleep 0.3
rm -f "$PIDFILE"
# 只有这里允许重建管道, 且一定在「玩家已全部杀掉」之后 ⇒ 不会有人拿着旧 inode
rm -f "$FIFO"
mkfifo "$FIFO" 2>/dev/null || true
chmod 666 "$FIFO" 2>/dev/null || true

# ★ exec 不换 pid ⇒ 这个 pidfile 就是后面 drmplay 的 pid (体检靠它)
echo $$ >"$PIDFILE"
exec 9>&-
log "接管屏: $FIFO (pid $$, 空闲 800 ms 自动刷黑)"
# 前台运行, 让 systemd 管生命周期 (Restart=always 兜住意外退出)
exec /usr/bin/drmplay $OPTS -F "$FIFO"
