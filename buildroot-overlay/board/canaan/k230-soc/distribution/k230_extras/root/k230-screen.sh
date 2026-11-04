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
# ★ 2026-11-03 主人定「不管手机什么尺寸都铺满」: 用 -S (stretch) 而不是 -f (cover)。
#   cover 等比会把 337x714 的内容上下各裁 108 行 (21%), 状态栏/导航栏看不见;
#   stretch 两轴各自缩到 480x800 ⇒ 一点内容不丢, 代价是轻微压扁 (主人明确接受)。
# ai2dplay: 硬件 NV12→AR24 + 缩放(→480x800), 恒读 16B 帧头, 无需 -p/-S/-H
OPTS="${WFD_PLAYER_OPTS:--w 480 -h 800}"
LOG="${SCREEN_LOG:-/var/log/k230-screen.log}"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >>"$LOG"; }

if [ ! -x /usr/bin/ai2dplay ]; then
    log "✗ /usr/bin/ai2dplay 不存在, 屏无法接管"
    # 不退出: 退出去只会被 systemd 反复拉起刷日志
    sleep 3600
    exit 1
fi

# ★ 2026-10-28: 抬内核管道上限 (drmplay 靠 F_SETPIPE_SZ 定容, 上限受这个 sysctl 约束)
#   为什么要 4 MB: 解码出图改回「原始尺寸」(720p NV12 一帧 1350 KB) 后, 1 MB 管道
#   只装得下 0.7 帧 ⇒ 一帧要 read() 6 次。drmplay 自己会按「2 帧 + 32 KB」申请
#   (环境变量 WFD_PIPE_CAP 可覆盖), 但内核默认 pipe-max-size 只有 1 MB, 不在这里
#   抬上去的话申请会被拒并回落到 256 KB —— 而且这是**内核参数, 重启即失**,
#   只能每次开机在起播放器之前写。写失败不致命 (drmplay 会退到 1 MB/256 KB 并打日志)。
PMAX=/proc/sys/fs/pipe-max-size
if [ -w "$PMAX" ]; then
    PMAX_CUR=$(cat "$PMAX" 2>/dev/null || echo 0)
    if [ "${PMAX_CUR:-0}" -lt 4194304 ] 2>/dev/null; then
        if echo 4194304 >"$PMAX" 2>/dev/null; then
            log "管道上限: ${PMAX_CUR} → $(cat "$PMAX" 2>/dev/null) (FIFO 可定容到 4 MB)"
        else
            log "⚠ 管道上限写不进去 (仍是 ${PMAX_CUR}), 大帧会退回 1 MB 管道"
        fi
    fi
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
pkill -9 -x ai2dplay 2>/dev/null || true
sleep 0.3
rm -f "$PIDFILE"
# 只有这里允许重建管道, 且一定在「玩家已全部杀掉」之后 ⇒ 不会有人拿着旧 inode
rm -f "$FIFO"
mkfifo "$FIFO" 2>/dev/null || true
chmod 666 "$FIFO" 2>/dev/null || true

# ★ exec 不换 pid ⇒ 这个 pidfile 就是后面 drmplay 的 pid (体检靠它)
echo $$ >"$PIDFILE"
exec 9>&-
# ★ 2026-11-03 零拷贝默认开 (无线三路一起受益, 板上 A/B 实测见下):
#   1080p 源 (Miracast 真机) 同一段 12 s / 30 fps ES, 只改帧体通道:
#     走管道: 管道延迟 avg 82.6~85.0 ms max 138 ms, 每帧CPU 14.50 ms
#     WFD_SHM: 管道延迟 avg 19.4~21.2 ms max  49 ms, 每帧CPU 12.16 ms
#   原因: 管道定容 2 MB = 720p 1.4 帧 / 1080p 0.9 帧, 帧体要拆成几十次 read(),
#   单核上每趟都是一次上下文切换, 而且积压的帧只能排队 —— 延迟是纯排队, 不是算力。
#   ⚠ 解码侧必须与这里一致: wfd-p2p.sh / k230-airplay / carlife 都 export 同一个值。
#   回滚: 在对应 unit 里写 Environment=WFD_SHM= (空串 = 关闭), 两端会一起回到老路。
export WFD_SHM="${WFD_SHM-/run/wfd-frame}"
log "接管屏: $FIFO (pid $$, 空闲 800 ms 自动刷黑)"
[ -n "${WFD_SHM:-}" ] && log "帧通道: 共享内存 ${WFD_SHM} (零拷贝)"
# 前台运行, 让 systemd 管生命周期 (Restart=always 兜住意外退出)
exec /usr/bin/ai2dplay $OPTS -F "$FIFO"
