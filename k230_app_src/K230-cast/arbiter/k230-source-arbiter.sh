#!/bin/sh
# k230-source-arbiter.sh —— K230 统一信源仲裁 (v2)
#
# 目标 (主人 2026-11-02 定):
#   开机把 CarPlay / AirPlay / Miracast / CarLife(无线+有线) 全部拉起来**待命**,
#   但**同一时刻只允许一个真正在投的会话**; 谁先连上谁赢, 赢家连上后其它投屏端全停,
#   会话结束再全部放回去待命。后续加新功能: ① ALL/BOOT 里加名字 ② 写 3 个函数。
#
# 为什么"全部待命"在这块板子上成立 (2026-11-02 实测):
#   · 显示层只有一个写入口: k230-screen 独占 drmplay, 各投屏端只往 /run/wfd-player.fifo 写
#     ⇒ 只要保证"同时只有一个端在写", 屏就不会花/不会抢 CRTC;
#   · CarPlay 与 CarLife 无线共用**同一个热点** (K230-CARPLAY / wlan1 / 192.168.50.1),
#     热点由 carlife-ap.sh 起, 属于基础设施, 仲裁器只保证它在, 不参与"会话"判定;
#   · CarPlay 控制口 7000, AirPlay(uxplay) 改到 7100 ⇒ 两个 AirPlay 系服务可以并存;
#   · Miracast 走 wlan0 的 P2P (unit 里 WFD_IFACE=wlan0), 不占 wlan1 的 AP。
#
# 停止/启动只碰"投屏端进程", **绝不碰** k230-screen / drmplay / pcmplay / hostapd。
set -u

LOG=${LOG:-/var/log/k230-arbiter.log}
STATE=/run/k230-arbiter.state
CONF=${CONF:-/etc/k230-arbiter.conf}
# 热点不归本脚本管 (见下方 ap_ok/check_ap): 开机由 k230-shared-ap.service 起一次。
AP_UNIT=${AP_UNIT:-k230-shared-ap.service}
IDLE_HOLD=${IDLE_HOLD:-3}

[ -f "$CONF" ] && . "$CONF"

# 左 = 优先级高。多个探针同时为真时, 列表里靠前的那项赢。
# usbmirror 排最前: 手机插着 USB 线在投时, 有线这一路最直接 (零压缩损失, 延迟最低),
# 它一旦在投, 无线那几路必须停 —— 两条管线同时写 /run/wfd-player.fifo 会花屏。
ALL=${ALL:-"usbmirror carplay airplay miracast carlife_wifi carlife_usb"}
BOOT=${BOOT:-"usbmirror carplay airplay miracast carlife_wifi carlife_usb"}

log() { printf '[%s] %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >>"$LOG"; }

# ---------------- 通用工具 ----------------
# 统计若干本地/对端端口上的已建立连接数 (busybox awk 也能跑, 不依赖 ss 的列顺序)
conn_cnt() {
    ss -tn state established 2>/dev/null | awk -v list="$*" '
        BEGIN { n = split(list, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
        NR > 1 {
            for (i = 1; i <= NF; i++) {
                k = $i; sub(/.*:/, "", k); if (k in want) { c++; break }
            }
        }
        END { print c + 0 }'
}

unit_on() { systemctl is-active --quiet "$1" 2>/dev/null; }
ensure_on() {  # ensure_on <unit> <tag>
    unit_on "$1" && return 0
    log "▶ 起 $2 ($1)"
    if ! systemctl start "$1" >>"$LOG" 2>&1; then
        # 上一轮可能是异步 stop, unit 还在 deactivating ⇒ 等一拍再来一次
        sleep 2
        unit_on "$1" && return 0
        log "▶ 再试一次 $2 ($1)"
        systemctl start "$1" >>"$LOG" 2>&1 || log "⚠ 起 $1 失败"
    fi
}
ensure_off() { # ensure_off <unit> <tag>
    unit_on "$1" || return 0
    log "⏹ 停 $2 ($1)"
    systemctl stop "$1" >>"$LOG" 2>&1 || log "⚠ 停 $1 失败"
}
# 快速停: 先同步发停止请求的**异步版**再立刻杀进程。
# 为什么需要: 2026-11-02 实测停 k230-miracast 要 **18 s** (wfd-p2p.sh 收尾慢),
#   加上 carlife_wifi 4 s, 一轮切换 22 s 空窗; 而"赢家要独占屏"只要求对方
#   **立刻不再往 /run/wfd-player.fifo 写**, 不需要等 systemd 收尾完。
#   所以: systemctl --no-block stop (排队) + pkill (进程秒退) ⇒ 切换 <1 s。
fast_off() {   # fast_off <unit> <tag> <pkill 模式...>
    _u=$1; _t=$2; shift 2
    if unit_on "$_u"; then
        log "⏹ 停 $_t ($_u, 快速)"
        systemctl --no-block stop "$_u" >>"$LOG" 2>&1 || log "⚠ 停 $_u 失败"
    fi
    # ★ 连杀 4 轮: Miracast 的管线是 `while :; do sink | v4l2_dec; done` **自重启**,
    #   只杀一次会在 0.5 s 后原地复活; 复活的那个 v4l2_dec 会重新打开
    #   /run/wfd-player.fifo, 和赢家抢同一条显示管道 ⇒ 画面一顿一顿
    #   (2026-11-02 现场: AirPlay 投屏时 fifo 上有两个 v4l2_dec 写端)。
    _i=0
    while [ "$_i" -lt 4 ]; do
        _hit=0
        for _pat in "$@"; do
            # ★ 2026-11-03: 原来直接 pkill -f, 但各路的命令行**长得一样**
            #   (Miracast / 有线投屏 都是 v4l2_dec /dev/stdin /run/wfd-player.fifo -H -D),
            #   停 Miracast 时会把**有线投屏那条管线**的解码器一起杀掉
            #   ⇒ usb_mirror 的 stdout 断掉退出 ⇒ run.sh 重启 ⇒ 新的 adb 会话
            #   ⇒ 手机又弹一次授权框。所以杀之前先看 /proc/<pid>/cgroup 属不属于本 unit。
            for _p in $(pgrep -f "$_pat" 2>/dev/null); do
                [ "$_p" = "$$" ] && continue
                grep -q "$_u" "/proc/$_p/cgroup" 2>/dev/null || continue
                kill "$_p" 2>/dev/null && _hit=1
            done
        done
        [ "$_hit" = 0 ] && break
        _i=$((_i + 1))
        sleep 0.4
    done
}

# 共享热点 (CarPlay / CarLife 无线): **只读体检, 永不写网卡**。
# 2026-11-02 现场教训 (两次整机失联, 都只能断电恢复):
#   运行期对 wlan1 做 ip link down/up + 重起 hostapd, 8189fs (SDIO WiFi) 会挂死内核,
#   表现为日志停在那一步、SSH 全断。热点因此改为**开机由 k230-shared-ap.service 起一次**,
#   本仲裁器只检测并打日志, 绝不动网络 (要恢复热点请手动 systemctl restart k230-shared-ap)。
ap_ok() {
    # 注意: ① iw 6.x 不支持 "iw dev wlan1", 必须从 "iw dev" 里按 Interface 分块找;
    #      ② 块里 type 在第 6 行 (Interface/ifindex/wdev/addr/ssid/type), -A4 抓不到。
    iw dev 2>/dev/null | grep -A8 'Interface wlan1' | grep -q 'type AP' \
        && pgrep -x hostapd >/dev/null 2>&1
}
check_ap() {   # 只报警, 不动手
    ap_ok && return 0
    log "⚠ 共享热点不在 (wlan1 非 AP 或无 hostapd) —— 仲裁器不自动重起 (会挂内核); 需要时手动: systemctl restart $AP_UNIT"
    return 1
}

# ---------------- 会话探针 (真=有会话) ----------------
probe_carplay()     { [ "$(conn_cnt 7000)" -gt 0 ]; }
probe_airplay()     { [ "$(conn_cnt 7100 7101)" -gt 0 ]; }
probe_miracast()    { [ "$(conn_cnt 7236)" -gt 0 ] || iw dev wlan0 link 2>/dev/null | grep -qi 'connected to'; }
probe_carlife_wifi(){ [ "$(ss -tn state established 2>/dev/null | awk '
        NR > 1 { for (i = 1; i <= NF; i++) { k = $i; sub(/.*:/, "", k)
                 if (k ~ /^(7240|8240|9240|9241|9242|9340|9440)$/) { c++; break } } }
        END { print c + 0 }')" -gt 0 ]; }
# 有线投屏: 标记文件只在 usb_mirror **真的开出 screenrecord 通道**时才写,
# 所以"手机插着但没人授权/没在投"不会被误判成会话。
probe_usbmirror()   { [ -f /run/usbmirror-active ]; }

probe_carlife_usb() {  # 安卓已进 AOA accessory 模式 = 有线 CarLife 会话
    for v in /sys/bus/usb/devices/*/idVendor; do
        [ -f "$v" ] || continue
        [ "$(cat "$v" 2>/dev/null)" = "18d1" ] || continue
        case "$(cat "${v%idVendor}idProduct" 2>/dev/null)" in
            # ★ 2026-11-03: AOA 有两种 —— 纯 accessory (2d00/2d02) 是 CarLife 在谈;
            #   而 "accessory + ADB" (2d01/2d03/2d04/2d05) **仍然有 ADB 接口**,
            #   我们的有线投屏 (usb_mirror) 在这种模式下连 adb 照样能通 (实测 18d1:2d01:
            #   if1.1 = ff/42/01, ep_in 0x82 / ep_out 0x03, getprop 一次成功)。
            #   所以只要没有 carlife_sink 在跑, 就不该把这种状态判成 CarLife 会话 ——
            #   否则板子重启后手机会被 carlife 抢走, 有线投屏永远起不来。
            2d01|2d03|2d04|2d05)
                pgrep -x carlife_sink >/dev/null 2>&1 && return 0
                ;;
            2d0*) return 0 ;;
        esac
    done
    return 1
}

# ---------------- 起 / 停 ----------------
start_carplay()     { ensure_on carplay-arbiter "CarPlay 传输仲裁"; ensure_on carplay-airplayd "CarPlay AirPlay 服务"; ensure_on carplay-wireless "CarPlay 蓝牙发现"; }
stop_carplay()      { ensure_off carplay-wireless "CarPlay 蓝牙发现"; ensure_off carplay-airplayd "CarPlay AirPlay 服务"; rm -f /run/airplay-active; }

start_airplay()     { ensure_on k230-airplay "AirPlay 镜像"; }
stop_airplay()      { ensure_off k230-airplay "AirPlay 镜像"; }

start_miracast()    { ensure_on k230-miracast "Miracast"; }
# 模式必须覆盖"整条管线", 否则自重启循环会复活:
#   wfd-p2p.sh(主控) · wfd-sink(收流) · v4l2_dec /dev/stdin(解码) · PLAYER_BIN=(重启循环外壳)
stop_miracast()     { fast_off k230-miracast "Miracast" 'wfd-p2p.sh' 'wfd-sink' 'v4l2_dec /dev/stdin' 'PLAYER_BIN='; }

# 只在 USB 总线上真有 adb 手机时才起 —— 否则 usb_mirror 起来也是空转 (每 2 s 枚举一次),
# 而且一旦它被起/停一次就是**一个新的 adb 会话** = 这台 nubia ROM 上又多一次授权弹窗。
# 所以这里先查"插没插", 没插就什么都不做。
usb_phone_present() {
    for v in /sys/bus/usb/devices/*/idVendor; do
        [ -f "$v" ] || continue
        case "$(cat "$v" 2>/dev/null)" in
            19d2|18d1|12d1|2717|2d01|05c6) return 0 ;;
        esac
    done
    return 1
}
start_usbmirror()   { usb_phone_present || { log "· 没插安卓手机, 跳过有线投屏"; return 0; }
                      ensure_on k230-usbmirror "有线投屏 (adb)"; }
# 只有"它真的在往 fifo 写"(标记在) 时才杀 —— 必须连 run.sh 一起杀, 因为那是个
# while 自重启外壳, 只杀 usb_mirror 会 1 s 后复活继续写。
# 没在写的时候**故意不杀**: 杀掉再起来就是一次新的 adb 会话, 这台 nubia ROM 会再弹
# 一次授权框 (见 usbmirror/README.md)。空转的 usb_mirror 不碰 fifo, 留着无害。
stop_usbmirror()    { [ -f /run/usbmirror-active ] || return 0
                      fast_off k230-usbmirror "有线投屏 (adb)" 'usb_mirror' 'usbmirror/run.sh'; }

start_carlife_wifi(){ ensure_on k230-carlife-wifi "CarLife 无线"; }
# 2026-11-03: 原来按 carlife_sink --wifi / --width 精确匹配, 但实际命令行是 carlife_sink --verbose
# (unit 里 ExecStart 的写法) => 一次都没杀掉, 进程变孤儿继续占着 ADB/AOA 通道
# => probe_carlife_usb 恒真 => 有线投屏永远抢不回屏。
# 现在统一用宽模式 carlife_sink, 安全性由 fast_off 的 cgroup 过滤保证 (只杀本 unit 的进程)。
stop_carlife_wifi() { fast_off k230-carlife-wifi "CarLife 无线" 'carlife_sink'; }

start_carlife_usb() { ensure_on k230-carlife "CarLife 有线"; }
stop_carlife_usb() { fast_off k230-carlife "CarLife 有线" 'carlife_sink'; }

probe()  { "probe_$1"; }
startf() { "start_$1"; }
stopf()  { "stop_$1"; }

stop_others() {  # stop_others <赢家>
    keep=$1
    for f in $ALL; do
        [ "$f" = "$keep" ] && continue
        stopf "$f"
    done
}

start_all() {
    for f in $BOOT; do
        startf "$f"
    done
}

# ---------------- 主循环 ----------------
log "仲裁器启动: 优先级=[$ALL] 待命顺序=[$BOOT] 会话结束保持=${IDLE_HOLD}s"
check_ap
start_all
log "开机已拉起全部投屏端 (共享热点 + $BOOT)"

cur=""          # 当前赢家
idle_since=0
while :; do
    # 热点只做体检, 10 s 一次, 避免每分钟都去碰 wlan1
    _n=$(( ${_n:-0} + 1 ))
    [ $((_n % 30)) -eq 0 ] && check_ap

    win=""
    for f in $ALL; do
        if probe "$f"; then win=$f; break; fi
    done

    if [ -n "$win" ] && [ "$win" != "$cur" ]; then
        log "🎯 会话: $win → 停掉其它投屏端 (热点保留)"
        stop_others "$win"
        startf "$win"          # 赢家可能刚被别的 stop 连带停掉, 补一次
        echo "$win" >"$STATE"
        cur=$win
        idle_since=0
    elif [ -z "$win" ] && [ -n "$cur" ]; then
        now=$(date +%s)
        [ "$idle_since" = 0 ] && idle_since=$now
        if [ $((now - idle_since)) -ge "$IDLE_HOLD" ]; then
            log "⏸ 会话 $cur 已结束 → 全部投屏端放回待命"
            start_all
            echo "" >"$STATE"
            cur=""
            idle_since=0
        fi
    elif [ -n "$win" ]; then
        idle_since=0
    fi

    sleep 1
done
