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
#   ui             主界面占屏: 屏/所有投屏端/仲裁器/fifo keeper 全停, k230-ui 起
#   wait:<svc>     待投屏: 该路服务起, **屏仍归 k230-ui (LVGL 二级画布)**, 不刷黑
#   video:<svc>    投屏中: k230-ui 停, k230-screen (drmplay) 起, 刷视频流
#   auto           交还开机仲裁器: 所有投屏端放回待命 + k230-screen 起 + 仲裁器起
#
# 2026-11-07 改版 (主人要求):
#   点主界面按钮 -> SELECT -> wait 模式 (服务起但屏不让, 二级画布一直在, 不刷黑);
#   后端探到真实视频会话 -> video 模式 (才把屏交给 drmplay);
#   视频流断开 -> 回 wait 模式 (k230-ui 重启, 靠 active-page 自动回到刚才那个按钮的二级页);
#   只有「返回主界面」(STOP) 才回 ui 模式。
#
# 用法:
#   k230-ui-mode.sh ui                  # 让主界面占屏 (空闲态)
#   k230-ui-mode.sh wait <svc>          # 起服务但保留 LVGL 二级画布 (待投屏)
#   k230-ui-mode.sh video <svc>         # 真正出帧: 屏交给 drmplay
#   k230-ui-mode.sh stop                # 停会话, 回主界面
#   k230-ui-mode.sh auto                # 停会话, 交给 k230-source-arbiter
#   k230-ui-mode.sh select <svc>        # (兼容旧调用) 直接让投屏占屏
#   k230-ui-mode.sh probe <service>     # 会话还在吗 (exit 0 = 在)
#   k230-ui-mode.sh status              # 打印当前模式
#
# 退出码: 0 = 成功, 1 = 参数错/失败, 2 = 未知 service
set -u

STATE=${K230UI_STATE:-/run/k230-ui-mode.state}
LOG=${K230UI_LOG:-/var/log/k230-ui.log}
CONF=${K230UI_CONF:-/etc/k230-ui.conf}

# 本次 UI 会话的运行态文件 (/run, 重启即失)
UI_RUN_DIR=${K230UI_RUN_DIR:-/run/k230-ui}
HOLD_FILE="$UI_RUN_DIR/hold-screen"          # 服务脚本见此标记 => 绝不碰 DRM/k230-screen
ACTIVE_PAGE="$UI_RUN_DIR/active-page"        # k230ui 重启后靠它回到刚才那个按钮的二级页
KEEPER_PID="$UI_RUN_DIR/fifo-keeper.pid"
FIFO=${WFD_PLAYER_FIFO:-/run/wfd-player.fifo}

# 各模式用到的 systemd 单元 (可在 /etc/k230-ui.conf 覆盖)
UI_BIN=${UI_BIN:-/usr/bin/k230ui}   # 主界面可执行文件 (本机联调可指到 build-sim/)
MODE_SWITCH=${MODE_SWITCH:-/usr/bin/k230-mode.sh}   # WiFi/BT 联动切换 (四模式)
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

# ---- 蓝牙策略 (2026-11-05 主人要求: 蓝牙跟着 UI 走, 不常开)
#   主界面 / Miracast / CarLife  = 蓝牙**关** (看不到设备, 也不会被误连)
#   AirPlay                      = 蓝牙**开** + 名字 K230-CAST + 起反控服务 k230-bt-hid
#   CarPlay                      = 蓝牙**开** 但只做车载 (名字可配), 不起 HID 反控
BT_UNIT=${BT_UNIT:-k230-bt-hid}
BT_DEV=${BT_DEV:-hci0}
BT_ON_MODES=${BT_ON_MODES:-"airplay carplay"}
BT_NAME_airplay=${BT_NAME_airplay:-K230-CAST}
BT_NAME_carplay=${BT_NAME_carplay:-K230-CARPLAY}

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

# ---------------------------------------------------------------- FIFO 读者 (待投屏)
# 为什么必须有: 投屏服务的解码器 (v4l2_dec) 一启动就 open(/run/wfd-player.fifo, O_WRONLY)。
#   wait 模式下 k230-screen (drmplay) 是停的 ⇒ 管道没有读者 ⇒ 解码器开局卡在
#   "最多等 10 s" 重试、超时退出, 然后 wfd-sink 被 SIGPIPE 带死、管线整条重起 ——
#   手机上正在建的 RTSP/P2P 会话会被这一下打断 (表现就是「好几次才行」)。
#   所以待投屏期间由一个纯 shell 的 keeper 常驻读管道并丢弃, 给解码器当读者,
#   **完全不碰 DRM** (drmplay 才是唯一的屏所有者, 此时不起它)。
# 切 video 时先起 k230-screen (drmplay 也成为读者)、再杀 keeper, 全程至少有一个读者,
#   解码器拿着的写 fd 不会因为 unlink 重建或 SIGPIPE 断掉。
fifo_keeper_start() {
    mkdir -p "$UI_RUN_DIR"
    if [ ! -p "$FIFO" ]; then
        rm -f "$FIFO"
        mkfifo "$FIFO" 2>/dev/null || true
        chmod 666 "$FIFO" 2>/dev/null || true
    fi
    if [ -f "$KEEPER_PID" ] && \
       kill -0 "$(cat "$KEEPER_PID" 2>/dev/null)" 2>/dev/null; then
        return 0
    fi
    # setsid 让 keeper 自成进程组, 停的时候整组一起收 (里面的 cat 可能正阻塞在 open)
    setsid sh -c 'while :; do cat "$1" >/dev/null 2>&1; sleep 0.05; done' \
        K230UI_FIFO_KEEPER "$FIFO" >/dev/null 2>&1 &
    echo $! > "$KEEPER_PID"
    log "🟢 fifo keeper pid=$(cat "$KEEPER_PID") (待投屏期间给解码器当读者, 不占 DRM)"
}

fifo_keeper_stop() {
    _fk_pid=$(cat "$KEEPER_PID" 2>/dev/null || echo 0)
    if [ -n "$_fk_pid" ] && [ "$_fk_pid" != 0 ]; then
        # 负 pid = 整个进程组 (setsid 后 $! 就是组长)
        kill -TERM "-$_fk_pid" 2>/dev/null || kill -TERM "$_fk_pid" 2>/dev/null || true
        # 兜底: 杀掉直接子进程 (cat/sleep), 再补一刀主进程
        for _fk_c in /proc/[0-9]*/stat; do
            _fk_p=${_fk_c#/proc/}; _fk_p=${_fk_p%/stat}
            _fk_ppid=$(awk '{print $4}' "$_fk_c" 2>/dev/null) || continue
            [ "$_fk_ppid" = "$_fk_pid" ] && kill -9 "$_fk_p" 2>/dev/null || true
        done
        kill -9 "$_fk_pid" 2>/dev/null || true
    fi
    rm -f "$KEEPER_PID"
}


# ---------------------------------------------------------------- 蓝牙开关
# 为什么要显式 hciconfig up/down: 只停 k230-bt-hid 的话适配器仍在广播,
# 手机上照样能看到设备 (主人要的是"主界面时蓝牙不开启")。
bt_power_on() {
    unit_on bluetooth || {
        log "▶ 起 bluetooth.service"
        systemctl start bluetooth >>"$LOG" 2>&1
        sleep 1
    }
    if command -v hciconfig >/dev/null 2>&1; then
        # ★ 根因 4 配套: BlueZ 会把 Discoverable 持久化到
        #   /var/lib/bluetooth/<bd>/settings, **上电时自动恢复 true** —— 所以"上电→
        #   我们再关掉"之间总有几秒窗口。AirPlay (hold) 场景先把持久值改成 false。
        if [ "${_bt_hold_disc:-0}" = 1 ]; then
            _bp_bd=$(hciconfig "$BT_DEV" 2>/dev/null | awk '/BD Address/{print $3; exit}')
            _bp_cfg="/var/lib/bluetooth/${_bp_bd}/settings"
            if [ -n "${_bp_bd:-}" ] && [ -f "$_bp_cfg" ]; then
                sed -i "s/^Discoverable=true/Discoverable=false/" "$_bp_cfg" 2>/dev/null || true
            fi
        fi
        hciconfig "$BT_DEV" up >>"$LOG" 2>&1 || log "⚠ hciconfig $BT_DEV up 失败"
    else
        bluetoothctl power on >>"$LOG" 2>&1 || true
    fi
    # ★ 2026-11-05 实测踩坑 1: 光 hciconfig up 时 bluetoothctl 里 `Pairable: no`
    #   ⇒ 手机"忽略此设备"之后重新配对会**直接被拒**(表现: 点 K230-CAST 连不上)。
    # ★ 2026-11-05 实测踩坑 2: hci0 刚 down→up 时 bluetoothd 还没把适配器登记好,
    #   这时立刻发 `bluetoothctl remove/pairable` 会回 `org.bluez.Error.NotReady`
    #   (板上实测 14:42:37 就吃了这个错)。所以先等适配器"Powered: yes"再动手。
    if command -v bluetoothctl >/dev/null 2>&1; then
        _bp_i=0
        while [ "$_bp_i" -lt 20 ]; do
            bluetoothctl show 2>/dev/null | grep -q "Powered: yes" && break
            bluetoothctl power on >>"$LOG" 2>&1 || true
            sleep 0.5
            _bp_i=$((_bp_i + 1))
        done
        # ★ 根因 4: hold 场景**一上电就立刻关可发现**, 别等 agent/pairable 那 ~3 s ——
        #   否则手机可能正好在这几秒扫到"看得见但 SDP 里没有 HID"的 K230-CAST。
        if [ "${_bt_hold_disc:-0}" = 1 ]; then
            bluetoothctl --timeout 5 discoverable off >>"$LOG" 2>&1 \
                || bluetoothctl discoverable off >>"$LOG" 2>&1 || true
        fi
        # 配对 agent: 板上默认没有 agent 进程, 手机点 K230-CAST 时 SSP 确认没人应答
        # ⇒ "连不上"。先起 agent 再开 pairable。${BT_AGENT:-} 为空则不启用 (老固件兼容)。
        if [ -n "${BT_AGENT:-}" ]; then
            systemctl start "$BT_AGENT" >>"$LOG" 2>&1 || log "⚠ $BT_AGENT 起不来 (手机配对可能被拒)"
        fi
        bluetoothctl --timeout 5 pairable on >>"$LOG" 2>&1 \
            || bluetoothctl pairable on >>"$LOG" 2>&1 || true
        # ★ 2026-11-05 根因 4: AirPlay 场景要等 HID 就绪才允许"可被发现"。
        #   否则手机看得见 K230-CAST, 但 SDP 里还没有 0x1124 ⇒ iOS 弹「配对不成功」。
        #   (调用方 bt_apply 会置 _bt_hold_disc=1, 等 bt_wait_hid_ready 成功后再开。)
        if [ "${_bt_hold_disc:-0}" != 1 ]; then
            bluetoothctl --timeout 5 discoverable on >>"$LOG" 2>&1 \
                || bluetoothctl discoverable on >>"$LOG" 2>&1 || true
        fi
        # 回读核对: 设置失败必须留证据 (手机连不上时一眼能看出是"不可配对")
        bluetoothctl show 2>/dev/null | grep -q "Pairable: yes" \
            || log "⚠ 适配器仍不可配对 (Pairable: no) — 手机重新配对会被拒"
    fi
    # 板上有个 k230-bt-hid.path (PathExists=/sys/class/bluetooth/hci0) 会自动拉起服务;
    # 关蓝牙时它被停掉, 这里要重新武装, 否则服务起不来。
    systemctl start "$BT_UNIT.path" >>"$LOG" 2>&1 || true
}

bt_power_off() {
    # ⚠ 必须连 .path 一起停: 只停 service 会被 path 触发器立刻拉起来 (实测 stop 后仍 active)。
    systemctl stop "$BT_UNIT.path" >>"$LOG" 2>&1 || true
    systemctl stop "$BT_UNIT" >>"$LOG" 2>&1 || true
    [ -n "${BT_AGENT:-}" ] && systemctl stop "$BT_AGENT" >>"$LOG" 2>&1
    if command -v hciconfig >/dev/null 2>&1; then
        hciconfig "$BT_DEV" down >>"$LOG" 2>&1 || true
    else
        bluetoothctl power off >>"$LOG" 2>&1 || true
    fi
    _bp_i=0
    while [ "$_bp_i" -lt 6 ] && unit_on "$BT_UNIT"; do
        sleep 0.5; _bp_i=$((_bp_i + 1))
    done
    unit_on "$BT_UNIT" && log "⚠ $BT_UNIT 仍在运行 (蓝牙关不彻底)"
}

# 等 HID 反控服务真的 active (最多 ~20 s), 期间失败就重试。
# 为什么不能只 systemctl start: hci0 刚 up 时 bluetoothd 还没把 SDP/profile 注册好,
# k230-bt-hid 会以退出码 3 退出, 由 Restart=on-failure (5 s) 重试; 只 start 一次就报
# 失败是误判 (2026-11-05 实测: 13:55:29 起, 自测只看 3 s 就以为失败)。
bt_wait_unit() {
    _bw_i=0
    while [ "$_bw_i" -lt 40 ]; do
        unit_on "$BT_UNIT" && return 0
        systemctl start "$BT_UNIT" >>"$LOG" 2>&1 || true
        sleep 0.5
        _bw_i=$((_bw_i + 1))
    done
    unit_on "$BT_UNIT"
}

# 等 HID profile **真的**注册完 (看 /run/k230-bt-hid.status 的 `注册=是`), 最多 ~30 s。
# ★ 2026-11-05 根因 4: 只有到这一步才允许 `discoverable on` —— 否则手机看得见设备、
#   点下去 SDP 里没有 0x1124, iOS 就弹「配对不成功」(重启后实测空窗 2 分 7 秒)。
bt_wait_hid_ready() {
    _bh_i=0
    while [ "$_bh_i" -lt 60 ]; do
        if grep -q "注册=是" /run/k230-bt-hid.status 2>/dev/null; then
            if command -v bluetoothctl >/dev/null 2>&1; then
                bluetoothctl --timeout 5 discoverable on >>"$LOG" 2>&1 \
                    || bluetoothctl discoverable on >>"$LOG" 2>&1 || true
            fi
            log "✓ HID 已注册 ⇒ 现在才开放可被发现 (手机此刻配 K230-CAST 才成功)"
            return 0
        fi
        sleep 0.5
        _bh_i=$((_bh_i + 1))
    done
    return 1
}

# 按当前模式套蓝牙策略 (唯一的入口: 各 mode_* 结束时调一次)
bt_apply() {
    _bt_m=$1
    _bt_want=0
    for _bt_s in $BT_ON_MODES; do
        [ "$_bt_s" = "$_bt_m" ] && _bt_want=1 && break
    done

    if [ "$_bt_want" = 0 ]; then
        bt_power_off
        log "🔵 蓝牙策略: 模式=$_bt_m ⇒ 关"
        return 0
    fi

    # ★ 2026-11-05 根因 4: AirPlay 先保持"不可被发现", 等 HID 注册完再开。
    #   顺手删掉上一轮遗留的状态文件 —— 否则 bt_wait_hid_ready 会读到旧的 `注册=是`
    #   而提前开可被发现 (板上实测踩到过)。
    if [ "$_bt_m" = "airplay" ]; then
        _bt_hold_disc=1
        rm -f /run/k230-bt-hid.status
    else
        _bt_hold_disc=0
    fi
    bt_power_on
    _bt_hold_disc=0
    # 只 AirPlay 需要 HID 反控 (手机当鼠标用); CarPlay 交给车载那套
    if [ "$_bt_m" = "airplay" ]; then
        if bt_wait_unit; then
            bt_wait_hid_ready \
                || log "⚠ HID 未就绪 ⇒ 保持不可被发现 (宁可搜不到, 也不让手机白点一次「配对不成功」)"
        else
            log "⚠ $BT_UNIT 起不来 (反控不可用)"
        fi
    else
        # 非 AirPlay 的模式: 不需要 HID 反控, 服务停掉但蓝牙保持开
        systemctl stop "$BT_UNIT.path" >>"$LOG" 2>&1 || true
        systemctl stop "$BT_UNIT" >>"$LOG" 2>&1 || true
    fi
    eval "_bt_name=\${BT_NAME_$_bt_m:-}"
    if [ -n "${_bt_name:-}" ] && command -v bluetoothctl >/dev/null 2>&1; then
        bluetoothctl system-alias "$_bt_name" >>"$LOG" 2>&1 || true
    fi
    # ★ 2026-11-07 CarPlay: 自栈 carplay-wireless 的 SDP server 要独占 L2CAP PSM 1,
    #   系统 bluetoothd 占着时它报 "SDP server exited: Address already in use (os error 98)"
    #   ⇒ 手机查不到 'Wireless iAPv2' 服务, 连上蓝牙也不提示「车载设备」。
    #   让 bluetoothd 让位; mode_wait 是「先起服务再调 bt_apply」, wireless 此时已在跑,
    #   必须重启一次让 SDP server 重新绑定 PSM 1 (实验: 重启后 serving handle 0x00010000)。
    #   回 ui / airplay 时 bt_power_on() 会按需重新 start bluetooth, 不影响 AirPlay 反控。
    if [ "$_bt_m" = "carplay" ]; then
        systemctl stop bluetooth.service >>"$LOG" 2>&1 || true
        unit_on carplay-wireless && systemctl restart carplay-wireless >>"$LOG" 2>&1 || true
        log "🔵 CarPlay: 已停 bluetoothd 让出 PSM 1, SDP 交 carplay-wireless 自栈"
    fi
    log "🔵 蓝牙策略: 模式=$_bt_m ⇒ 开 (名字=${_bt_name:-默认})"
    return 0
}

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
    # ⚠ 必须**先**清掉页面记忆: 下面 ensure_on "$UI_UNIT" 会拉起新的 k230ui 进程,
    #   它的 app_init 读这个文件决定显示主界面还是二级画布。回主界面 = 必须忘掉。
    rm -f "$ACTIVE_PAGE" "$HOLD_FILE"
    fifo_keeper_stop
    # 顺序很重要: 先让别人放开 DRM, 再起 UI。
    # 一次性把「仲裁器 + 四路投屏 + 屏 + 旧界面」全部下发 stop, 再一轮杀干净。
    fast_off_many "$ARBITER_UNIT $(all_unit_list) $SCREEN_UNIT $UI_UNIT"

    # ⚠ 手工跑过的 k230ui 不在任何 unit 的 cgroup 里, 上面杀不到 ——
    #   它若还活着就仍是 DRM master, 新起的这个只会静默拿不到 master (不报错、不画面)。
    pkill -x k230ui >>"$LOG" 2>&1 || true
    # 回主界面 = 无线模式已退出; 清运行态, 否则下次点同一模式 k230-mode.sh 会误判
    # "已经是 <mode>" 而不做 AP/P2P + 蓝牙联动.
    rm -f /run/k230-mode.state
    sleep 0.4                      # 给 drmplay/k230ui 一点时间 release drm master
    ensure_on "$UI_UNIT" || return 1
    bt_apply ui
    echo "ui" >"$STATE"
    log "✅ 模式 = ui (主界面占屏)"
    say "OK mode=ui"
    return 0
}

# wait <svc>: 起该路服务, 但**屏仍归 k230-ui** (LVGL 二级画布), 不刷黑。
# 关键: 不停 $UI_UNIT; 用 fifo keeper 顶替 drmplay 当管道读者; 置 hold-screen
#       让投屏服务脚本不去抢 DRM master / 不去拉起 k230-screen。
mode_wait() {
    _mw_svc=$1
    _mw_units=$(units_of "$_mw_svc")
    if [ -z "${_mw_units:-}" ]; then
        log "✗ 未知 service: $_mw_svc"
        say "ERROR: unknown service $_mw_svc"
        return 2
    fi

    _mw_prev=$(cat "$STATE" 2>/dev/null || echo "")
    mkdir -p "$UI_RUN_DIR"
    echo "$_mw_svc" >"$ACTIVE_PAGE"     # k230ui 重启后回到这个按钮的二级页
    : >"$HOLD_FILE"

    if [ "$_mw_prev" = "wait:$_mw_svc" ] || [ "$_mw_prev" = "video:$_mw_svc" ]; then
        # 快路径: 视频流断开回到二级页 / 点了「重新连接」。服务本身还活着,
        # 蓝牙状态也没变 ⇒ 不重跑 bt_apply (AirPlay 那套会等 30 s HID 注册)。
        fast_off_many "$SCREEN_UNIT"
        fifo_keeper_start
        ensure_on "$UI_UNIT" || return 1
        for _mw_u in $_mw_units; do ensure_on "$_mw_u" || true; done
        echo "wait:$_mw_svc" >"$STATE"
        log "✅ 模式 = wait:$_mw_svc (回到二级画布, 服务继续待命)"
        say "OK mode=wait:$_mw_svc"
        return 0
    fi

    # 首次进入 / 换了一路: 先做 WiFi 射频域 + 蓝牙形态联动 (重置/起目标),
    # 再按 UI 的屏幕逻辑停其它三路。k230-mode.sh 会自检, 失败只告警不阻断 UI。
    _mw_mode_ok=0
    if [ -x "$MODE_SWITCH" ]; then
        log "🔗 WiFi/BT 联动: $MODE_SWITCH $_mw_svc"
        if sh "$MODE_SWITCH" "$_mw_svc" >>"$LOG" 2>&1; then
            _mw_mode_ok=1
        else
            log "⚠ k230-mode.sh $_mw_svc 未过门禁, 回退 bt_apply"
        fi
    fi

    # 首次进入 / 换了一路: 停其它三路 + 仲裁器 + 屏, 但**保住 k230-ui**。
    _mw_stop="$SCREEN_UNIT $ARBITER_UNIT"
    for _mw_s in $ALL_SERVICES; do
        [ "$_mw_s" = "$_mw_svc" ] && continue
        _mw_u=$(units_of "$_mw_s")
        [ -n "${_mw_u:-}" ] && _mw_stop="$_mw_stop $_mw_u"
    done
    fast_off_many $_mw_stop
    fifo_keeper_start
    ensure_on "$UI_UNIT" || return 1
    for _mw_u in $_mw_units; do
        ensure_on "$_mw_u" || { log "⚠ 起 $_mw_u 失败"; say "ERROR: start $_mw_u failed"; return 1; }
    done
    if [ "$_mw_mode_ok" = 1 ]; then
        log "🔵 蓝牙策略: 已由 k230-mode.sh 处理, 跳过 bt_apply"
    else
        bt_apply "$_mw_svc"
    fi
    echo "wait:$_mw_svc" >"$STATE"
    log "✅ 模式 = wait:$_mw_svc (服务后台等投屏, LVGL 二级画布占屏, 不刷黑)"
    say "OK mode=wait:$_mw_svc"
    return 0
}

# video <svc>: 真实投屏会话已建立, 才把屏从 LVGL 交给 drmplay 刷视频流。
# 顺序要点: 先起 k230-screen (drmplay 成为 fifo 读者)、**后**杀 keeper,
#   这样解码器拿着的写 fd 全程有读者, 不会因 unlink 重建 / SIGPIPE 断掉。
mode_video() {
    _mv_svc=$1
    _mv_units=$(units_of "$_mv_svc")
    if [ -z "${_mv_units:-}" ]; then
        log "✗ 未知 service: $_mv_svc"
        say "ERROR: unknown service $_mv_svc"
        return 2
    fi

    # 1) 停 UI, 放开 DRM master
    fast_off_many "$UI_UNIT"
    pkill -x k230ui >>"$LOG" 2>&1 || true
    sleep 0.4

    # 2) 屏所有者接管 (k230-ui.service 的 Conflicts 也会顺带停 UI)
    ensure_on "$SCREEN_UNIT" || {
        log "⚠ $SCREEN_UNIT 起不来, 退回 wait (保留二级画布)"
        mode_wait "$_mv_svc"
        return 1
    }

    # 3) drmplay 已在读 => 现在才收 keeper, 之后只剩一个读者
    fifo_keeper_stop

    # 4) 服务是 wait 阶段起的, 这里只兜底确认
    for _mv_u in $_mv_units; do ensure_on "$_mv_u" || true; done

    # 蓝牙状态与 wait 阶段一致, 不重跑 bt_apply (避免切屏时卡 30 s)
    echo "video:$_mv_svc" >"$STATE"
    log "✅ 模式 = video:$_mv_svc (真实投屏, drmplay 刷视频流)"
    say "OK mode=video:$_mv_svc"
    return 0
}

# 兼容老的「select <svc> 直接占屏」调用 (调试/手工用); 后端已改用 wait + video。
# 先清 hold 标记, 让投屏服务脚本恢复自己管播放器, 否则会没人上屏。
mode_select() {
    _ms_svc=$1
    _ms_units=$(units_of "$_ms_svc")
    if [ -z "${_ms_units:-}" ]; then
        log "✗ 未知 service: $_ms_svc"
        say "ERROR: unknown service $_ms_svc"
        return 2
    fi
    rm -f "$HOLD_FILE"
    fifo_keeper_stop
    mkdir -p "$UI_RUN_DIR"
    echo "$_ms_svc" >"$ACTIVE_PAGE"

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

    bt_apply "$_ms_svc"
    echo "video:$_ms_svc" >"$STATE"
    log "✅ 模式 = video:$_ms_svc (旧 select 兼容路径, 投屏占屏)"
    say "OK mode=video:$_ms_svc"
    return 0
}

mode_auto() {
    rm -f "$ACTIVE_PAGE" "$HOLD_FILE"
    fifo_keeper_stop
    fast_off_many "$UI_UNIT $(all_unit_list)"
    pkill -x k230ui >>"$LOG" 2>&1 || true
    ensure_on "$SCREEN_UNIT" || log "⚠ $SCREEN_UNIT 起不来"
    ensure_on "$ARBITER_UNIT" || log "⚠ $ARBITER_UNIT 起不来"
    bt_apply ui
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
    wait)   [ $# -ge 2 ] || { say "ERROR: wait needs a service"; exit 1; }
            mode_wait "$2" ;;
    video)  [ $# -ge 2 ] || { say "ERROR: video needs a service"; exit 1; }
            mode_video "$2" ;;
    select) [ $# -ge 2 ] || { say "ERROR: select needs a service"; exit 1; }
            mode_select "$2" ;;
    stop)   mode_ui ;;
    auto)   mode_auto ;;
    probe)  [ $# -ge 2 ] || exit 1
            do_probe "$2" ;;
    status) [ -f "$STATE" ] && cat "$STATE" || echo "unknown" ;;
    *)
        say "usage: $0 {ui|wait <svc>|video <svc>|select <svc>|stop|auto|probe <svc>|status}"
        exit 1
        ;;
esac
