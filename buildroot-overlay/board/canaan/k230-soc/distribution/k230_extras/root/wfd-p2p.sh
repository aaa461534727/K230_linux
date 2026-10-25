#!/bin/sh
# =============================================================================
# wfd-p2p.sh — K230 当 Wi-Fi Display SINK 的 P2P (Wi-Fi Direct) 侧
#
# 目的: 让手机的「无线投屏 / Cast」列表里出现 K230。
#       手机是通过 P2P 扫描 + WFD IE 识别 sink 的, 光开 RTSP 端口手机看不到。
#
# 前提 (已实测, 见 docs/MIRACAST_PORT.md §8.10):
#   1) 驱动 rtl8189fs 的 `iw list` 报 P2P-client / P2P-GO (仅 2.4G)
#   2) wpa_supplicant 编进了 CONFIG_P2P + CONFIG_WIFI_DISPLAY
#      (WFD 子元素命令 WFD_SUBELEM_SET/GET 可用)
#   3) ⚠ WFD IE 只有在 supplicant 的 global->p2p != NULL 时才塞进 beacon/probe-resp
#      ⇒ 必须用 P2P 配置重启 supplicant, 在 station 配置下 set 子元素是无效的
#
# ⚠⚠ 本脚本会把 wlan0 从「连路由器的 station」切成「P2P GO」:
#     · 切入后 wlan0 变成 GO (默认 192.168.49.1), 原 WiFi IP 消失 ⇒ SSH 会断
#     · 兜底通道: 串口 /dev/ttyUSB0 115200  (见 docs/BOARD_HARDWARE.md §10)
#     · `wfd-p2p.sh stop` 或断电重启即恢复原 station 配置
#
# WFD Device Information 子元素 (2+2+2 字节, 大端):
#   bytes 0-1  device info 位图 (AOSP android.net.wifi.p2p.WifiP2pWfdInfo):
#                bits 0-1  device type: 0=source 1=primary sink 2=secondary sink
#                                      3=source-or-primary-sink
#                bit  2    coupled sink supported at source
#                bit  3    coupled sink supported at sink
#                bits 4-5  session available mask (bit4=ok, bit5=overlapping)
#                bit  6    WFD service discovery supported
#                bit  7    preferred connectivity (0=P2P, 1=TDLS)
#                bit  8    content protection (HDCP 2.0/2.1) supported
#                bit  9    time sync (802.1AS)
#                bit 10    audio unsupported at primary sink
#   子元素字节 = 长度(2B BE) + device_info(2B) + RTSP端口(2B) + 最大平均吞吐(2B)
#   ⇒ 0006 = 正文 6 字节 / 0011 = primary sink + session available /
#     1c44 = RTSP 7236 / 0000 = 吞吐未声明  ⇒ 本脚本默认 000600111c440000
#   ⚠ wpa_supplicant 把你给的 hex **原样**当成子元素内容 (只在外层补 id=0),
#     **长度字段必须自己写进 hex**, 不写会下发成非法子元素 (2026-10-21 上板踩过)
#
# 用法:
#   wfd-p2p.sh start [额外 p2p_group_add 参数]   起 GO + 广播 WFD IE + DHCP
#   wfd-p2p.sh client                           ★推荐: 当 P2P client (手机/PC 当 GO, 地址自动判别)
#   wfd-p2p.sh listen                           只进入 P2P listen (不建组), 兼容只认 device 的 ROM
#   wfd-p2p.sh stop                              停 GO 并恢复原 station 配置
#   wfd-p2p.sh status                            看当前 P2P 状态 / GO SSID / WFD IE
#   wfd-p2p.sh devinfo <hex>                     改设备信息子元素 (调试位定义用)
# =============================================================================
set -eu

# 本机自编的 wpa_supplicant (带 PD 响应补丁) 是动态链接 buildroot 的 openssl 3.4,
# 而 Ubuntu rootfs 自带的是 3.0.x —— 版本符号对不上会启动失败。
# /root/wpalib 里放了配套的 libcrypto.so.3 / libssl.so.3, 有就优先用。
if [ -d /root/wpalib ]; then
    LD_LIBRARY_PATH="/root/wpalib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

# 用哪块网卡当 P2P GO:
#   默认优先用 **第二块 vif (wlan1)** —— 这样 wlan0 能继续当 station 连路由器 (不断网),
#   和参考设计 (海奇板 p2p0 独立接口、数据面走 p2p0) 一致。
#   只有 wlan1 不存在时才回退到 wlan0 (那就会牺牲路由器 WiFi)。
if [ -n "${WFD_IFACE:-}" ]; then
    IFACE="$WFD_IFACE"
elif ip link show wlan1 >/dev/null 2>&1; then
    IFACE=wlan1
else
    IFACE=wlan0
fi
STATION_IFACE="${WFD_STATION_IFACE:-wlan0}"
PIDFILE="${WFD_PIDFILE:-/run/wfd-p2p.pid}"
PBC_PIDFILE="${WFD_PBC_PIDFILE:-/run/wfd-pbc.pid}"
CTRL_DIR="${WFD_CTRL_DIR:-/run/wpa_supplicant}"
P2P_CONF="${WFD_P2P_CONF:-/tmp/wfd-p2p.conf}"
P2P_LOG="${WFD_P2P_LOG:-/tmp/wfd-p2p.log}"
BROADCAST_LOG="${WFD_BROADCAST_LOG:-/var/log/wfd-p2p.log}"
SSID_POSTFIX="${WFD_SSID_POSTFIX:-K230}" # GO SSID = DIRECT-<2随机字符><postfix>
# 2026-10-22: GO 走持久组 (id=9), SSID 写死, 默认与海奇一致 "DIRECT-"
GO_SSID="${WFD_GO_SSID:-DIRECT-}"
GO_IP="${WFD_GO_IP:-192.168.49.1}"       # GO 自身地址 (手机加入后才能访问 RTSP)
# client 模式 (手机自己当 GO): 我们作为 P2P client 用**固定** .128
#   依据 captures/p2p0.pcap: 能投屏的那一场里, 手机=192.168.49.1 (GO),
#   参考 sink=192.168.49.128, 且整个抓包里一帧 DHCP 都没有 ⇒ 静态地址。
CLIENT_IP="${WFD_CLIENT_IP:-192.168.49.128}"
CLIENT_WATCH_PID="${WFD_CLIENT_PIDFILE:-/run/wfd-client-watch.pid}"
DHCP_POOL="${WFD_DHCP_POOL:-192.168.49.10}"  # dhcpd 发的地址池起始
DHCP_LOG="${WFD_DHCP_LOG:-/tmp/dhcpd.log}"
# ⚠ 注意编码: wpa_supplicant 的 WFD_SUBELEM_SET 存的是 [id 由它自己加] + 你给的 hex 原样,
#   所以 **hex 里要自己包含 2 字节大端长度**: 0006=6 字节 | 0011=primary sink+session available
#   | 1c44=RTSP 7236 | 0000=吞吐未声明
# WFD 子元素 0 = Device Information (必须是「WFD primary sink」!)
# 布局: subelem_id(1) len(2 LE) device_info(2) session_mgmt_port(2 大端) max_throughput(2 大端)
#
# ⚠ 2026-10-21 抓包对账: 旧值 `0011` 解出来 Device Type = 00 = **WFD Source**,
#   等于我们把自己扮成了「手机」(source), 手机拿到这个 WFD IE 后跟自己对撞,
#   PD 应答正常但之后一帧 GO Negotiation 都不发 (air 抓包实证: 手机只发 PD)。
#   参考 sink (`captures/20261021-hichip-sink-sniff-ch6.pcap`, 同一台手机能投) 实测为:
#     dd20 506f9a0a | 00 06 01 51 | 02 2a | 01 2c | ...
#     device_info=0x5101 ⇒ bit0-1=01 = Primary Sink, 另有 WFD Service Discovery /
#     Available for WFD Session / HDCP2.0 位
#   故取 `0151` + 我方 RTSP 端口 7236 (`1c44`) + 吞吐 300Mbps (`012c`)。
#
# ⚠ 2026-10-21 22:55 再对账 (`captures/20261021-2238-k230-v5-attempt.pcap` frame 6127
#   vs `captures/20261021-2050-hichip-success-reference.pcap` frame 2261):
#   手机**收到了**我们的 PD-Resp (frame 6136 = 它对我们的 ACK), token / Config Methods 全对,
#   之后 30 s 却零 GO-Neg。两边 PD-Resp 唯一剩下的差异就是 WFD IE:
#     我们 13 B: 00 06 | 01 51 | 1c44 | 012c            (HDCP2.0=True, port=7236, 只有子元素 0)
#     海奇 32 B: 00 06 | 00 51 | 022a | 012c + 01.. + 06.. (HDCP2.0=False, port=554)
#   ⇒ 本轮改成与海奇一致: dev_info 0x5100 (HDCP2.0 off)、port 554,
#     并补 Associated BSSID(1) / Coupled Sink Info(6) 两个子元素 (全 0, 与海奇相同)。
#   对照实验目的: 排除「手机看到 sink 声明 HDCP2.0 就放弃建组」这条路径。
# ⚠ 2026-10-21 23:05 **回退 dev_info** (改回 0151 / port 7236): 改成海奇值后
#   主人反馈「手机找不到 K230 了」(板端 beacon 正常, 129 s 1197 个, ch6) ⇒
#   怀疑手机列表按 WFD device_info 位做了过滤。故先恢复「手机能看到」的位,
#   只保留海奇那两个子元素做**单变量**对照 (待主人再试确认)。
DEVINFO="${WFD_DEVINFO:-000601511c44012c}"
# WFD 子元素 1 = Associated BSSID (海奇发全 0), 6 = Coupled Sink Info (海奇发全 0)
# 取值 "none" = 不发该子元素 (对照实验用)
WFD_SUBELEM_ASSOC_BSSID="${WFD_SUBELEM_ASSOC_BSSID:-0006000000000000}"
WFD_SUBELEM_COUPLED_SINK="${WFD_SUBELEM_COUPLED_SINK:-000700000000000000}"
# 2026-10-25: 默认改 ch11 (2462)。Windows 10 的 P2P listen/操作信道默认就是 ch11,
# 我们若停在 ch6, 它发的 GO Negotiation Confirmation 落在 ch11 我们听不到 ⇒
# 双方都超时 (PC 弹"无法连接")。8189fs 只支持 2.4G, ch11 与手机历史成功帧一致。
GO_FREQ="${WFD_GO_FREQ:-2462}"           # 2.4G ch11
# listen 信道必须跟组信道一致 (手机把 GO-Neg/PD 发到它看到的 listen 信道)。
# 由 GO_FREQ 反推: 2412→1, 2437→6, 2462→11; 也可用 WFD_LISTEN_CHANNEL 直接指定。
case "$GO_FREQ" in
    2412) _def_ch=1 ;;
    2437) _def_ch=6 ;;
    2462) _def_ch=11 ;;
    *)    _def_ch=6 ;;
esac
LISTEN_CHANNEL="${WFD_LISTEN_CHANNEL:-$_def_ch}"
DEVICE_NAME="${WFD_DEVICE_NAME:-K230-Miracast}"

log()  { printf '[wfd-p2p] %s\n' "$*"; }
die()  { printf '[wfd-p2p] ✗ %s\n' "$*" >&2; exit 1; }

# ⚠ 已验收基线: GO 位必须显示
#   2026-10-22 实测: 隐藏 GO 位 (group_capab 0x0b→0x0a) 后, 手机投屏列表里直接消失;
#   恢复 0x0b 后 beacon 与已验收版逐字节一致 (captures/20261022-1505-k230-v11-fix.pcap)
#   → 不许再改, 改前先看 docs/PROGRESS.md 2026-10-22 条目
export P2P_HIDE_GO=0

need_root() { [ "$(id -u)" = "0" ] || die "需要 root (wpa_supplicant / 网卡配置)"; }

wpa() { wpa_cli -p "$CTRL_DIR" -i "$IFACE" "$@" 2>&1; }

wait_ctrl() {
    i=0
    while [ "$i" -lt 40 ]; do
        [ -S "$CTRL_DIR/$IFACE" ] && wpa ping >/dev/null 2>&1 && return 0
        i=$((i + 1)); sleep 0.25
    done
    return 1
}

# 找出当前是谁在管 wpa_supplicant: systemd 单元名 (Ubuntu: wpa_supplicant.service 或 wpa_supplicant@wlan0)
backup_conf() {
    for c in "/etc/wpa_supplicant/wpa_supplicant-$IFACE.conf" \
             "/etc/wpa_supplicant/wpa_supplicant.conf"; do
        if [ -f "$c" ]; then
            cp -a "$c" "/tmp/wfd-p2p-backup.conf"
            log "已备份 $c → /tmp/wfd-p2p-backup.conf"
            return 0
        fi
    done
    log "没找到现有 wpa_supplicant conf (可能全部来自 NetworkManager), 跳过备份"
}

write_conf() {
    _mode="${1:-go}"
    # client 模式必须让手机赢 GO 协商 (intent 0); go 模式用 autonomous group, 该值不生效
    if [ "$_mode" = "client" ]; then
        _go_intent_line="p2p_go_intent=0"
    else
        _go_intent_line="# p2p_go_intent 默认 7 (autonomous GO 不需要)"
    fi
    # 2026-10-25 修复「连一次后第二次连不上」: 旧 conf 里的**对端持久组**要留着。
    # wpa_supplicant 把上次建组时拿到的 PSK 存成 network{} 块 (带 bssid=),
    # 对端 (Windows / 手机) 记得那个组, 再点会用 Invitation 邀请我们回去;
    # 我们这边一旦把 conf 重写掉, 就成了 unknown-network ⇒ 只能重协商,
    # 而 Windows 这时候正等 Invitation Response, 于是对不上 ⇒ "无法连接"。
    _keep_nets=""
    if [ -f "$P2P_CONF" ]; then
        _keep_nets=$(awk '/^network=\{/{f=1; buf=""} f{buf=buf $0 "\n"} f&&/^\}/ {f=0; if (buf ~ /bssid=/) printf "%s", buf}' "$P2P_CONF" 2>/dev/null)
    fi

    cat > "$P2P_CONF" <<EOF
# 由 wfd-p2p.sh 生成 — 只用于 Miracast sink 的 P2P 模式
ctrl_interface=DIR=$CTRL_DIR GROUP=netdev
update_config=1
ap_scan=1

device_name=$DEVICE_NAME
manufacturer=Codebuff
model_name=K230
model_number=01Studio
serial_number=00000000

# 2026-10-22 关键修复 ①: 主设备类型必须写 "7-0050F204-1" (category 7 = Display)。
# 不写 ⇒ P2P Device Info / WPS 0x1054 里全是 00-00-00-00-00-00, 手机投屏列表会
# 直接不显示这台 sink (海奇真机就是 00070050f2040001)。丢掉的机制见 docs/PROGRESS.md。
device_type=7-0050F204-1

# 手机一般用 PBC (WPS 按钮) 或显示 PIN; 三个都给
config_methods=display push_button keypad virtual_display physical_display

# 单网卡 (8189fs) 不要再开 group interface, 直接用 wlan0 当 GO
p2p_no_group_iface=1

# 2026-10-25 修复「连一次就再也连不上」: wpa_supplicant 默认在建组后按
# p2p_group_idle 秒数做空闲回收 —— 组起来后若 10 s 内没有数据流量
# (DHCP/RTSP 还没跑起来、或手机刚断开), 它自己就把组拆了:
#   P2P-GROUP-REMOVED reason=IDLE → 对端看到掉线 → 再点就"无法连接"。
# 0 = 关闭空闲回收, 组只由对端结束会话或本脚本 stop/重建时拆。
p2p_group_idle=0

# 2026-10-25: 对端 (Windows/手机) 再点连接时会发 Invitation 回旧组。
# 不打开这个开关, supplicant 只抛 P2P-INVITATION-RECEIVED 事件不动作;
# 打开后它按已存的持久组自动回 Invitation Response 并重新入组。
persistent_reconnect=1

# 2026-10-21 关键修复: listen 信道必须 = 组所在信道 (ch6)。
# 默认 listen 是 ch11, 手机会把 GO Negotiation 发到它看到的 listen 信道,
# 而我们 GO 在 ch6 -> 一帧 GO Neg 都收不到 (现象: PD 全收到, 之后死等 30s 重发)。
p2p_listen_reg_class=81
p2p_listen_channel=$LISTEN_CHANNEL

# 2026-10-23 关键修复 ③: 显式声明"操作信道", 逼 GO-Neg-Resp 一定带 Operating Channel 属性。
# 背景: 手机当 GO 时, 上游 supplicant 在 p2p->num_pref_freq == 0 时会走
#   "Omit Operating Channel attribute" —— 手机于是自己挑信道 (实测挑到 ch8/2447),
#   然后对我们 **179 帧合法 AUTH 一帧都不 ACK**, 16 s 后 FORMATION_FAILED。
# 8189fs 走 cfg80211 拿不到驱动 PCL, 所以只能靠配置把信道钉死:
#   p2p_oper_reg_class + p2p_oper_channel ⇒ cfg_op_channel=1 ⇒ SDK 补丁
#   (0100-k230-miracast.patch) 才会带上 Operating Channel, 并把 Channel List 收成
#   只剩这一个信道, 手机没有"挑错"的余地。
# ⚠ 绝对不要写 p2p_pref_chan: p2p_check_pref_chan() 见到 cfg->num_pref_chan 非零就
#   直接 return, num_pref_freq 永远 0 —— 补丁之前正是它让属性一直被省略 (见
#   docs/MIRACAST_AUTH_WINDOW.md §10.3)。它只对"我们自己发 GO-Neg-Req"那条路生效。

# 固定 SSID 后缀, 免得每次 p2p_group_add 都换一个 "DIRECT-XXK230" 名字,
# 手机那边缓存/用户认名字都会乱 (2026-10-21)。
p2p_ssid_postfix=wrK230

$_go_intent_line

# 2026-10-22 关键修复 ②: GO 用"持久组"启动 (id=9), SSID 写死成 $GO_SSID, 不再每次
# p2p_group_add 随机生成 "DIRECT-XXK230"。
#   · 海奇真机的 GO SSID 就是 "DIRECT-" —— 正好等于手机 P2P 发现时扫的通配 SSID,
#     定向 probe 精确命中 (我们随机后缀时对不上, 手机列表里就没了)
#   · SSID 固定后, 手机缓存的 DIRECT-<xx> 名字不会因为重启而失效
# (wpa_supplicant 2.11 的 network 块不认 id= / p2p_persistent_group= 这两个字段,
#  持久组就靠 mode=3 + disabled=2 表达; 索引 = network 块顺序 = 0)
network={
    ssid="$GO_SSID"
    mode=3
    disabled=2
    key_mgmt=WPA-PSK
    psk="k230miracast"
}

# ⚠ 不要加 wifi_display=1 / p2p_no_go / p2p_no_cli:
#   wpa_supplicant 2.10/2.11 里根本没有这些 **全局配置字段**, 写了会
#   "unknown global field" 直接导致整个 conf 解析失败 (2026-10-21 踩过)。
#   · Wi-Fi Display: 编进 CONFIG_WIFI_DISPLAY 后 wifi_display_init() 就默认置 1
#   · no_go / no_cli: 是 p2p_set 的**运行时**参数, 不是 conf 字段
EOF
    # 还原对端持久组 (见上), 并让 supplicant **自动接受**对端的 Invitation ——
    # 否则它只抛 P2P-INVITATION-RECEIVED 事件, 前端不处理就超时。
    if [ -n "$_keep_nets" ]; then
        printf '%s\n' "$_keep_nets" >> "$P2P_CONF"
        log "已还原 $(printf '%s\n' "$_keep_nets" | grep -c '^network={') 个对端持久组"
    fi

    # 2026-10-25: 操作信道默认**不**钉死。钉死时 (上面补丁) GO-Neg-Resp 会带
    # Operating Channel 且 Channel List 只剩 1 个信道; 实测 Windows 10 收到这种
    # 应答后**不发 GO Negotiation Confirmation**, 600 ms 后我们超时 ⇒ PC "无法连接"。
    # 手机老路 (钉死 ch6, 防手机挑到 ch8) 用 WFD_FORCE_OPER_CH=1 打开。
    if [ "${WFD_FORCE_OPER_CH:-0}" = "1" ]; then
        cat >> "$P2P_CONF" <<EOF2
p2p_oper_reg_class=81
p2p_oper_channel=$LISTEN_CHANNEL
EOF2
    fi
    log "已生成 $P2P_CONF (device_name=$DEVICE_NAME, force_oper_ch=${WFD_FORCE_OPER_CH:-0})"
}

stop_supplicant() {
    # 目标: 只停“我们自己在 P2P 网卡上起的那个 supplicant”, 不动 wlan0 的 station。
    # 如果回退到 wlan0 当 GO (没有 wlan1 的机器), 才需要跟 systemd 单元斗。

    # A) 我们自己的实例: 按 pidfile 精准杀
    if [ -f "$PIDFILE" ]; then
        local p
        p=$(cat "$PIDFILE" 2>/dev/null || true)
        if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
            if [ -S "$CTRL_DIR/$IFACE" ]; then
                wpa p2p_group_remove "$IFACE" >/dev/null 2>&1 || true
                wpa terminate >/dev/null 2>&1 || true
            fi
            i=0
            while [ "$i" -lt 20 ] && kill -0 "$p" 2>/dev/null; do
                i=$((i + 1)); sleep 0.25
            done
            kill -9 "$p" 2>/dev/null || true
        fi
        rm -f "$PIDFILE"
    fi

    # A2) 兜底: pidfile 只记最后一个实例, 反复 start 会叠加出多个 supplicant
    #     (都抢 wlan1 → 一个建组一个报错, 表现为"起来了又死了")
    #     按 /proc/<pid>/cmdline 匹配 pidfile 路径, 只杀 wpa_supplicant 本身
    _kill_by_pidfile() {
        for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
            if grep -qa "$PIDFILE" "/proc/$_p/cmdline" 2>/dev/null; then
                kill "$_p" 2>/dev/null || true
            fi
        done
    }
    _kill_by_pidfile
    i=0
    while [ "$i" -lt 20 ]; do
        _stale=0
        for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
            grep -qa "$PIDFILE" "/proc/$_p/cmdline" 2>/dev/null && _stale=1
        done
        [ "$_stale" = 0 ] && break
        i=$((i + 1)); sleep 0.25
    done
    for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
        grep -qa "$PIDFILE" "/proc/$_p/cmdline" 2>/dev/null && kill -9 "$_p" 2>/dev/null
    done
    rm -f "$CTRL_DIR/$IFACE" "$CTRL_DIR/p2p-dev-$IFACE" 2>/dev/null || true

    # B) 只有回退到 wlan0 时才需要 mask/stop/重启 systemd 的 wpa_supplicant@wlan0
    [ "$IFACE" = "$STATION_IFACE" ] || return 0

    # 顺序很重要: 先 mask 再任何“让进程退出”的动作。
    # 否则 systemd 的 wpa_supplicant@wlan0.service (Restart=on-failure) 会在
    # 我们 kill / terminate 之后秒拉起, 抢走 ctrl socket → 自己的实例 bind 失败。
    # 2026-10-21 上板反复踩, 最终顺序 = mask → 良性退出 → stop → kill → 清 socket。

    # 1) 先 mask (runtime, 重启即失效)
    for u in "wpa_supplicant@$IFACE" "wpa_supplicant"; do
        systemctl mask --runtime "$u" 2>/dev/null || true
    done
    for u in "wpa_supplicant@$IFACE" "wpa_supplicant" "wpa_supplicant.service"; do
        systemctl stop "$u" 2>/dev/null || true
    done

    # 2) 良性退出: 直接 SIGKILL 的话驱动里的 AP / action frame 注册不释放,
    #    下次启动会刷 "Match already configured" 然后起不来
    if [ -S "$CTRL_DIR/$IFACE" ]; then
        wpa p2p_group_remove "$IFACE" >/dev/null 2>&1 || true
        wpa terminate >/dev/null 2>&1 || true
        i=0
        while [ "$i" -lt 20 ]; do
            pgrep -x wpa_supplicant >/dev/null 2>&1 || break
            i=$((i + 1)); sleep 0.25
        done
    fi

    # 3) 清残留进程: 必须用 -x (精确匹配进程名),
    #    用 -f 会把自己 / 外层 shell 的命令行也匹配进去 (2026-10-21 把自己 kill 过)
    i=0
    while [ "$i" -lt 40 ]; do
        pgrep -x wpa_supplicant >/dev/null 2>&1 || break
        pkill -9 -x wpa_supplicant 2>/dev/null || true
        i=$((i + 1)); sleep 0.25
    done
    pgrep -x wpa_supplicant >/dev/null 2>&1 && die "wpa_supplicant 杀不掉 (pid $(pgrep -x wpa_supplicant | tr '\n' ' '))"

    # 4) 清 socket (包括 p2p-dev-*), 并确认真的没人监听了
    i=0
    while [ "$i" -lt 20 ]; do
        rm -f "$CTRL_DIR/$IFACE" "$CTRL_DIR/p2p-dev-$IFACE" 2>/dev/null || true
        wpa ping >/dev/null 2>&1 || return 0     # 没人应答 = 干净
        page=$(pgrep -x wpa_supplicant | tr '\n' ' ' || true)
        [ -n "$page" ] && pkill -9 -x wpa_supplicant 2>/dev/null || true
        i=$((i + 1)); sleep 0.25
    done
    die "ctrl socket 一直有人应答, 清不掉"
}

start_supplicant() {
    : > "$P2P_LOG"
    if ! wpa_supplicant -B -P "$PIDFILE" -i "$IFACE" -D nl80211 -c "$P2P_CONF" -f "$P2P_LOG" -dd -t; then
        # 常见原因: 上一个实例在 AP 模式下被强杀, 驱动/vif 状态没释放
        # (日志里是 "ctrl_iface bind: Address already in use" 或 "Match already configured")
        # 把网卡拉下去再上来, 清一次 socket, 重试一次
        log "⚠ 首次启动失败, 复位 $IFACE 后重试一次 (日志 $P2P_LOG)"
        ip link set dev "$IFACE" down 2>/dev/null || true
        rm -f "$CTRL_DIR/$IFACE" "$CTRL_DIR/p2p-dev-$IFACE" 2>/dev/null || true
        sleep 1
        ip link set dev "$IFACE" up 2>/dev/null || true
        sleep 1
        if ! wpa_supplicant -B -P "$PIDFILE" -i "$IFACE" -D nl80211 -c "$P2P_CONF" -f "$P2P_LOG" -dd -t; then
            log "✗ wpa_supplicant 启动失败, 日志 $P2P_LOG"
            log "→ 自动恢复 station 模式, 免得上不了网"
            cmd_stop || true
            die "见上面日志 (常见: conf 里有 supplicant 不认识的字段)"
        fi
    fi
    if ! wait_ctrl; then
        cmd_stop || true
        die "等不到 ctrl socket $CTRL_DIR/$IFACE, 看 $P2P_LOG"
    fi
    log "wpa_supplicant 已起 (P2P 配置)"
}

cmd_start() {
    need_root
    backup_conf
    write_conf
    systemctl start miracast.service 2>/dev/null || true
    rm -f /tmp/wfd-p2p.done /tmp/wfd-p2p.failed
    start_supplicant

    # 1) WFD IE 内容: 设备信息子元素 (必须在 p2p 起来之后设, 顺序其实无所谓, 但先设更稳)
    log "设置 WFD 设备信息: $DEVINFO"
    wpa wfd_subelem_set 0 "$DEVINFO" | grep -q OK || die "WFD_SUBELEM_SET 失败"
    # 1 = Associated BSSID, 6 = Coupled Sink Info (对齐海奇 PD-Resp 的 32 B WFD IE)
    # 取值 "none" ⇒ 清掉该子元素 (对照实验)
    set_subelem() { # $1=id  $2=hex|none
        if [ "$2" = "none" ]; then
            wpa wfd_subelem_set "$1" "" >/dev/null 2>&1 || true
        else
            wpa wfd_subelem_set "$1" "$2" >/dev/null 2>&1 || log "⚠ subelem $1 没设上"
        fi
    }
    set_subelem 1 "$WFD_SUBELEM_ASSOC_BSSID"
    set_subelem 6 "$WFD_SUBELEM_COUPLED_SINK"
    # 已验证的当前内容回读 (确认 supplicant 真的存下来了)
    log "回读: 0=[$(wpa wfd_subelem_get 0 | tail -1)] 1=[$(wpa wfd_subelem_get 1 | tail -1)] 6=[$(wpa wfd_subelem_get 6 | tail -1)]"

    # 2) 起 GO (autonomous group), 手机扫描时能看到 DIRECT-xx-K230-Miracast
    # GO 名字: DIRECT-<2 随机字符><postfix>, 手机列表里就是它是啥名
    wpa p2p_set ssid_postfix "$SSID_POSTFIX" >/dev/null 2>&1 || log "⚠ ssid_postfix 没设上 (不影响发现)"

    # 2026-10-21: 先把 listen 信道对齐到组信道, 否则手机把 GO Negotiation 发到
    # 它看到的 listen 信道(默认 ch11), 我们 GO 在 ch6 -> 收不到, 表现为 PD 之后死等
    wpa p2p_set listen_channel "$LISTEN_CHANNEL" >/dev/null 2>&1 || true
    log "p2p_group_add freq=$GO_FREQ $* (listen_channel=$LISTEN_CHANNEL)"
    # 优先用持久组 (SSID 固定 = $GO_SSID); 失败则回退普通 p2p_group_add (随机 SSID)
    if ! wpa p2p_group_add "freq=$GO_FREQ" persistent=0 "$@" >/dev/null 2>&1; then
        log "⚠ 持久组 (persistent=0) 起不来, 回退普通 p2p_group_add (SSID 会是随机后缀)"
        wpa p2p_group_add "freq=$GO_FREQ" "$@" >/dev/null || die "p2p_group_add 失败, 看 $P2P_LOG"
    fi
    wpa p2p_set listen_channel "$LISTEN_CHANNEL" >/dev/null 2>&1 || true

    # 3) 等 P2P-GROUP-STARTED
    i=0
    while [ "$i" -lt 40 ]; do
        if wpa status | grep -q "wpa_state=.*COMPLETED\|p2p_device_address"; then :; fi
        if grep -q "P2P-GROUP-STARTED" "$P2P_LOG" 2>/dev/null; then break; fi
        i=$((i + 1)); sleep 0.25
    done
    grep -q "P2P-GROUP-STARTED" "$P2P_LOG" || die "GO 没起来, 看 $P2P_LOG"

    go_if=$(sed -n 's/.*P2P-GROUP-STARTED \([^ ]*\).*/\1/p' "$P2P_LOG" | tail -1)
    # 日志格式: P2P-GROUP-STARTED <iface> <role> ssid="DIRECT-xxK230" freq=2437 ...
    ssid=$(sed -n 's/.*P2P-GROUP-STARTED [^ ]* [^ ]* ssid="\([^"]*\)".*/\1/p' "$P2P_LOG" | tail -1)
    log "✅ GO 已起: iface=${go_if:-$IFACE} ssid=${ssid:-?} freq=$GO_FREQ"

    # ⚠ 信道硬约束 (2026-10-20 实测): 8189fs 驱动两个 vif 单信道共存 ⇒ 如果 wlan0
    #   station 连着非 1/6/11 的 AP (例如 ch9), GO 会被顶到同一信道; 而手机 P2P 发现
    #   只在社交信道 (1/6/11) 探测 ⇒ 永远看不到 sink。
    go_freq=$(wpa status 2>/dev/null | sed -n 's/^freq=//p' | head -1)
    case "$go_freq" in
        2412|2437|2462)
            log "✅ GO 在社交信道 (freq=$go_freq, ch 1/6/11), 手机能扫到" ;;
        "")
            log "⚠ 读不到 GO 频率 (wpa status 异常)" ;;
        *)
            log "⚠ GO 不在社交信道 (freq=$go_freq) — 手机只在 1/6/11 探测, 发现不了!"
            log "   处理: 断开 wlan0 station (wpa_cli -i wlan0 disconnect) 或让它连 1/6/11 的 AP, 再重启" ;;
    esac

    # GO 自身地址: wpa_supplicant 不会自动配, 不配手机加入后访问不到 7236
    ip addr add "$GO_IP/24" dev "${go_if:-$IFACE}" 2>/dev/null || true
    log "GO 地址: $(ip -4 -o addr show "${go_if:-$IFACE}" | awk '{print $4}' | head -1)"

    # 4) 拉 DHCP 服务: 手机加入组后必须有 DHCP 才拿得到 IP
    start_dhcpd "${go_if:-$IFACE}"

    # 4.5) WPS PBC 自动应答: 手机点选后发 PROV-DISC-PBC-REQ, 不回就连不上
    start_pbc_watcher

    # 5) 证据: 日志里必须能找到 "WFD IE" 构建记录 (说明 beacon 会带)
    if grep -q "WFD: Update WFD IE" "$P2P_LOG"; then
        log "✅ 日志有 'WFD: Update WFD IE' (WFD IE 已生效)"
        log "实际下发的 WFD IE: $(grep 'WFD: WFD IE for Probe Response' "$P2P_LOG" | tail -1 | sed 's/.*hexdump(len=[0-9]*): //')"
    else
        log "⚠ 日志没看到 'WFD: Update WFD IE', 下面贴相关行:"
        grep -i "wfd" "$P2P_LOG" | tail -10 || true
    fi

    cp "$P2P_LOG" "$BROADCAST_LOG" 2>/dev/null || true
    log "现在用手机开「无线投屏」, 列表里应出现 ${ssid:-K230}"
    log "GO 网关 IP 一般是 192.168.49.1 (手机侧可访问 192.168.49.1:7236 的 RTSP sink)"
}

stop_dhcpd() {
    pkill -x dhcpd 2>/dev/null && log "已停 dhcpd" || true
}

# ---------------------------------------------------------------- WPS PBC 应答
# 手机(小米/红米实测)发现 sink 后, 点选会先发 P2P Provision Discovery (PBC);
# GO 侧必须回一次 `wps_pbc`, 手机才能用 WPS 按钮方式加入组并拿 DHCP。
# 之前不回 => 日志里只有 P2P-PROV-DISC-PBC-REQ, 然后 P2P-DEVICE-LOST, 连不上。
start_pbc_watcher() {
    [ -f "$P2P_LOG" ] || return 0
    # 2026-10-21: 让位逻辑已独立成 wfd-pbc-watcher.sh (同目录, 见其文件头注释):
    #   K230 在本硬件上只能用"跑 GO 组"的方式被手机看见 (managed 模式下驱动不上报
    #   probe request); 但 GO 组跑着时 supplicant 的 p2p->state != IDLE, 手机发来的
    #   GO Negotiation Request 会被回 Status=1 (INFO_CURRENTLY_UNAVAILABLE), 手机就放弃。
    #   ⇒ 让位: 收到 GO-Neg/PD 后立刻 p2p_group_remove + p2p_listen, 再由我们主动
    #     p2p_connect 让手机当 GO, 我们当 client; 15s 没结果就恢复 GO 组。
    w="$(dirname "$0")/wfd-pbc-watcher.sh"
    if [ -x "$w" ]; then
        "$w" > /tmp/wfd-yield.log 2>&1 &
        echo $! > "$PBC_PIDFILE"
        log "PBC/让位 watcher 已启动 (pid $(cat "$PBC_PIDFILE" 2>/dev/null))"
    else
        log "⚠ 未找到 $w (让位 watcher 缺失, 手机发 GO-Neg 会被拒)"
    fi
    # ⚠ 这里**不要**再开 p2p_find: GO 已在 ch6 当 AP, 开 find 会跳台漏帧
    log "P2P 发现: 不主动 scan (GO 常驻 ch6, 由手机扫我们)"
}

stop_pbc_watcher() {
    if [ -f "$PBC_PIDFILE" ]; then
        p=$(cat "$PBC_PIDFILE" 2>/dev/null || true)
        [ -n "$p" ] && kill "$p" 2>/dev/null || true
        rm -f "$PBC_PIDFILE"
    fi
    # 只杀子 shell 会留下孤儿的 tail -F, 每次重启叠一个 (实测攒了 3 个,
    # 老的还在跑旧逻辑) ⇒ 按命令行精确清掉本日志的 tail
    pkill -f "tail -n 0 -F $P2P_LOG" 2>/dev/null || true
    # 2026-10-21: watcher 里还有个 keep_pbc 后台子 shell (同脚本名下运行),
    # 只 kill 主 pid 会把它留成孤儿 ⇒ 按脚本名一起清掉 (pkill 不会杀自己)
    pkill -f "wfd-pbc-watcher.sh" 2>/dev/null || true
}

start_dhcpd() {
    local dev="$1"
    local bin=""
    for c in /usr/bin/dhcpd /root/dhcpd "$(dirname "$0")/dhcpd" ./dhcpd; do
        [ -x "$c" ] && { bin="$c"; break; }
    done
    if [ -z "$bin" ]; then
        log "⚠ 没找到 dhcpd 二进制, 手机加入后会拿不到 IP (把 dhcpd 放 /usr/bin/ 或本脚本同目录)"
        return 0
    fi
    pkill -x dhcpd 2>/dev/null || true
    setsid nohup "$bin" -i "$dev" -a "$GO_IP" -p "$DHCP_POOL" -n 16 -v \
        >> "$DHCP_LOG" 2>&1 &
    sleep 1
    if pgrep -x dhcpd >/dev/null 2>&1; then
        log "✅ dhcpd 已起: 池 $DHCP_POOL +16 (/24), 网关/DNS $GO_IP, 日志 $DHCP_LOG"
    else
        log "⚠ dhcpd 没起来, 看 $DHCP_LOG"
    fi
}

# ---------------------------------------------------------------- listen 模式
# 很多电视/盒子在发现阶段是「P2P device (listen)」而不是「已激活的 GO」。
# Android 的投屏列表对这两种形态处理不一样, 若 GO 模式手机看不到, 先试这个。
cmd_listen() {
    need_root
    backup_conf
    write_conf
    stop_dhcpd
    stop_supplicant
    start_supplicant

    log "设置 WFD 设备信息: $DEVINFO"
    wpa wfd_subelem_set 0 "$DEVINFO" | grep -q OK || die "WFD_SUBELEM_SET 失败"
    wpa p2p_set ssid_postfix "$SSID_POSTFIX" >/dev/null 2>&1 || true

    # p2p_listen 不带 timeout = 一直听; 手机 p2p_find 时会看到我们作为 P2P device
    log "进入 P2P listen 模式 (作为 device 播, 不先建组)"
    wpa p2p_listen >/dev/null || die "p2p_listen 失败, 看 $P2P_LOG"
    sleep 2

    log "当前 p2p 状态: $(wpa status | grep -E 'p2p_device_address|wpa_state' | tr '\n' ' ')"
    log "WFD IE: $(grep 'WFD: WFD IE for Probe Response' "$P2P_LOG" | tail -1 | sed 's/.*hexdump(len=[0-9]*): //')"
    log "现在用手机「无线投屏」找 (本模式不建组, 所以没有 192.168.49.1; 手机选了才会建组)"
    log "建组后看日志: grep -E 'P2P-GO-NEG-REQUEST|P2P-GROUP-STARTED|AP-STA-CONNECTED' $P2P_LOG"
}

# ---------------------------------------------------------------- client 模式
# 依据参考抓包 captures/p2p0.pcap: 能投屏的那台手机**自己当 P2P GO**
#   (192.168.49.1, 同时是 RTSP client + RTP 发送端), sink 是 P2P client
#   (192.168.49.128) 并在 7236 跑 RTSP 服务端。
# 所以 client 模式: 我们只做发现 + 监听, 让出 GO(intent=0), 手机协商成功后
#   用固定 IP 192.168.49.128 加入, 手机自己会连过来开 RTSP。
# ---- sink 客户端 pipeline (真手机投屏走这条) ---------------------------
# 依据参考抓包 captures/p2p0.pcap:
#   手机 = P2P GO = 192.168.49.1 = RTSP 服务端(:7236) = RTP 发送端
#   本机 = P2P client = 192.168.49.128 = RTSP 客户端(主动去连) = RTP 接收端
# 所以组建成 client 后, 必须把 pipeline 从「监听服务端」切成「客户端去连」,
# 否则手机永远等不到 SETUP/PLAY (手机不会主动来连我们)。
SINK_PIPE_PID=/tmp/wfd-sink-client.pid
# GO / RTSP 服务端地址: 留空 = 建组后自动判别 (不写死, 见 prepare_client_net)
#   安卓手机当 GO: 192.168.49.1 (静态 192.168.49.128, 历史成功链路)
#   Windows 10 当 GO: 192.168.137.1 (ICS 段, 必须 DHCP 拿租约)
SINK_GO_IP="${WFD_SINK_GO_IP:-}"
SINK_GO_PORT="${WFD_SINK_GO_PORT:-7236}"
CLIENT_IF=""
DHCP_WAIT="${WFD_DHCP_WAIT:-6}"

# ---- 网络自适应 (2026-10-24 加: 让 PC/Win10 也能投进来) --------------------
iface_ip() { ip -4 addr show dev "$1" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1; }
iface_gw() { ip route 2>/dev/null | awk -v d="$1" '$1=="default" && $NF==d {print $3; exit}'; }

# 跑一次 DHCP 客户端, 成功时 stdout 只输出 GO(server) 的 IP。
#   ① 优先板端自带的极简客户端 (不做 ARP probe; 手机 GO 用 dhcpcd 会卡在探测)
#   ② dhcpcd / udhcpc 兜底, 从默认网关或本端子网 .1 推 GO
dhcp_acquire() {
    _if="$1"
    for _p in /usr/local/bin/k230-dhcp-simple.py /root/k230-dhcp-simple.py /tmp/k230-dhcp-simple.py; do
        [ -f "$_p" ] || continue
        _out=$(timeout $((DHCP_WAIT + 4)) python3 "$_p" "$_if" "$DHCP_WAIT" 2>&1 || true)
        _srv=$(printf '%s\n' "$_out" | sed -n 's/.*SERVER=\([0-9.]*\).*/\1/p' | tail -1)
        if [ -n "$_srv" ]; then printf '%s' "$_srv"; return 0; fi
    done
    if command -v dhcpcd >/dev/null 2>&1; then
        timeout "$DHCP_WAIT" dhcpcd -4 -q -t 4 -w -1 -m 2000 "$_if" >/dev/null 2>&1 || true
    elif command -v udhcpc >/dev/null 2>&1; then
        timeout "$DHCP_WAIT" udhcpc -i "$_if" -t 4 -T 1 -n -q >/dev/null 2>&1 || true
    fi
    _gw=$(iface_gw "$_if"); _ip=$(iface_ip "$_if")
    case "$_ip" in ""|0.0.0.0|169.254.*) _ip="" ;; esac
    if [ -n "$_gw" ]; then printf '%s' "$_gw"; return 0; fi
    if [ -n "$_ip" ]; then printf '%s' "$(printf '%s' "$_ip" | sed 's/\.[0-9]*$/.1/')"; return 0; fi
    return 1
}

# 建组(role=client)后: 配地址 + 认出 GO。安卓零回归, Windows 走 DHCP。
prepare_client_net() {
    _if="$1"
    CLIENT_IF="$_if"
    ip link set dev "$_if" up 2>/dev/null || true
    ip addr flush dev "$_if" 2>/dev/null || true

    # ① 安卓手机路径: 静态 + 1 s 探 GO (与历史成功链路完全一致, 不引入延迟)
    ip addr add "$CLIENT_IP/24" dev "$_if" 2>/dev/null || true
    if ping -c1 -W1 -I "$_if" 192.168.49.1 >/dev/null 2>&1; then
        SINK_GO_IP=192.168.49.1
        log "✅ GO=192.168.49.1 (Android 手机路径, 静态 $CLIENT_IP/24)"
        return 0
    fi

    # ② Windows 10 / 任何按标准跑 DHCP 的 GO: Windows 只认租约, 不给地址它不发 RTSP
    log "没探到 192.168.49.1 (Android), 按 Windows/标准 GO 走 DHCP..."
    ip addr flush dev "$_if" 2>/dev/null || true
    _go=$(dhcp_acquire "$_if" 2>/dev/null || true)
    if [ -n "$_go" ]; then
        SINK_GO_IP="$_go"
        log "✅ DHCP 完成: 本端 $(iface_ip "$_if") / GO=$SINK_GO_IP"
        return 0
    fi

    # ③ 兜底: Windows ICS 固定段静态试一次
    ip addr add 192.168.137.128/24 dev "$_if" 2>/dev/null || true
    if ping -c1 -W1 -I "$_if" 192.168.137.1 >/dev/null 2>&1; then
        SINK_GO_IP=192.168.137.1
        log "⚠ 没拿到租约, 静态 192.168.137.128/24 探到 GO=192.168.137.1"
        return 0
    fi

    SINK_GO_IP=192.168.49.1
    log "⚠ 既没探到安卓 GO 也没拿到 DHCP 租约, 按 $SINK_GO_IP 兜底 (大概率连不上, 看下面日志)"
    return 0
}

stop_sink_client() {
    if [ -f "$SINK_PIPE_PID" ]; then
        _pg=$(cat "$SINK_PIPE_PID" 2>/dev/null || echo "")
        [ -n "$_pg" ] && kill -9 -- "-$_pg" 2>/dev/null || true
        rm -f "$SINK_PIPE_PID"
    fi
    # K230: setsid sh -c 的 while 循环会不断重起 wfd-sink, 必须按命令行 SIGKILL
    pkill -9 -f "/usr/bin/wfd-sink" 2>/dev/null || true
    for _b in wfd-sink v4l2_dec drmplay; do
        pkill -9 -x "$_b" 2>/dev/null || true
    done
    sleep 1
}

start_sink_client() {
    stop_sink_client
    setsid sh -c '
        i=0
        while [ $i -lt 20 ]; do
            /usr/bin/wfd-sink -c '"$SINK_GO_IP"':'"$SINK_GO_PORT"' -d /dev/stdout 2>>/var/log/miracast.log | \
            /usr/bin/v4l2_dec /dev/stdin /dev/stdout -H 2>>/var/log/miracast-dec.log | \
            /usr/bin/drmplay -H -p nv12 -w 480 -h 800 -r 30 2>>/var/log/miracast-play.log
            i=$((i+1))
            sleep 2
        done' >/dev/null 2>&1 &
    echo $! > "$SINK_PIPE_PID"
    log "▶ sink 客户端模式已启动: rtsp://$SINK_GO_IP:$SINK_GO_PORT → v4l2_dec → drmplay (pid $(cat "$SINK_PIPE_PID"))"
}

start_client_watcher() {
    [ -f "$P2P_LOG" ] || return 0
    ( tail -n 0 -F "$P2P_LOG" 2>/dev/null | while IFS= read -r line; do
        case "$line" in
            *P2P-PROV-DISC-PBC-REQ*)
                # client 模式下我们是 WPS **enrollee**, PD/PBC 应答由 supplicant 自己发,
                # 不需要(也不应该)调 wps_pbc (那是 GO/registrar 侧的动作)
                log "📲 手机请求 PBC (PD 已由 supplicant 自动应答)" ;;
            *P2P-GO-NEG-REQUEST*)
                log "🤝 收到 GO Negotiation (我们 intent=0 ⇒ 手机当 GO)" ;;
            *P2P-INVITATION-RECEIVED*)
                # Windows 记住上一轮的 PERSISTENT 组后, 再点一次不会走 GO Negotiation,
                # 而是直接发 Invitation 邀请我们回到那个组 —— 此时**必须由前端应答**
                # (supplicant 只抛事件, 不会自动加入), 否则对端超时 ⇒ "无法连接"。
                # 2026-10-25 11:09 实测: PC 发 Invitation(persistent=4) 后我们只打了一行日志,
                # 组没建起来, PC 报无法连接。
                _inv_sa=$(printf '%s\n' "$line" | sed -n 's/.*[ ]sa=\([0-9a-fA-F:]*\).*/\1/p')
                _inv_pg=$(printf '%s\n' "$line" | sed -n 's/.*persistent=\([0-9]*\).*/\1/p')
                # ⚠ 2026-10-25 实测教训: 事件里若不是已知持久组 (行尾带 unknown-network),
                # **绝不能**回 p2p_connect —— 那会发 GO Negotiation Request, 而对端此刻
                # 正等 Invitation Response, 于是直接回 status 5 拒绝 (12:30:01 现场),
                # 而且 supplicant 此后停在 IDLE 不再 listen ⇒ 越点越连不上。
                # 已知组交给 persistent_reconnect=1 自动接受; 这里只负责兜底重挂 listen。
                log "📥 收到 Invitation: sa=${_inv_sa:-?} persistent=${_inv_pg:-?}"
                if [ -n "$_inv_pg" ] && [ -n "$_inv_sa" ]; then
                    wpa p2p_connect "$_inv_sa" pbc persistent="$_inv_pg" >/dev/null 2>&1 \
                        || wpa p2p_listen >/dev/null 2>&1 || true
                else
                    log "   (未知持久组: 不动 supplicant, 等对端回落到全新协商)"
                    wpa p2p_listen >/dev/null 2>&1 || true
                fi ;;
            *P2P-GO-NEG-FAILURE*)
                # 协商被拒/超时后 supplicant 会停在 IDLE, 必须重挂 listen,
                # 否则板子从此"消失", 对端删设备重连也找不到 (12:30 现场)。
                log "⚠ 协商失败, 重挂 listen"
                sleep 1
                wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || true ;;
            *P2P-GROUP-STARTED*)
                _gif=$(printf '%s\n' "$line" | awk '{for(i=1;i<=NF;i++) if($i=="P2P-GROUP-STARTED"){print $(i+1); exit}}')
                _role=$(printf '%s\n' "$line" | awk '{for(i=1;i<=NF;i++) if($i=="P2P-GROUP-STARTED"){print $(i+2); exit}}')
                log "✅ 组已建立: iface=$_gif role=$_role"
                if [ "$_role" = "client" ]; then
                    prepare_client_net "$_gif"
                    log "   wfd-sink 主动连 rtsp://$SINK_GO_IP:$SINK_GO_PORT (GO=手机或 PC, 由上面判定)"
                    sleep 1
                    start_sink_client
                    log "   看 sink 日志: tail -f /var/log/miracast.log"
                else
                    log "⚠ 本端成了 GO (role=$_role); Windows 源码一般自己当 GO —— 若对方是 PC 请查 p2p_go_intent 是否 0"
                fi ;;
            *P2P-GROUP-REMOVED*)
                log "⚠ 组已移除: $(printf '%s' "$line" | cut -c1-80)"
                stop_sink_client
                ip addr flush dev "${CLIENT_IF:-$IFACE}" 2>/dev/null || true
                SINK_GO_IP=""
                wpa p2p_group_remove "${CLIENT_IF:-$IFACE}" >/dev/null 2>&1 || true
                sleep 1
                wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || true
                log "♻ 已清理(进程+IP)并重挂 listen, 恢复可被发现" ;;
            *P2P-GROUP-FORMATION-FAILURE*)
                # 组没建成 (对端取消/超时/信道不合): 必须重新挂 listen,
                # 否则 supplicant 停在 IDLE, 对端再点就"扫不到/无法连接"。
                log "⚠ 建组失败, 重挂 listen (可让对端再试一次)"
                stop_sink_client
                ip addr flush dev "${CLIENT_IF:-$IFACE}" 2>/dev/null || true
                SINK_GO_IP=""
                sleep 1
                wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || true ;;
            *CTRL-EVENT-DISCONNECTED*)
                log "⚠ 与 GO 断开: $(printf '%s' "$line" | cut -c1-70)" ;;
            *P2P-FIND-STOPPED*|*P2P-LISTEN-STOPPED*)
                wpa p2p_listen >/dev/null 2>&1 || true ;;
        esac
    done ) &
    echo $! > "$CLIENT_WATCH_PID"
    log "client 事件监听已启动 (pid $(cat "$CLIENT_WATCH_PID" 2>/dev/null))"
}

cmd_client() {
    need_root
    backup_conf
    write_conf client
    stop_dhcpd            # client 模式不用 DHCP 服务端 (参考实现也是静态地址)
    # 监听模式的 pipeline 由 miracast.service 保活 (Restart=always), 必须显式停掉,
    # 否则它会把解码器/DRM 占住, 我们客户端模式起不来 (2026-10-21)。
    # K230 2026-10-24: 开机自启已改为本脚本(client)本身, 不再停 miracast.service (否则自杀)
    ip addr flush dev "$IFACE" 2>/dev/null || true
    rm -f /tmp/wfd-p2p.done /tmp/wfd-p2p.failed
    start_supplicant

    log "设置 WFD 设备信息: $DEVINFO"
    wpa wfd_subelem_set 0 "$DEVINFO" | grep -q OK || die "WFD_SUBELEM_SET 失败"
    log "回读: $(wpa wfd_subelem_get 0 | tail -1)"
    # K230 2026-10-24: 补 WFD 子元素 1/6 (Associated BSSID / Coupled Sink Info),
    # 让 WFD IE 从 13B 变 32B, 与海奇(商用接收器)一致 —— 部分手机(K80/小米新机型)
    # 的标准 P2P 发现需要完整 WFD IE 才显示 sink
    wpa wfd_subelem_set 1 "$WFD_SUBELEM_ASSOC_BSSID" >/dev/null 2>&1 || true
    wpa wfd_subelem_set 6 "$WFD_SUBELEM_COUPLED_SINK" >/dev/null 2>&1 || true
    wpa p2p_set ssid_postfix "$SSID_POSTFIX" >/dev/null 2>&1 || true

    # 只做监听, **不建组**: 等手机/PC 发起 GO Negotiation / Invitation。
    # 2026-10-25: 用 p2p_listen 而不是 p2p_find —— 实测 p2p_find 是「扫 0.7 s →
    # 听 0.3 s」的循环, sink 只在 30% 时间停在 ch6, PC/手机经常扫不到(表现为
    # "很久才连上"或"直接连不上"); listen 是 100% 驻留, 谁要连我们就发过来。
    wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || die "p2p_listen 失败, 看 $P2P_LOG"
    start_client_watcher

    log "✅ 已进入 client 模式: P2P device (不建组), p2p_go_intent=0 ⇒ 对端(手机/PC)当 GO"
    log "现在用: 手机「无线投屏」/ Windows 10 「Win+K → 连接到无线显示器」里点 $DEVICE_NAME"
    log "  · 安卓 GO=192.168.49.1 → 静态 $CLIENT_IP; Windows GO=192.168.137.1 → DHCP 拿租约"
    log "  · Windows 侧注意: 只留一块支持无线显示器的网卡 (netsh wlan show drivers 有那一行为 是)"
    log "成功后应看到: grep -E 'P2P-GROUP-STARTED|✅ GO=|✅ DHCP' $P2P_LOG"
    # 驻留: 作为 systemd service 时进程不能退 (Type=simple + KillMode=control-group,
    # 一退就整组被杀 ⇒ 监听器/ supplicant 全没, 服务每 3 s 重启一次空转)。
    log "（前台驻留, Ctrl+C 或 $0 stop 结束）"
    wait
}

cmd_stop() {
    need_root
    stop_pbc_watcher
    stop_sink_client
    if [ -f "$CLIENT_WATCH_PID" ]; then
        kill "$(cat "$CLIENT_WATCH_PID" 2>/dev/null || echo 0)" 2>/dev/null || true
        rm -f "$CLIENT_WATCH_PID"
    fi
    ip addr del "$CLIENT_IP/24" dev "$IFACE" 2>/dev/null || true
    stop_dhcpd
    wpa p2p_group_remove "$IFACE" >/dev/null 2>&1 || true
    stop_supplicant
    if [ -f /tmp/wfd-p2p-backup.conf ]; then
        for c in "/etc/wpa_supplicant/wpa_supplicant-$IFACE.conf" \
                 "/etc/wpa_supplicant/wpa_supplicant.conf"; do
            if [ -f "$c" ]; then cp -a /tmp/wfd-p2p-backup.conf "$c"; log "已恢复 $c"; break; fi
        done
    fi
    # 只在 GO 用的是 station 网卡 (回退模式) 时才恢复 systemd 的 station supplicant
    if [ "$IFACE" = "$STATION_IFACE" ]; then
        systemctl unmask --runtime "wpa_supplicant@$IFACE" wpa_supplicant 2>/dev/null || true
        systemctl start "wpa_supplicant@$IFACE" 2>/dev/null || systemctl start wpa_supplicant 2>/dev/null || true
        sleep 3
    else
        # 独立 P2P 网卡: 把 GO 地址摘掉就行, wlan0 全程没被动过
        ip addr del "$GO_IP/24" dev "$IFACE" 2>/dev/null || true
        ip link set dev "$IFACE" down 2>/dev/null || true
    fi
    systemctl start miracast.service 2>/dev/null || true
    log "已停 P2P; station($STATION_IFACE) IP: $(ip -4 addr show "$STATION_IFACE" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)"
}

cmd_status() {
    log "iface=$IFACE  conf=$P2P_CONF"
    [ -S "$CTRL_DIR/$IFACE" ] || { log "supplicant ctrl socket 不在"; exit 1; }
    wpa status | head -12
    log "--- WFD 子元素回读 ---"
    wpa wfd_subelem_get 0 | tail -1
    log "--- GO / 客户端事件 (日志尾) ---"
    grep -E "P2P-GROUP-STARTED|AP-STA-CONNECTED|P2P-DEVICE-FOUND|WFD:" "$P2P_LOG" 2>/dev/null | tail -12 || log "(无事件)"
}

cmd_devinfo() {
    need_root
    [ -n "${1:-}" ] || die "用法: $0 devinfo <hex, 如 00111C440000>"
    wpa wfd_subelem_set 0 "$1" | grep -q OK || die "设置失败"
    log "已设 WFD 设备信息 = $1 (回读 $(wpa wfd_subelem_get 0 | tail -1))"
    log "注意: 改了要重新 p2p_group_add 才会进 beacon (start 会自动做)"
}

usage() {
    sed -n '3,50p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-}" in
    start)   shift; cmd_start "$@" ;;
    listen)  cmd_listen ;;
    stop)    cmd_stop ;;
    client)  cmd_client ;;
    status)  cmd_status ;;
    devinfo) shift; cmd_devinfo "$@" ;;
    *)       usage; exit 1 ;;
esac
