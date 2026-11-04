#!/bin/sh
# K230 Miracast sink — P2P 事件 watcher v5
#   形态: 手机当 GO 建组 → K230 当普通 STA 用 WPS PBC 关联进去 → 起 RTSP 客户端拉流
#
# ============================ 为什么是这个逻辑 ============================
# 全部来自 2026-10-21 真机 + 空口抓包实证 (证据见 docs/CAPTURES.md / docs/WORKLOG.md),
# 不是猜测:
#
# 1) 手机 (REDMI K80 Ultra) 作 WFD source 时**只发 Provision Discovery (PBC)**。
#    我们的 PD 响应里若带 P2P IE 的 Group ID / Operating Channel, 手机就停在原地:
#    30 s 后重发一次 PD, 全程 0 帧 Auth/Assoc、0 帧 GO-Neg、也不建自己的组
#    (抓包 20261021-2036-k230-phone-go-neg.pcap: 42k 帧里手机只有 5 个 Action 帧)。
#    而参考 sink (海奇) 的 PD 响应**完全不含 P2P IE**, 手机随后就发 GO-Neg, 并把
#    GO-Neg 拒掉之后**自己建组当 GO**(SSID `DIRECT-<2随机>-REDMI K80 Ultra`)。
#    ⇒ 结论: 回退 wpa_supplicant 的 PD Group-ID 补丁, 保持 stock 行为 (不发 P2P IE)。
#      本脚本不再做任何 "主动 p2p_connect / p2p_invite" 的敲门动作 (实测全部无人应答)。
#
# 2) 手机组的 BSSID = 手机 P2P 设备地址的**前 5 字节** + 随机末字节:
#       设备地址 da:85:76:6f:f8:2d → 组 BSSID da:85:76:6f:f8:99 / da:85:76:6f:f8:cf
#    信道跟着手机走 (实测 ch6 见过、ch1 也见过), 所以**必须多信道扫描**,
#    不能像以前那样死等 ch6。板端在 GO 状态下扫描是可行的 (实测能扫到)。
#
# 3) 成功路径 (抓包 20261021-2100-hichip-full-success.pcap):
#        t=56.05 sink 回 Invitation(no common channels)  →  手机不理会
#        t=57.6  手机做 IPv6 DAD   →  t=58.08 手机开始回 probe response (已是 AP)
#        t=58.62 sink 直接 Authentication + Association (SSID DIRECT-...REDMI...)
#        t=60.81 WPS PBC 4-way 完成
#    ⇒ sink 只需要: 扫描找到手机那个组 → 用自己的 STA 身份 WPS PBC 关联进去。
#      不需要 GO-Neg 成功, 不需要 P2P Invitation, 也不需要知道组密码 (PBC 现场下发)。
#
# 4) sink 必须**释放自己的 GO 组**再入组: 参考 sink 在成功投屏期间 ch6 上已无它的
#    P2P beacon (抓包 20261021-2135-hichip-full-clean2.pcap) ⇒ 单射频设备不能同时当 GO。
#
# 5) ⚠ PBC 必须**常驻就绪**: wps_pbc 只有 120 s 有效, 过期后 beacon/probe-resp 里
#    `Selected Registrar / Device Password ID` 消失, 手机点选我们时会当成"没有按按钮的
#    设备"而跳过 (见 2026-10-21 抓包 20261021-1910 / 1919)。所以每 60 s 续一次。
#
# 用法: wfd-pbc-watcher.sh [日志文件]
# 依赖: 板端 wpa_supplicant (LD_LIBRARY_PATH=/root/wpalib), /tmp/wfd-p2p.log
set -u
export LD_LIBRARY_PATH=/root/wpalib

IFACE=wlan1
P2P_LOG=/tmp/wfd-p2p.log
GO_FREQ=${WFD_GO_FREQ:-2437}   # 我们自己 GO 组的工作频点 (默认 ch6), 由 wfd-p2p.sh 透传
JOIN_DELAY=3            # 收到 PD 后等多久开始找手机组 (留给手机发 GO-Neg 并被拒)
SCAN_GO_WAIT=10         # 先在自己还当 GO 的情况下扫多久
SCAN_FREE_WAIT=40       # 释放 GO 组后再扫多久
CONNECT_WAIT=25         # WPS PBC 关联最长等多久
KEEP_PBC=60             # PBC 续期周期
LOCK=/tmp/wfd-join.lock
MAXLOCK=60
SINK_PIPE_PID=/tmp/wfd-sink-client.pid

wpa()  { wpa_cli -p /run/wpa_supplicant -i "$IFACE" "$@" 2>&1; }
wpq()  { wpa_cli -p /run/wpa_supplicant -i "$IFACE" "$@" >/dev/null 2>&1; }
log()  { printf "[%s] [wfd-join] %s\n" "$(date +%H:%M:%S)" "$*"; }
is_go() { iw dev "$IFACE" info 2>/dev/null | grep -q "type P2P-GO"; }
is_connected() { wpa status 2>/dev/null | grep -q "wpa_state=COMPLETED"; }

# ---------------------------------------------------------------- 扫描找手机组
# 判据: scan_results 里 BSSID 前 5 字节 == 手机设备地址前 5 字节, 且 SSID 以 DIRECT- 开头
scan_once() {
    _p="$1"
    wpq scan
    sleep 3
    wpa scan_results | awk -F'\t' -v p="$_p" '
        index($1, p ":") == 1 && $5 ~ /^DIRECT-/ { print $1 "\t" $2 "\t" $5; exit }'
}

find_phone_group() {
    _p="$1"; _max="$2"; _t=0
    while [ "$_t" -lt "$_max" ]; do
        _r=$(scan_once "$_p")
        [ -n "$_r" ] && { printf "%s" "$_r"; return 0; }
        _t=$((_t + 3))
    done
    return 1
}

restore_go() {
    log "恢复 GO 组 (freq=$GO_FREQ), 让手机还能在列表里看到我们"
    wpq remove_network all
    wpq p2p_group_add "freq=$GO_FREQ"
    sleep 1
    wpq wps_pbc
    if is_go; then log "✅ GO 组已恢复"; else log "⚠ GO 组没恢复起来, 看 $P2P_LOG"; fi
}

# ------------------------------------------------------- 入组: 普通 STA + WPS PBC
join_phone_group() {
    _peer="$1"
    _p="${_peer%:*}"                      # da:85:76:6f:f8
    log "开始找手机组 (BSSID 前缀 $_p, SSID DIRECT-*)"

    _found=$(find_phone_group "$_p" "$SCAN_GO_WAIT")
    if [ -z "$_found" ]; then
        log "GO 状态下 ${SCAN_GO_WAIT}s 没扫到 → 先释放我们的 GO 组再扫"
        wpq p2p_stop_find
        wpq p2p_group_remove "$IFACE"
        sleep 1
        _found=$(find_phone_group "$_p" "$SCAN_FREE_WAIT")
    fi
    if [ -z "$_found" ]; then
        log "❌ 没找到手机组 (可能手机已放弃本次投屏)"
        restore_go
        rm -f "$LOCK"
        return 1
    fi

    _bssid=$(printf "%s" "$_found" | cut -f1)
    _freq=$(printf "%s" "$_found" | cut -f2)
    _ssid=$(printf "%s" "$_found" | cut -f3)
    log "✅ 找到手机组: '$_ssid' bssid=$_bssid freq=${_freq}MHz"

    # 单射频: 必须先把我们自己的 GO 组拆掉
    if is_go; then
        log "释放我们的 GO 组 (单射频不能同时当 GO)"
        wpq p2p_stop_find
        wpq p2p_group_remove "$IFACE"
        sleep 1
    fi

    wpq remove_network all
    _id=$(wpa add_network | tail -1)
    case "$_id" in
        ''|*[!0-9]*) log "❌ add_network 失败: $_id"; restore_go; rm -f "$LOCK"; return 1 ;;
    esac
    wpq set_network "$_id" ssid "\"$_ssid\""
    wpq set_network "$_id" key_mgmt WPS
    wpq set_network "$_id" bssid "$_bssid"
    wpq set_network "$_id" scan_ssid 1
    log "发起 WPS PBC 关联 (network id=$_id, bssid=$_bssid)"
    _pbc=$(wpa wps_pbc "$_bssid" | tail -1)
    log "wps_pbc → $_pbc"
    wpq select_network "$_id"

    _t=0
    while [ "$_t" -lt "$CONNECT_WAIT" ]; do
        if is_connected; then break; fi
        sleep 1; _t=$((_t + 1))
    done

    if ! is_connected; then
        log "❌ ${CONNECT_WAIT}s 内 WPS PBC 没连上; 现场:"
        wpa status 2>/dev/null | sed 's/^/    /'
        restore_go
        rm -f "$LOCK"
        return 1
    fi

    log "✅ 已关联手机组 (SSID='$_ssid'), 现场:"
    wpa status 2>/dev/null | sed 's/^/    /'

    # 手机 GO 是 192.168.49.1 (DHCP 服务端); 我们用固定 .128 (与参考 sink 一致)
    ip addr flush dev "$IFACE" 2>/dev/null || true
    ip link set dev "$IFACE" up 2>/dev/null || true
    ip addr add 192.168.49.128/24 dev "$IFACE" 2>/dev/null || true
    log "client 地址: $(ip -4 -o addr show "$IFACE" 2>/dev/null | sed -n 's/.*inet \([0-9./]*\).*/\1/p' | head -1)"

    ping -c 2 -W 1 192.168.49.1 >/dev/null 2>&1 \
        && log "✅ 能 ping 通手机 GO 192.168.49.1" \
        || log "⚠ ping 不通 192.168.49.1 (可能手机还没起 RTSP 服务, sink 会自己重试)"

    start_sink_client
    rm -f "$LOCK"
    return 0
}

# ------------------------------------------------ 入组成功后拉起真正的投屏 pipeline
# 手机 = GO = 192.168.49.1 = RTSP 服务端(:7236) = RTP 发送端
# K230 = client = 192.168.49.128 = RTSP 客户端 = 硬解 + DRM 上屏
start_sink_client() {
    _ip="${1:-192.168.49.1}"
    for _b in wfd-sink v4l2_dec drmplay; do
        for _pid in $(pgrep -x "$_b" 2>/dev/null); do kill "$_pid" 2>/dev/null || true; done
    done
    # miracast.service 是"监听模式"pipeline, 会占住解码器/DRM, 必须先停
    systemctl stop miracast.service 2>/dev/null || true
    sleep 1
    setsid sh -c '
        i=0
        while [ $i -lt 30 ]; do
            /usr/bin/wfd-sink -c '"$_ip"':7236 -d /dev/stdout 2>>/var/log/miracast.log | \
            /usr/bin/v4l2_dec /dev/stdin /dev/stdout -H 2>>/var/log/miracast-dec.log | \
            /usr/bin/drmplay -H -p nv12 -w 480 -h 800 -r 30 2>>/var/log/miracast-play.log
            i=$((i+1)); sleep 2
        done' >/dev/null 2>&1 &
    echo $! > "$SINK_PIPE_PID"
    log "▶ sink 客户端已启动: rtsp://${_ip}:7236 → v4l2_dec → drmplay (pid $(cat "$SINK_PIPE_PID" 2>/dev/null))"
}

# --------------------------------------------- v6: 主动邀请手机加入我们的组
# 现象 (2026-10-21 共 6 轮抓包): 手机点选后只发 PD, 我们的 PD 响应被手机 ACK 之后
# 30 s 内**零 GO-Neg / 零 Invitation / 零 probe / 零 Auth**, 然后每 ~30 s 重发一次 PD。
# 我们本身已经是 GO (DIRECT-xxK230), 最可能的语义是: 手机在等 **sink 主动发 P2P
# Invitation** 把组 ID (BSSID+SSID) 和工作信道告诉它。
# 依赖补丁 `wpa_supplicant-2.11-k230-pd-peer-listen-freq.patch`: 收 PD 时把 RX 信道
# 记为对端 listen/oper 信道, 否则 p2p_invite 会报 "Cannot invite unknown P2P Device"。
invite_peer() {   # $1 = 手机 P2P 设备地址
    _p="$1"
    _i=1
    while [ "$_i" -le 3 ]; do
        _r=$(wpa p2p_invite group="$IFACE" peer="$_p" | tail -1)
        log "📤 p2p_invite #$_i group=$IFACE peer=$_p → ${_r:-<空>}"
        sleep 4
        _i=$((_i + 1))
    done
}

wait_sta() {      # $1 = 最长等多少秒; 成功时打印 STA MAC
    _t=0
    while [ "$_t" -lt "$1" ]; do
        _st=$(iw dev "$IFACE" station dump 2>/dev/null | sed -n 's/^Station \([0-9a-f:]*\).*/\1/p' | head -1)
        if [ -n "$_st" ]; then printf "%s" "$_st"; return 0; fi
        sleep 1; _t=$((_t + 1))
    done
    return 1
}

phone_ip_of() {   # $1 = STA MAC; 从 DHCP 日志 / ARP 里取手机 IP
    _m="$1"
    _ip=$(grep -i "$_m" /tmp/dhcpd.log 2>/dev/null | grep -oE '([0-9]{1,3}\.){3}[0-9]{1,3}' | tail -1)
    [ -n "$_ip" ] || _ip=$(ip neigh show dev "$IFACE" 2>/dev/null | grep -i "$_m" | awk '{print $1}' | head -1)
    [ -n "$_ip" ] || _ip=192.168.49.10
    printf "%s" "$_ip"
}

invite_then_join() {
    _peer="$1"
    invite_peer "$_peer"
    _sta=$(wait_sta 20) || _sta=""
    if [ -n "$_sta" ]; then
        _ip=$(phone_ip_of "$_sta")
        log "✅ 手机已作为 STA 加入我们的组: $_sta ip=$_ip"
        sleep 2
        start_sink_client "$_ip"
        rm -f "$LOCK"
        return 0
    fi
    log "ℹ 20s 内手机没进我们的组 → 回退 v5 老路 (找手机自己建的组)"
    join_phone_group "$_peer"
    rm -f "$LOCK"
}

# ------------------------------------------------------------------ PBC 常驻保活
keep_pbc() {
    while :; do
        sleep "$KEEP_PBC"
        is_go || continue
        # 已有 STA 关联 (正在 WPS) 时不碰, 免得打断进行中的会话
        iw dev "$IFACE" station dump 2>/dev/null | grep -q '^Station' && continue
        wpq wps_pbc && log "♻ 已续期 wps_pbc (保持 beacon 里 PBC 就绪)"
    done
}

log "watcher 启动 (v6: 收到 PD 后主动 p2p_invite 邀请手机进我们的组; 20s 无果再回退 v5 找手机组)"
keep_pbc &
KEEP_PID=$!
log "PBC 保活已启动 (每 ${KEEP_PBC}s, pid $KEEP_PID)"

# ------------------------------------------------------------------- 事件循环
tail -n 0 -F "$P2P_LOG" 2>/dev/null | while IFS= read -r line; do
  case "$line" in
    # 手机 (source) 只会发这一种 P2P 动作帧; 此时只回 PBC 就绪 + 记下它, 然后去找它建的组
    *P2P-PROV-DISC-PBC-REQ*)
        _peer=$(printf "%s" "$line" | awk '{print $2}')
        log "📨 PD 请求(PBC): $(printf "%s" "$line" | cut -c1-110)"
        wpq wps_pbc
        # 手机自报的 P2P 状态: listen/oper 信道、能力位、是否把我们当 GO —— 决定它下一步往哪发
        log "🔎 手机 P2P 状态: $(wpa p2p_peer "$_peer" 2>/dev/null | tr '\n' ' ' | cut -c1-360)"
        if [ "${WFD_PD_MODE:-hold}" = "hold" ]; then
            log "⏸ hold: 保持 GO + PBC 不动, 不再拆组/不再 invite (参照海奇: PD 后 ~35s 手机才发 Invitation/GO-Neg)"
            continue
        fi
        # K230 2026-10-22: Win10 实测 —— 对端发完 PD Request 后**什么也不发**
        # (空口 34s 内 0 个 action 帧, 只有 probe-req; 板端日志也 0 个 GO-Neg)。
        # 海奇那套"等对端发 GO-Neg"的被动策略对 Windows 无效, 这里改为主动上门:
        # 只发 P2P Invitation 邀请对端进我们的组, **不拆组、不回退** (纯加法, 失败无副作用)。
        if [ "${WFD_PD_MODE:-hold}" = "invite_only" ]; then
            log "📤 invite_only: 主动邀请对端进我们的组 (不拆组, 不回退)"
            ( _n=0
              while [ "$_n" -lt 12 ]; do
                  _r=$(wpa p2p_invite group="$IFACE" peer="$_peer" 2>&1 | tail -1)
                  log "   p2p_invite group=$IFACE peer=$_peer → ${_r:-<空>}"
                  case "$_r" in *OK*|*FAIL*) : ;; esac
                  _n=$((_n + 1)); sleep 3
              done ) &
            continue
        fi
        if [ -f "$LOCK" ]; then
            _age=$(( $(date +%s) - $(cat "$LOCK" 2>/dev/null || echo 0) ))
            if [ "$_age" -lt "$MAXLOCK" ]; then
                log "   (已有一轮入组流程在跑, ${_age}s 前开始, 本次只回 PBC)"
                continue
            fi
        fi
        date +%s > "$LOCK"
        ( sleep "$JOIN_DELAY"; invite_then_join "$_peer" ) &
        ;;
    # 我们是 GO 时, stock supplicant 会自动回 status=1 (参考 sink 也是这样, 属正常)
    *P2P-INVITATION-RESULT*)     log "📬 邀请结果: $(printf "%s" "$line" | cut -c1-120)" ;;
    *P2P-INVITATION-ACCEPTED*)   log "✅ 手机接受了邀请: $(printf "%s" "$line" | cut -c1-120)" ;;
    *P2P-GO-NEG-REQUEST*)        log "📨 GO-Neg-Req (我们是 GO, 按 stock 拒绝, 不干预): $(printf "%s" "$line" | cut -c1-100)" ;;
    *P2P-GO-NEG-SUCCESS*)        log "✅ GO 协商成功: $(printf "%s" "$line" | cut -c1-140)" ;;
    *P2P-GO-NEG-FAILURE*)        log "ℹ GO 协商失败 (预期内): $(printf "%s" "$line" | cut -c1-120)" ;;
    *P2P-GROUP-FORMATION-SUCCESS*)  log "✅ 组建立成功" ;;
    *P2P-GROUP-FORMATION-FAILURE*)  log "❌ 组建立失败" ;;
    *P2P-GROUP-STARTED*)         log "✅ 组已启动: $(printf "%s" "$line" | cut -c1-140)" ;;
    *AP-STA-CONNECTED*)          log "ℹ 有 STA 加入我们 (一般不会发生): $(printf "%s" "$line" | cut -c1-120)" ;;
    *CTRL-EVENT-CONNECTED*)
        log "✅ 已关联: $(printf "%s" "$line" | cut -c1-130)"
        ;;
    *CTRL-EVENT-DISCONNECTED*)
        log "⚠ 断开: $(printf "%s" "$line" | cut -c1-130)"
        # 从手机组掉线 ⇒ 稍后恢复 GO 组, 保证手机还能发现我们
        ( sleep 5
          if ! is_connected && ! is_go && wpa status 2>/dev/null | grep -q "wpa_state=DISCONNECTED"; then
              restore_go
          fi ) &
        ;;
  esac
done
