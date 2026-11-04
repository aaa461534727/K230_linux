#!/bin/sh
# k230-ui-mode.sh —— LVGL 主界面 + 投屏会话 的"屏幕让位"总控
#
# 为什么需要这个脚本 (核心矛盾):
#   DRM 只有一个 master。板上有两个都想画屏的进程:
#     · k230-ui      = LVGL 主界面 (本项目的 k230ui 程序)
#     · k230-screen  = 常驻 drmplay, 读 /run/wfd-player.fifo 显示投屏画面
#   两者**绝对不能同时开** (后开的那个拿不到 DRM master, 屏幕直接黑/紫)。
#   所以模式切换必须是"先停对方, 再起自己", 且由**一个地方**统一决定谁该上屏。
#   这个脚本就是那个"一个地方"。
#
# 模式 (写进 $STATE):
#   ui        主界面占屏: k230-screen 停, 所有投屏端停, 仲裁器停, k230-ui 起
#   <service> 投屏占屏: k230-ui 停, k230-screen 起, 只起该路投屏端, 仲裁器停
#   auto      交还开机仲裁器: 所有投屏端放回待命 + k230-screen 起 + 仲裁器起 (k230-ui 停)
#
# 用法:
#   k230-ui-mode.sh ui                  # 让主界面占屏 (空闲态)
#   k230-ui-mode.sh select miracast     # 让 Miracast 占屏 (miracast/airplay/carlife/carplay)
#   k230-ui-mode.sh stop                # 停会话, 回主界面
#   k230-ui-mode.sh auto                # 停会话, 交给 k230-source-arbiter
#   k230-ui-mode.sh probe <service>     # 会话还在吗 (exit 0 = 在)
#   k230-ui-mode.sh status              # 打印当前模式
#
# 退出码: 0 = 成功, 1 = 参数错/失败, 2 = 未知 service
set -u

STATE=${K230UI_STATE:-/run/k230-ui-mode.state}
LOG=${K230UI_LOG:-/var/log/k230-ui.log}
CONF=${K230UI_CONF:-/etc/k230-ui.conf}

# 各模式用到的 systemd 单元 (可在 /etc/k230-ui.conf 覆盖)
UI_BIN=${UI_BIN:-/usr/bin/k230ui}   # 主界面可执行文件 (本机联调可指到 build-sim/)
UI_UNIT=${UI_UNIT:-k230-ui}
SCREEN_UNIT=${SCREEN_UNIT:-k230-screen}
ARBITER_UNIT=${ARBITER_UNIT:-k230-source-arbiter}

# 4 个按钮 -> 要起的 unit 列表
UNITS_miracast=${UNITS_miracast:-k230-miracast}
UNITS_airplay=${UNITS_airplay:-k230-airplay}
# CarLife 默认走**无线** (有线 USB 会把本板 USB 网卡切走 => SSH 断)
UNITS_carlife=${UNITS_carlife:-k230-carlife-wifi}
UNITS_carplay=${UNITS_carplay:-carplay-arbiter carplay-airplayd carplay-wireless}

# 一旦切模式就要先清干净的"其它路" (手动模式的互斥靠它, 不靠仲裁器)
ALL_SERVICES=${ALL_SERVICES:-"miracast airplay carlife carplay"}

[ -f "$CONF" ] && . "$CONF"

log() { printf '[%s] %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >>"$LOG" 2>/dev/null; }
say() { printf '%s\n' "$*"; }

unit_on()  { systemctl is-active --quiet "$1" 2>/dev/null; }

ensure_on() {
    unit_on "$1" && return 0
    log "▶ 起 $1"
    systemctl start "$1" >>"$LOG" 2>&1 && return 0
    sleep 1
    systemctl start "$1" >>"$LOG" 2>&1 && return 0
    log "⚠ 起 $1 失败"
    return 1
}

# 停单元 (可一次停多个): 先 --no-block 全部排队, 再**一轮**按 cgroup 精确杀
#   · 为什么 --no-block: 2026-11-02 实测 stop k230-miracast 要 18 s (wfd-p2p.sh 收尾慢),
#     而"让位"只要求它**立刻停止往 fifo 写 / 立刻放开 DRM**, 不需要等 systemd 收尾;
#   · 为什么按 cgroup 杀: 各路的命令行长得一样 (v4l2_dec /dev/stdin /run/wfd-player.fifo),
#     按命令行 pkill 会误伤正在投的另一路; /proc/<pid>/cgroup 里的 unit 名是精确的;
#   · 为什么连杀 6 轮: Miracast 是 `while :; do sink | dec; done` 自重启外壳,
#     只杀一次 0.5 s 后原地复活, 复活的那个重新打开 fifo 和赢家抢管道;
#   · 为什么**合并成一轮**: 逐个 unit 串行停, 每个都要走一遍 /proc 扫描 + 重试,
#     板上实测"回主界面"要 19 s (黑屏 19 s 才看到界面)。合并后所有 unit 同时下发
#     stop, 再一轮扫描统一杀, 实测 ~3 s。
fast_off_many() {
    _fm_units="$*"
    [ -n "${_fm_units:-}" ] || return 0
    _fm_any=0
    for _fm_u in $_fm_units; do
        unit_on "$_fm_u" || continue
        _fm_any=1
        log "⏹ 停 $_fm_u"
        systemctl --no-block stop "$_fm_u" >>"$LOG" 2>&1 || log "⚠ 停 $_fm_u 失败"
    done
    [ "$_fm_any" = 1 ] || return 0

    _fm_i=0
    while [ "$_fm_i" -lt 6 ]; do
        _fm_hit=0
        for _fm_c in /proc/[0-9]*/cgroup; do
            _fm_p=${_fm_c#/proc/}
            _fm_p=${_fm_p%/cgroup}
            [ "$_fm_p" = "$$" ] && continue
            # ⚠ 一个进程一个进程地 fork grep 太慢 (本机 500 进程 × 8 unit = 4000 次 fork,
            #   实测一次模式切换 4 s; 板上 1 核更糟)。改成纯 shell 内建:
            #   读 cgroup 那一行 -> 取最后一段叶子名 -> 和 unit 名**全等**比较。
            #   这样也天然避开子串误伤 (k230-ui vs k230-ui-backend: 叶子名不同, 不会误杀后端)。
            while IFS= read -r _fm_line; do
                case "$_fm_line" in
                    */*.service) ;;
                    *) continue ;;
                esac
                _fm_leaf=${_fm_line##*/}
                _fm_leaf=${_fm_leaf%.service}
                for _fm_u in $_fm_units; do
                    if [ "$_fm_leaf" = "$_fm_u" ]; then
                        kill "$_fm_p" 2>/dev/null && _fm_hit=1
                        break
                    fi
                done
            done <"$_fm_c" 2>/dev/null   # 进程可能在扫描途中退出, 别刷 "cannot open /proc/N/cgroup"
        done
        [ "$_fm_hit" = 0 ] && break
        _fm_i=$((_fm_i + 1))
        sleep 0.3
    done

    # 按 cgroup 杀掉 = systemd 记成 "failed"; 清掉, 免得 is-active 返回 failed / 屏幕上一片红
    for _fm_u in $_fm_units; do
        systemctl reset-failed "$_fm_u" >>"$LOG" 2>&1 || true
    done
    return 0
}

fast_off() { fast_off_many "$1"; }

# ⚠ POSIX sh 的函数**没有局部变量**: 每个函数都用自己独立前缀的变量名。
#   2026-11-03 踩过: select miracast 实际起了 carplay —— 因为 stop_service 复用
#   了 _units/_svc, 循环最后一轮是 carplay, 把外层要用的值覆盖了。
# 某一路 service 对应的 unit 列表 (输出到 stdout)
units_of() {
    _uo_svc=$1
    eval "printf '%s' \"\${UNITS_$_uo_svc:-}\""
}

# 所有投屏端的 unit 列表 (输出到 stdout)
all_unit_list() {
    _au_out=""
    for _au_s in $ALL_SERVICES; do
        _au_u=$(units_of "$_au_s")
        [ -n "${_au_u:-}" ] && _au_out="$_au_out $_au_u"
    done
    printf '%s' "$_au_out"
}

# 停某一路投屏 (unit 名不一定等于 service 名, 所以按 service 反查)
stop_service() {
    _ss_svc=$1
    _ss_units=$(units_of "$_ss_svc")
    [ -n "${_ss_units:-}" ] || return 0
    fast_off_many $_ss_units
}

stop_all_services() {
    _sas_u=$(all_unit_list)
    [ -n "${_sas_u:-}" ] && fast_off_many $_sas_u
}

# ---------------------------------------------------------------- 前置检查
need_ui_binary() {
    [ -x "$UI_BIN" ] && return 0
    log "✗ $UI_BIN 不存在, 无法进入 ui 模式"
    say "ERROR: $UI_BIN missing"
    return 1
}

# ---------------------------------------------------------------- 会话探针
conn_cnt() {
    ss -tn state established 2>/dev/null | awk -v list="$*" '
        BEGIN { n = split(list, a, " "); for (i = 1; i <= n; i++) want[a[i]] = 1 }
        NR > 1 { for (i = 1; i <= NF; i++) { k = $i; sub(/.*:/, "", k); if (k in want) { c++; break } } }
        END { print c + 0 }'
}

probe_miracast() { [ "$(conn_cnt 7236)" -gt 0 ] || iw dev wlan0 link 2>/dev/null | grep -qi 'connected to'; }
probe_airplay()  { [ "$(conn_cnt 7100 7101)" -gt 0 ]; }
probe_carplay()  { [ -f /run/airplay-active ] || [ "$(conn_cnt 7000)" -gt 0 ]; }
probe_carlife()  { [ "$(conn_cnt 7240 8240 9240 9241 9242 9340 9440)" -gt 0 ]; }

# ---------------------------------------------------------------- 模式切换
mode_ui() {
    need_ui_binary || return 1
    # 顺序很重要: 先让别人放开 DRM, 再起 UI。
    # 一次性把「仲裁器 + 四路投屏 + 屏 + 旧界面」全部下发 stop, 再一轮杀干净。
    fast_off_many "$ARBITER_UNIT $(all_unit_list) $SCREEN_UNIT $UI_UNIT"

    # ⚠ 手工跑过的 k230ui 不在任何 unit 的 cgroup 里, 上面杀不到 ——
    #   它若还活着就仍是 DRM master, 新起的这个只会静默拿不到 master (不报错、不画面)。
    pkill -x k230ui >>"$LOG" 2>&1 || true
    sleep 0.4                      # 给 drmplay/k230ui 一点时间 release drm master
    ensure_on "$UI_UNIT" || return 1
    echo "ui" >"$STATE"
    log "✅ 模式 = ui (主界面占屏)"
    say "OK mode=ui"
    return 0
}

mode_select() {
    _ms_svc=$1
    _ms_units=$(units_of "$_ms_svc")
    if [ -z "${_ms_units:-}" ]; then
        log "✗ 未知 service: $_ms_svc"
        say "ERROR: unknown service $_ms_svc"
        return 2
    fi

    # 要停的 = 主界面 + 仲裁器 + 其它三路 (手动模式 = 严格互斥)
    _ms_stop="$UI_UNIT $ARBITER_UNIT"
    for _ms_s in $ALL_SERVICES; do
        [ "$_ms_s" = "$_ms_svc" ] && continue
        _ms_u=$(units_of "$_ms_s")
        [ -n "${_ms_u:-}" ] && _ms_stop="$_ms_stop $_ms_u"
    done

    # 1) 先让 UI 放开 DRM (它拿着 master, 屏不放开 k230-screen 起不来画面)
    #    同时把仲裁器和其它路一起停掉 —— 一次下发, 一轮杀完
    fast_off_many $_ms_stop
    pkill -x k230ui >>"$LOG" 2>&1 || true

    # 2) 屏幕所有者接管 DRM
    ensure_on "$SCREEN_UNIT" || { log "⚠ $SCREEN_UNIT 起不来, 退回 ui"; mode_ui; return 1; }

    # 3) 起目标服务
    for _ms_u in $_ms_units; do
        ensure_on "$_ms_u" || { log "⚠ 起 $_ms_u 失败"; say "ERROR: start $_ms_u failed"; return 1; }
    done

    # 4) 收尾复查屏的所有者
    #    别的路的 ExecStop (wfd-p2p.sh stop) 会顺带 `systemctl restart k230-screen`,
    #    这波浪可能正好落在我们刚起的 screen 之后 ⇒ 有个 1~2 s 的空窗 (屏上无 plane)。
    #    这里再确认一次, 保证"模式切完 = 屏上确实有 drmplay"。
    sleep 1
    if ! unit_on "$SCREEN_UNIT" || { ! pgrep -x ai2dplay >/dev/null 2>&1 && ! pgrep -x drmplay >/dev/null 2>&1; }; then
        log "⚠ 复查: k230-screen 被别的 ExecStop 掀了, 重起一次"
        ensure_on "$SCREEN_UNIT" || log "⚠ k230-screen 重起失败"
    fi

    echo "$_ms_svc" >"$STATE"
    log "✅ 模式 = $_ms_svc (投屏占屏, 已停其余路)"
    say "OK mode=$_ms_svc"
    return 0
}

mode_auto() {
    fast_off_many "$UI_UNIT $(all_unit_list)"
    pkill -x k230ui >>"$LOG" 2>&1 || true
    ensure_on "$SCREEN_UNIT" || log "⚠ $SCREEN_UNIT 起不来"
    ensure_on "$ARBITER_UNIT" || log "⚠ $ARBITER_UNIT 起不来"
    echo "auto" >"$STATE"
    log "✅ 模式 = auto (交还仲裁器)"
    say "OK mode=auto"
    return 0
}

do_probe() {
    _dp_svc=$1
    case "$_dp_svc" in
        miracast|airplay|carlife|carplay) "probe_$_dp_svc" ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------- 入口
cmd=${1:-status}
case "$cmd" in
    ui)     mode_ui ;;
    select) [ $# -ge 2 ] || { say "ERROR: select needs a service"; exit 1; }
            mode_select "$2" ;;
    stop)   mode_ui ;;
    auto)   mode_auto ;;
    probe)  [ $# -ge 2 ] || exit 1
            do_probe "$2" ;;
    status) [ -f "$STATE" ] && cat "$STATE" || echo "unknown" ;;
    *)
        say "usage: $0 {ui|select <svc>|stop|auto|probe <svc>|status}"
        exit 1
        ;;
esac
