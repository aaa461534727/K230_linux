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
#   wfd-p2p.sh stop                              停 GO + 置 hold (开机自启不再抢设备)
#   wfd-p2p.sh resume                            清 hold 并重新起 GO (= 解除调试独占)
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
# 帧率口径 (2026-10-26 晚 主人: 「先保证有一版能使用先」):
#   默认 = 30fps 基线 —— 只广告 720p30 (CEA bit5), native/preferred 留 00 00,
#   与 17:43 真机跑通那版 (3076 帧 / 102.5 s = 30.0 fps, RESULT: PASS) 完全一致。
#   历史: 1080p 在单核 + 2.4 GHz 上扛不住 (峰值 ~1 MB/s, 5 s 丢 194~653 包 = 花屏),
#   720p 只要 1/2.25 的像素, 所以默认档一直是 720p。
#   WFD 没有单独帧率字段, 帧率完全取决于广告的 CEA 位: bit5=720p30, bit6=720p60。
#   要 60fps (验证中, 未验收): WFD_CEA=0x40 sh wfd-p2p.sh start
#   要 1080p30:              WFD_CEA=0x80 sh wfd-p2p.sh start
#   要 720p 双档:            WFD_CEA=0x60 sh wfd-p2p.sh start
WFD_LOWLAT="${WFD_LOWLAT:-0}"; export WFD_LOWLAT
PIDFILE="${WFD_PIDFILE:-/run/wfd-p2p.pid}"
# 2026-10-26 自启与调试的隔离阀: 有本文件时自启**空跑退出**, 不会去抢 wlan1 / 7236。
#   · stop 子命令默认置位 (调试时停一次就不会被 systemd 再拉起来)
#   · resume 子命令 / systemctl start k230-miracast 清除并启动
#   · 调试脚本要绕过: WFD_FORCE=1 sh wfd-p2p.sh start
HOLD_FILE="${WFD_HOLD_FILE:-/run/wfd-p2p.hold}"
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
# GO 模式(本机当 GO)下的收流 watcher pid: 等 source(手机) 关联 + 拿租约后再起 pipeline
GO_WATCH_PID="${WFD_GO_PIDFILE:-/run/wfd-go-client-watch.pid}"
GO_SUP_PID="${WFD_GO_SUP_PIDFILE:-/run/wfd-go-supervisor.pid}"
GO_IF=""                                  # GO 模式实际承载组的接口 (cmd_start 里填)
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
# 2026-10-26: 默认改回 ch1 (2412)。依据是现场铁证 —— K80 的 P2P Listen Channel
#   实测 = ch1 (Op Class 81), 只在 ch1 听; 我们 GO 若起在 ch11, 它要撞运气才看得到
#   ("找到一次, 刷新又没了")。之前靠 shell 里手敲 WFD_GO_FREQ=2412 才凑效, 手敲一丢
#   就又回到 ch11 ⇒ 现在把 2412 写进默认值, 裸 start 就是已知可用配置。
GO_FREQ="${WFD_GO_FREQ:-2412}"           # 2.4G ch1 (对齐 K80 的 listen channel)
# listen 信道必须跟组信道一致 (手机把 GO-Neg/PD 发到它看到的 listen 信道)。
# 由 GO_FREQ 反推: 2412→1, 2437→6, 2462→11; 也可用 WFD_LISTEN_CHANNEL 直接指定。
case "$GO_FREQ" in
    2412) _def_ch=1 ;;
    2437) _def_ch=6 ;;
    2462) _def_ch=11 ;;
    *)    _def_ch=6 ;;
esac
LISTEN_CHANNEL="${WFD_LISTEN_CHANNEL:-$_def_ch}"
# 2026-10-28: 对端持久组自动重连开关。
#   0 (默认) = 拒绝对端 Invitation, 每次走全新 GO Negotiation
#   1        = 老行为 (Windows 依赖它), 按已存持久组自动回 Invitation Response
PERSIST_RECONNECT="${WFD_PERSISTENT_RECONNECT:-0}"
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
    # 2026-10-25 再修一层: /tmp 是 tmpfs, **断电/重启就没了** ⇒ 对端的持久组凭据
    # 必须存一份到 /root, 起动时先恢复, 否则手机拿旧组发 Invitation 我们回 status 8
    # (unknown group), 它就退回全新协商并把我们踢出组 (实测 18:30 轮)。
    P2P_CONF_BAK=/root/wfd-p2p-persistent.conf
    if [ ! -f "$P2P_CONF" ] && [ -f "$P2P_CONF_BAK" ]; then
        cp -f "$P2P_CONF_BAK" "$P2P_CONF" 2>/dev/null || true
        log "已从 $P2P_CONF_BAK 恢复持久组凭据"
    fi
    _keep_nets=""
    if [ -f "$P2P_CONF" ]; then
        # 2026-10-27 现场定因 (K80 首次点投屏 70% 失败):
        #   持久组块里存着 bssid=<对端 P2P **设备**地址> (da:85:76:6f:f8:2d)。
        #   手机每次建组都换一个随机组接口 MAC (实测 ...f8:1d / ...f8:ce / ...f8:01),
        #   把 bssid 钉死 ⇒ 重新入组时按旧地址关联, 空口上那个 MAC 根本不存在
        #   ⇒ ASSOCIATING 干等 2~3 s 后 reason 1 (UNSPECIFIED), 连试 3 次
        #   ⇒ P2P-GROUP-FORMATION-FAILURE (就是"连好几次才成功"的那 30%)。
        #   修法: 保留持久组凭据 (ssid/psk, 否则对端 Invitation 会变 unknown-group),
        #   但**丢掉 bssid= 那一行**, 让 supplicant 每次按 SSID 扫到当前真实的
        #   GO 组接口地址再关联。
        _keep_nets=$(awk '/^network=\{/{f=1; buf=""} f{if ($0 !~ /^[ \t]*bssid=/) buf=buf $0 "\n"} f&&/^\}/ {f=0; if (buf ~ /psk=/ || buf ~ /key_mgmt=/) printf "%s", buf}' "$P2P_CONF" 2>/dev/null)
        # 2026-10-28 修「越用越乱 + 角色反转」:
        #   上面这条会把**我们自己那个 GO 持久组块** (ssid=$GO_SSID) 也一起收进来,
        #   每次重启又 append 回去 ⇒ /tmp/wfd-p2p.conf 里 network 块**指数增长**
        #   (现场实测一次累积到 35 个)。后果不是慢一点, 是启动时 supplicant 把
        #   这些组当"已存在的持久组"全部拉活, 我们**抢成 GO** (role=GO),
        #   而手机当 source 时必须是 GO ⇒ 它怎么也连不上, 只能弹回"连接"。
        #   修法: ① 剔除 ssid 等于我们自己的那些块; ② 只留最近 4 块 (对端每次
        #   建组会换随机 GROUP SSID, 不设上限同样会无限长)。
        _keep_nets=$(printf '%s\n' "$_keep_nets" | awk -v own="$GO_SSID" '
            /^network=\{/ { n++; blk[n] = "" }
            { if (n > 0) blk[n] = blk[n] $0 "\n" }
            END {
                s = (n > 4) ? n - 3 : 1;
                for (i = s; i <= n; i++)
                    if (blk[i] !~ ("ssid=\"" own "\"")) printf "%s", blk[i];
            }' 2>/dev/null)
        if [ -n "$_keep_nets" ]; then
            _kn=$(printf '%s\n' "$_keep_nets" | grep -c '^network={' 2>/dev/null || true)
            log "保留 [$_kn] 个持久组凭据 (已去掉 bssid 引脚, 重连时自己找当前组地址)"
        fi
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

# 2026-10-28: 默认**关闭**对端持久组自动重连 (WFD_PERSISTENT_RECONNECT=1 恢复老行为)。
#   实测 K80 (MTK): 再点投屏时用存下来的持久组发 Invitation, 组直接起在 ch8/2447
#   (或它 STA 的 5G), 既绕过 Operating Channel 钉死, 又出现 "合法 AUTH 一帧都不
#   ACK" ⇒ 怎么点都连不上 (09:47:11 现场)。关掉后 Invitation 由前端拒绝, 手机
#   回落全新 GO Negotiation ⇒ 走钉死的 ch1 ⇒ 一次连上。
persistent_reconnect=$PERSIST_RECONNECT

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
    # 2026-10-28: 只在 PERSIST_RECONNECT=1 时还原; 默认丢掉, 强制全新协商。
    if [ -n "$_keep_nets" ] && [ "$PERSIST_RECONNECT" = "1" ]; then
        printf '%s\n' "$_keep_nets" >> "$P2P_CONF"
        log "已还原 $(printf '%s\n' "$_keep_nets" | grep -c '^network={') 个对端持久组"
    elif [ -n "$_keep_nets" ]; then
        log "已丢弃对端持久组 (PERSIST_RECONNECT=0 ⇒ 每次全新协商, 避开旧组信道)"
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
    cp -f "$P2P_CONF" /root/wfd-p2p-persistent.conf 2>/dev/null || true
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
    # 2026-10-26 实测: 同一块板反复 `start` 会**叠加**出多个 wpa_supplicant
    # (现场攒到 5 个: 每个都 `-dd` 往同一份 /tmp/wfd-p2p.log 写, 每个都还占着
    #  /run/wpa_supplicant/wlan1 这个路径; 新实例 bind 失败 → 脚本把 wlan1 down/up
    #  一次才挤进去 ⇒ 每次重起都抖一下)。所以起新实例前先按"命令行里带我们这份
    #  P2P 配置"精准清场 (不会碰 wlan0 station 的 supplicant)。
    for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
        grep -qa "wfd-p2p.conf" "/proc/$_p/cmdline" 2>/dev/null && kill -9 "$_p" 2>/dev/null
    done
    _i=0
    while [ "$_i" -lt 10 ]; do
        _left=0
        for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
            grep -qa "wfd-p2p.conf" "/proc/$_p/cmdline" 2>/dev/null && _left=1
        done
        [ "$_left" = 0 ] && break
        _i=$((_i + 1)); sleep 0.2
    done
    rm -f "$CTRL_DIR/$IFACE" "$CTRL_DIR/p2p-dev-$IFACE" 2>/dev/null || true
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
    # 自启与调试隔离 (2026-10-26): 调试时执行过 stop 就会留下 HOLD_FILE,
    #   自启看到它**空跑退出**, 绝不抢 wlan1 / 7236 / DRM。要恢复: 本脚本 resume。
    if [ -f "$HOLD_FILE" ] && [ "${WFD_FORCE:-0}" != "1" ]; then
        log "⏸ 存在 $HOLD_FILE (上次 stop 或调试独占) —— 本次不启动。解除: sh $0 resume"
        exit 0
    fi
    # 2026-10-26 实测: 本板 net.core.rmem_max 只有 212992 (208 KB), 而 1080p30 的
    #   WFD 码流峰值 ~1 MB/s ⇒ 收包缓冲只够 200 ms, 手机一突发就丢包 (现场
    #   wfd-sink 统计 lost 5 s 内 0→194→653)。wfd_sink 里虽然请求了 4 MB,
    #   但被 rmem_max 截断。这里先把上限抬高, 让 SO_RCVBUF 真能生效。
    sysctl -w net.core.rmem_max=4194304 >/dev/null 2>&1 ||
        echo 4194304 > /proc/sys/net/core/rmem_max 2>/dev/null || true
    # 2026-10-26 紫屏根治: 上屏进程不再跟着管线起落, 而是**从开机就常驻**。
    #   早一点起来 = 少一段"没人持有 CRTC"的空白 (= 紫屏)。这里先清掉上一轮的残留
    #   (孤儿占着 DRM master 会让新的 SETCRTC 报 Permission denied), 再立刻起播放器。
    # 2026-10-26 晚: 屏由 k230-screen.service 独占 (它自己 Restart=always)。只有当它
    # 没在跑 (单跑本脚本、或服务被禁) 时才由本脚本兜底起一个播放器 —— 否则在这里
    # pkill 会连它一起杀掉, 立刻露一次紫屏。
    if ! systemctl is-active --quiet k230-screen.service 2>/dev/null; then
        pkill -9 -x drmplay 2>/dev/null || true
        sleep 0.2
    fi
    select_player
    start_player
    backup_conf
    write_conf
    # ⚠ GO 模式下 wlan1 + /tmp/wfd-p2p.conf 必须由本脚本独占: miracast.service 跑的
    #   是 client 模式 (同一张网卡 / 同一份 conf), 两个 supplicant 会互相 kill ⇒
    #   默认先停服务; 需要并存时显式 WFD_WITH_SERVICE=1。
    #   (2026-10-26 实测: stop 结尾会拉起服务, 手动起 GO 后服务把 supplicant 抢回去,
    #    表现为 GO 组消失 / /tmp/wfd-p2p.log 被重写)
    if [ "${WFD_WITH_SERVICE:-0}" = "1" ]; then
        systemctl start miracast.service 2>/dev/null || true
    else
        systemctl stop miracast.service 2>/dev/null || true
    fi
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

    # 4.6) GO 模式收流 (2026-10-26): source 关联进来 + 拿到租约后立刻起 RTSP pipeline,
    #      否则手机把画面推到 192.168.49.1:7236 时无人应答 ⇒ 表现为「连不上」。
    GO_IF="${go_if:-$IFACE}"
    stop_go_watchers
    start_go_sink_watch

    # 4.7) 先起「纯监听」pipeline: 手机一关联上来就有人应答 :7236
    #      (等它拿到租约后 watcher 会切成双模 `-c <手机ip>:7236`)。
    SINK_GO_IP=""
    start_sink_client

    # 4.8) 看护: pipeline 重试用尽 / 异常退出 时自动补起 (手机还连着但没有 7236 的场景)
    start_go_supervisor

    # 5) 证据: 日志里必须能找到 "WFD IE" 构建记录 (说明 beacon 会带)
    if grep -q "WFD: Update WFD IE" "$P2P_LOG"; then
        log "✅ 日志有 'WFD: Update WFD IE' (WFD IE 已生效)"
        log "实际下发的 WFD IE: $(grep 'WFD: WFD IE for Probe Response' "$P2P_LOG" | tail -1 | sed 's/.*hexdump(len=[0-9]*): //')"
    else
        log "⚠ 日志没看到 'WFD: Update WFD IE', 下面贴相关行:"
        grep -i "wfd" "$P2P_LOG" | tail -10 || true
    fi

    # 5.5) 单实例自检 (2026-10-26): 只允许 1 份 P2P supplicant + 1 组 pbc-watcher
    #      (以前每次 start 都叠一版, 攒到 5 份 supplicant / 15 个 watcher 进程)
    _ns=0
    for _p in $(pgrep -x wpa_supplicant 2>/dev/null); do
        grep -qa "wfd-p2p.conf" "/proc/$_p/cmdline" 2>/dev/null && _ns=$((_ns + 1))
    done
    _np=$(pgrep -f "wfd-pbc-watcher.sh" 2>/dev/null | wc -l)
    if [ "$_ns" -eq 1 ] && [ "$_np" -le 3 ]; then
        log "✅ 单实例自检: P2P supplicant ${_ns} 份 / pbc-watcher ${_np} 个进程 (1 主 + 2 子 = 正常)"
    else
        log "⚠ 单实例自检不过: supplicant ${_ns} 份 (期望 1) / pbc-watcher ${_np} 个 (期望 ≤3), 看 ps"
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
    # 2026-10-26: 以前只在 cmd_stop 里清 watcher, 反复 `start` 会一版叠一版
    # (现场攒到 5 组 / 15 个进程, 老的还在 tail 同一份日志、对同一事件重复
    #  发 p2p_connect/让位 ⇒ GO 抖动、手机"点几次才成")。起新的前先清场。
    stop_pbc_watcher
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
    # 2026-10-26 实测: 小米/红米每次投屏都换一个随机 MAC (现场 15 分钟内换了
    #   8 个: f8:73/f8:d5/f8:ad/f8:36/f8:69/f8:28/de:f0../ae:82..), 而租约
    #   默认 3600s 且池只有 16 个 ⇒ 一小时内点十几次就「池满, 拒绝」, 手机拿
    #   不到 IP, 表现正是「有时候投得上、有时候投不上」。改成 64 个池 + 300s
    #   短租约 (活动会话会自己续租, 不活动的一批 5 分钟就回收)。
    setsid nohup "$bin" -i "$dev" -a "$GO_IP" -p "$DHCP_POOL" -n 64 -t 300 -v \
        >> "$DHCP_LOG" 2>&1 &
    sleep 1
    if pgrep -x dhcpd >/dev/null 2>&1; then
        log "✅ dhcpd 已起: 池 $DHCP_POOL +64, 租约 300s (/24), 网关/DNS $GO_IP, 日志 $DHCP_LOG"
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
BLANK_PID=/tmp/drmplay-blank.pid
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

# 2026-10-27 现场: 8189fs 默认 Power save: on + rtw_smart_ps=2。
#   省电模式下本端在 DTIM 之间睡, P2P GO(手机) 只能把下行帧攒到信标再发, 接收侧看到的
#   是「一坨一坨」⇒ 帧间隔忽大忽小、源端还会据此降码率/降帧 (实测同一台手机 13:17 能出
#   59.8 fps, 13:28 只出 31~46 fps 且 lost=0 / cc_err=0, 说明不是丢包而是对端主动降)。
#   Miracast 接收端对实时性要求最高, 省电必须关。iw 设置会被重新关联清掉, 所以每次
#   起管线 / 进 client 模式都补一次 (幂等, 失败不影响投屏)。
ps_off() {
    for _psif in $(iw dev 2>/dev/null | awk '/Interface/{print $2}'); do
        # 只在真的是 on 时才动 (幂等: 反复 set 会让驱动重算 ps 状态)
        if iw dev "$_psif" get power_save 2>/dev/null | grep -q 'on'; then
            iw dev "$_psif" set power_save off >/dev/null 2>&1 && \
                log "省电已关: $_psif (DTIM 攒帧会让对端降码率/降帧 ⇒ 卡顿)"
        fi
    done
    # 驱动侧三道闸门全关: smart_ps(省电等级) / power_mgnt(PS 模式 2=最省) / lps_chk_by_tp
    #   smart_ps 与 power_mgnt 都是可写的模块参数, 写完立即对后续收发生效 (无需重新加载驱动)。
    for _psp in rtw_smart_ps rtw_power_mgnt; do
        _f=/sys/module/8189fs/parameters/$_psp
        [ -w "$_f" ] && [ "$(cat "$_f" 2>/dev/null)" != "0" ] && \
            echo 0 > "$_f" 2>/dev/null && log "省电已关: $_psp=0"
    done
    return 0
}
stop_sink_client() {
    if [ -f "$SINK_PIPE_PID" ]; then
        _pg=$(cat "$SINK_PIPE_PID" 2>/dev/null || echo "")
        # 2026-10-26: dash 的 kill 不支持 `--` 分隔符 (实测报 `Illegal number: -`
        # 且**什么都没杀**), 必须写成 kill -9 -<PGID> 才真按进程组杀。
        [ -n "$_pg" ] && kill -9 "-$_pg" 2>/dev/null || true
        rm -f "$SINK_PIPE_PID"
    fi
    # K230: setsid sh -c 的 while 循环会不断重起 wfd-sink, 必须按命令行 SIGKILL
    pkill -9 -f "/usr/bin/wfd-sink" 2>/dev/null || true
    # 2026-10-26: **不要杀 drmplay** —— 它是常驻播放器 (读命名管道, 空闲刷黑),
    #   杀了就有一段没人持有 CRTC 的空白 ⇒ 面板扫未定义显存 = 全屏紫色。
    #   只有 vgplay (老的 GPU 路径, 仍在管线里) 需要跟着管线走。
    for _b in wfd-sink vgplay; do
        pkill -9 -x "$_b" 2>/dev/null || true
    done
    # 2026-10-26 (互斥实测): **v4l2_dec 不能按名字杀** —— 板上同时有两个:
    #   · 安卓这条读 /dev/stdin   (wfd-sink | v4l2_dec | 常驻播放器)
    #   · 苹果这条读 /run/airplay.h264 (k230-airplay 的看护循环)
    #   按名字 pkill 会把苹果的解码器一起打死 (airplay.log 里每切一次就一条
    #   `硬解退出 (rc=137)`); 若那一刀正好落在 iPhone 首帧之前, 解码器重启后
    #   从流中间接上、丢了 SPS ⇒ 「投上了但黑屏」。所以只精确杀安卓这条。
    pkill -9 -f "v4l2_dec /dev/stdin" 2>/dev/null || true
    sleep 1
}

# ---- 常驻播放器 (2026-10-26 去紫屏) --------------------------------------
# 一个进程从开机持有 DRM master 到关机: drmplay -F <fifo>, 管道里没数据就刷黑。
# 好处: ① 任何时候屏上都有 plane (无紫) ② 管线重启不再抢 DRM master
#      ③ 会话切换零空档。调试要独占屏就直接 kill 它 (自启看护会补起)。
PLAYER_FIFO="${WFD_PLAYER_FIFO:-/run/wfd-player.fifo}"
PLAYER_PIDFILE="${WFD_PLAYER_PID:-/run/wfd-player.pid}"
PLAYER_FIFO_MODE=0

start_player() {
    [ -x "$PLAYER_BIN" ] || return 0
    if [ "$PLAYER_BIN" != /usr/bin/drmplay ]; then
        # vgplay 不支持 -F 常驻, 退回老的「管线里当最后一段」
        PLAYER_FIFO_MODE=0
        return 0
    fi
    # ★ 2026-10-27: 体检判据 = pidfile 活 + fd 0 指向**当前**管道 + 管道在。
    #   老写法只看「pid 活着」就复用, 于是玩家拿着 (deleted) 旧 inode 也判"健康"
    #   ⇒ 解码器 fopen 永久阻塞、屏上全黑 (现场 16:00)。
    player_ok() {
        _pp=$(cat "$PLAYER_PIDFILE" 2>/dev/null || echo 0)
        if [ -z "$_pp" ] || [ "$_pp" = 0 ] || ! kill -0 "$_pp" 2>/dev/null; then
            _pp=$(pgrep -f "drmplay .*-F $PLAYER_FIFO" 2>/dev/null | head -1)
        fi
        [ -n "$_pp" ] || return 1
        [ -p "$PLAYER_FIFO" ] || return 1
        _tgt=$(readlink "/proc/$_pp/fd/0" 2>/dev/null || echo "")
        case "$_tgt" in
            *"(deleted)") return 1 ;;
            *"$PLAYER_FIFO") return 0 ;;
            *) return 1 ;;
        esac
    }
    if player_ok; then
        PLAYER_FIFO_MODE=1
        return 0            # 已经在跑且管道正常, 别重启 (重启就是一次紫屏)
    fi
    # 不健康 / 没有玩家 ⇒ 交给唯一所有者 k230-screen.service 重建 (它先 pkill 再 mkfifo)
    if systemctl is-enabled --quiet k230-screen.service 2>/dev/null ||
       systemctl is-active --quiet k230-screen.service 2>/dev/null; then
        log "⚠ 常驻播放器不健康 → 重启 k230-screen 重新接管屏"
        systemctl restart k230-screen.service 2>/dev/null || true
        sleep 1.5
        if player_ok; then
            PLAYER_FIFO_MODE=1
            log "✅ 播放器已重新接管 (pid $(cat "$PLAYER_PIDFILE" 2>/dev/null))"
            return 0
        fi
        log "⚠ 重启 k230-screen 后仍不健康 (黑屏风险): 看 /var/log/k230-screen.log"
    fi
    # 兜底 (单跑本脚本 / k230-screen 被禁): 本地起一个。
    #   注意 **不 rm 管道** —— 只补建缺失的; 删 inode 正是黑屏的根源。
    stop_blank
    [ -p "$PLAYER_FIFO" ] || { mkfifo "$PLAYER_FIFO" 2>/dev/null || true; }
    chmod 666 "$PLAYER_FIFO" 2>/dev/null || true
    setsid /usr/bin/drmplay $PLAYER_OPTS -F "$PLAYER_FIFO" \
        >>/var/log/miracast-play.log 2>&1 < /dev/null &
    echo $! > "$PLAYER_PIDFILE"
    PLAYER_FIFO_MODE=1
    sleep 0.4
    log "🖤 常驻播放器已起 (pid $(cat "$PLAYER_PIDFILE" 2>/dev/null), 管道 $PLAYER_FIFO) —— 空闲自动刷黑, 不再紫屏"
}

stop_player() {
    if [ -f "$PLAYER_PIDFILE" ]; then
        _pp=$(cat "$PLAYER_PIDFILE" 2>/dev/null || echo "")
        [ -n "$_pp" ] && kill -9 "$_pp" 2>/dev/null || true
        rm -f "$PLAYER_PIDFILE"
    fi
    pkill -9 -x drmplay 2>/dev/null || true
    rm -f "$PLAYER_FIFO"
    PLAYER_FIFO_MODE=0
    # ★ 2026-10-27: 管子被删掉后, 屏上就没有 plane 了 (紫)。把重建交回唯一所有者,
    #   否则删 inode 会让还活着的玩家拿着 (deleted) 管道 ⇒ 解码器卡死在 open()。
    if systemctl is-enabled --quiet k230-screen.service 2>/dev/null; then
        systemctl restart k230-screen.service 2>/dev/null || true
    fi
}

# 空闲占屏: 用 drmplay -B 把 CRTC 占成纯黑。
# 不占的话平面被拆掉 ⇒ 面板扫到未定义内容, 现场表现就是"全屏紫色跟投屏抢显示"。
stop_blank() {
    if [ -f "$BLANK_PID" ]; then
        _bp=$(cat "$BLANK_PID" 2>/dev/null || echo "")
        [ -n "$_bp" ] && kill -9 "-$_bp" 2>/dev/null || true
        [ -n "$_bp" ] && kill -9 "$_bp" 2>/dev/null || true
        rm -f "$BLANK_PID"
    fi
    pkill -9 -f "drmplay -B" 2>/dev/null || true
    pkill -9 -f "drmplay --blank" 2>/dev/null || true
}
start_blank() {
    [ -x /usr/bin/drmplay ] || return 0
    # 2026-10-26: 常驻播放器已经在占 CRTC 并会自己刷黑, 再去起一个 -B 只会抢
    #   DRM master (现场症状: SETCRTC Permission denied + 紫屏/黑屏抖动)。
    if pgrep -f "drmplay .*-F /run/wfd-player.fifo" >/dev/null 2>&1; then
        return 0
    fi
    stop_blank
    setsid /usr/bin/drmplay -B -w 480 -h 800 >>/var/log/miracast-blank.log 2>&1 < /dev/null &
    echo $! > "$BLANK_PID"
    sleep 0.3
}

# 上屏播放器: GPU (VGLite 硬件缩放) 优先, 自检不过或没装则回落 CPU (drmplay)
#   WFD_PLAYER=cpu  强制 CPU 缩放 (老路, 出问题时的保险)
#   WFD_PLAYER=gpu  强制 GPU (跳过自检, 调试用)
PLAYER_BIN=/usr/bin/drmplay
PLAYER_OPTS="-H -p nv12 -w 480 -h 800"
select_player() {
    PLAYER_BIN=/usr/bin/drmplay
    PLAYER_OPTS="-H -p nv12 -w 480 -h 800"
    case "${WFD_PLAYER:-auto}" in
        cpu) log "🎛 上屏: CPU 缩放 (drmplay, 强制)"; return 0 ;;
    esac
    # ⚠ 2026-10-25: GPU 路径实测会把板子跑死 (1280x720 缩放 blit), 所以**默认不开**;
    # 只有在板上跑完 scripts/board_hwscale_test.sh 全过并 touch /root/.vgplay-ok 才启用。
    if [ ! -x /usr/bin/vgplay ] || [ ! -f /root/.vgplay-ok ]; then
        log "🎛 上屏: CPU 缩放 (drmplay; GPU 路径未验证/未启用)"
        return 0
    fi
    if [ "${WFD_PLAYER:-auto}" = "gpu" ] || /usr/bin/vgplay --selftest >>/var/log/miracast-vg.log 2>&1; then
        PLAYER_BIN=/usr/bin/vgplay
        PLAYER_OPTS="-H -w 480 -h 800 -r 30"
        log "🎛 上屏: VGLite 硬件缩放 (GPU)"
    else
        log "⚠ VGLite 自检失败 (见 /var/log/miracast-vg.log) → 回落 CPU 缩放"
    fi
    return 0
}

start_sink_client() {
    # 只重启 wfd-sink / v4l2_dec; 播放器常驻 (start_player 幂等, 已在跑就不动)
    ps_off
    stop_sink_client
    select_player
    start_player
    if [ "$PLAYER_FIFO_MODE" != 1 ]; then
        stop_blank
    fi
    # 2026-10-26 实测定论 (dmesg 铁证): 本固件 v5/v7 H264 解码器**不支持**
    #   DEC_DST_CROPPING —— ioctl 返回成功, 固件随后回
    #   "set-opt h264dec: Unsupported option. (index=107)",
    #   结果: -c 被静默忽略, 而 -s 480x800 照常生效 ⇒ 整幅 1920x1080 (含左右
    #   黑边) 被横向压进 480 宽 ⇒ 手机内容只剩中间一条 (主人看到的"中间一点点")。
    #   所以管线只用「解码器出原分辨率 → drmplay 自己裁黑边+缩放」
    #   (drmplay 侧实测: 源 509x1080 @708,0 → 绘制区 377x800 @51,0, 画面正确)。
    #   ⇒ 解码器不再传 -L/-c/-s, 学习内容区那套机制保留在工具里备用, 不进管线。
    # SINK_GO_IP 为空 ⇒ 只监听 :7236 (GO 模式: source 会拨进来);
    # 非空 ⇒ 双模 (同时监听 + 主动拨对端:7236), wfd_sink.c 里 listener 无条件创建。
    if [ -n "$SINK_GO_IP" ]; then
        SINK_ARG="-c $SINK_GO_IP:$SINK_GO_PORT"
    else
        SINK_ARG=""
    fi
    # 2026-10-26 常驻播放器: 解码器直接写命名管道 (/run/wfd-player.fifo), 播放器那一侧
    #   一个进程吃到底 —— 管线怎么重试都不会出现"没人持有 CRTC"的空窗 (紫屏)。
    # 2026-10-27 取证开关 (默认关): touch /run/k230-es-dump ⇒ sink 输出的 H.264 ES 再存一份
    #   到 /run/k230-es.h264。用途: 离线逐帧比对「屏上内容不变」到底是手机编码器发的重复帧,
    #   还是本板解码器没把新画面送出去 —— 两种结论的修法完全相反, 别靠猜。
    #   代价: 多一个 tee 进程搬 2~4 Mbps (0.3~0.5 MB/s), 单核占用 <1%; 9 MB/30 s, /run 有 192 MB。
    TEE_SEG=""
    # ★ 2026-10-27 17:05 修: 原来 `tee <file>` **每次重开管线就截断** —— 会话一结束
    #   管线自愈重启, 37 MB 的现场 ES 当场变 0 字节, 白抓一次 (主人得多投一遍)。
    #   改用 `tee -a` 追加: 一次投屏全过程累积, 断流/重连都不丢。
    #   ⚠ 这里**不做清空** —— start_sink_client 会在每次会话事件里重入, 清一次就又丢了;
    #   要开新一轮取证自己 `rm -f /run/k230-es.h264`, 然后投屏。
    [ -f /run/k230-es-dump ] && TEE_SEG=' | /usr/bin/tee -a /run/k230-es.h264 2>/dev/null'
    # ★ 2026-10-28 清晰度根因修复 (两次试错后的结论, 别再走回头路):
    #   症状: 投屏文字糊成一团。
    #   根因: 原来 v4l2_dec 硬带 -A (autofit), 它把**整帧** contain 进 480x800
    #         ⇒ 1280x720 缩成 480x270。可是竖屏手机的画面在 1280x720 帧里只占
    #         中间 ~420x720 (两侧黑边), 缩完内容只剩 157x270, drmplay 再放大
    #         2.96 倍铺到 465x800 ⇒ 每个源像素铺 9 个屏幕像素, 必然糊。
    #         (不是 720p 的锅, 也不是码率: 帧里那 420px 已经够 465px 屏用。)
    #   试错①: v4l2_dec -c 裁内容区 + -s 480x800 —— 本固件 H264 解码器**不支持
    #         DEC_DST_CROPPING** (解码日志: `Unsupported option`), -c 被静默忽略,
    #         只有 -s 生效 ⇒ 变成「含黑边的整帧强行拉伸 1280x720→480x800」,
    #         更糊且变形。此路不通; WFD_DEC_SCALE 留着只作对照。
    #   结论: 解码器输**原始尺寸**, 裁剪+缩放全交 drmplay (它按真实内容区
    #         geom_fit, 只转绘制区, CPU 不吃亏)。
    #   回退: WFD_DEC_FIT=1 → 老行为 (-A); WFD_DEC_OPTS 可整体覆盖。
    if [ -z "${DEC_OPTS:-}" ]; then   # set -u: 必须带 :-
        DEC_OPTS="-H -D"
        if [ "${WFD_DEC_FIT:-0}" = "1" ]; then
            DEC_OPTS="$DEC_OPTS -A"           # 老行为, 对照用
        fi
        if [ -n "${WFD_DEC_SCALE:-}" ]; then  # "WxH" 让 VPU 整帧缩放, 对照用
            DEC_OPTS="$DEC_OPTS -s $WFD_DEC_SCALE"
        fi
    fi
    log "🎛 解码器参数: v4l2_dec $DEC_OPTS (原始尺寸出图, 裁剪/缩放交 drmplay)"
    if [ "$PLAYER_FIFO_MODE" = 1 ]; then
        DEC_OUT="$PLAYER_FIFO"
        PIPE_ONE='/usr/bin/wfd-sink '"$SINK_ARG"' -d /dev/stdout 2>>/var/log/miracast.log'"$TEE_SEG"' | /usr/bin/v4l2_dec /dev/stdin '"$DEC_OUT"' '"$DEC_OPTS"' 2>>/var/log/miracast-dec.log'
    else
        DEC_OUT=/dev/stdout
        PIPE_ONE='/usr/bin/wfd-sink '"$SINK_ARG"' -d /dev/stdout 2>>/var/log/miracast.log'"$TEE_SEG"' | /usr/bin/v4l2_dec /dev/stdin '"$DEC_OUT"' '"$DEC_OPTS"' 2>>/var/log/miracast-dec.log | '"$PLAYER_BIN $PLAYER_OPTS"' 2>>/var/log/miracast-play.log'
    fi
    setsid sh -c '
        PLAYER_BIN="'"$PLAYER_BIN"'"
        PLAYER_OPTS="'"$PLAYER_OPTS"'"
        SINK_ARG="'"$SINK_ARG"'"
        i=0
        while :; do
            '"$PIPE_ONE"'
            _rc=$?
            # GPU 播放器起不来 (没 /dev/vg_lite / map 失败等) 立刻回落 CPU, 不让主人看到黑屏
            if [ "$_rc" -ne 0 ] && [ "$PLAYER_BIN" = /usr/bin/vgplay ]; then
                echo "$(date) vgplay 退出码 $_rc → 回落 CPU 缩放 (drmplay)" >>/var/log/miracast-play.log
                PLAYER_BIN=/usr/bin/drmplay
                PLAYER_OPTS="-H -p nv12 -w 480 -h 800"
            fi
            # 2026-10-26 实测修: 这里原来是「起 drmplay -B 占屏 2 s 再杀」。实测
            #   ★每次重试都和新管线的 drmplay 抢 CRTC★ (play.log 成片
            #   `✗ SETCRTC(crtc=52 conn=54): Permission denied`), 新管线的上屏
            #   进程抢不到就退出 ⇒ 管线只剩 sink|v4l2_dec 两段; 而重试又恰好发生
            #   在「手机连上、切双模」那一秒, 现场就是「连上了但黑屏 / 这次连不上」。
            #   改成不留占位、0.5 s 后立刻重起整条管线 (空档毫秒级; 空闲态现在
            #   常驻监听管线, 本来就有 drmplay 占着 CRTC, 不需要额外黑屏占位)。
            sleep 0.5
            i=$((i+1))
            # 2026-10-26: 原来封顶 20 次, 手机秒断的重拨风暴 (13:17:18) 会在一秒内
            # 把额度烧光 ⇒ pipeline 自灭、7236 从此无人监听 (现象: 连上了但屏黑)。
            # 改成**不封顶**, 只每 10 次打一行; 真正的"别再重拨"由 source 断开事件负责。
            [ $((i % 10)) -eq 0 ] && \
                echo "[$(date +%H:%M:%S)] ♻ pipeline 已重试 $i 次 (不封顶, 继续等对端)" >>/var/log/miracast.log
        done' >/dev/null 2>&1 &
    echo $! > "$SINK_PIPE_PID"
    if [ -n "$SINK_GO_IP" ]; then
        log "▶ sink 客户端模式已启动: rtsp://$SINK_GO_IP:$SINK_GO_PORT → v4l2_dec → $PLAYER_BIN (pid $(cat "$SINK_PIPE_PID"))"
    else
        log "▶ sink 服务端模式已启动: 监听 :$SINK_GO_PORT 等 source 拨入 → v4l2_dec → $PLAYER_BIN (pid $(cat "$SINK_PIPE_PID"))"
    fi
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
                if [ "$PERSIST_RECONNECT" = "1" ] && [ -n "$_inv_pg" ] && [ -n "$_inv_sa" ]; then
                    wpa p2p_connect "$_inv_sa" pbc persistent="$_inv_pg" >/dev/null 2>&1 \
                        || wpa p2p_listen >/dev/null 2>&1 || true
                else
                    # 2026-10-28: 默认**拒绝** Invitation (见 conf 里 persistent_reconnect 注释)。
                    #   手机存下来的组在 ch8/5G 上, 接进去必然 AUTH 不被 ACK; 拒了它才回落
                    #   全新协商, 走我们钉死的 ch1。p2p_reject 会回 Invitation Response=fail。
                    log "   (PERSIST_RECONNECT=0 ⇒ 拒绝, 等对端回落到全新 GO 协商)"
                    if [ -n "$_inv_sa" ]; then
                        wpa p2p_reject "$_inv_sa" >/dev/null 2>&1 || true
                    fi
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
                    # 2026-10-27 实测 (努比亚 Z17 miniS): 我们在 listen 状态 (还没建组) 时,
                    #   对端不先发 PD, 而是直接发 GO Negotiation; 它 intent 让得更彻底,
                    #   结果**我们被选成 GO** ⇒ role=GO。
                    #   原来这里只打一行日志就撒手 ⇒ 组是个空壳: 没有 dhcpd (手机拿不到
                    #   IP)、:7236 无人监听 ⇒ 手机连上又掉; 而且我们从此卡在 GO 组里,
                    #   对外名字变成 "DIRECT-xxK230", 主人再点就「列表里找不到 K230」。
                    #   现在: 既然已经是 GO, 就把 GO 该干的活全干上 (配地址 + dhcpd +
                    #   GO 收流 watcher + 纯监听 :7236 + 看护), 让这台手机正常投上来。
                    GO_IF="$_gif"
                    : > /tmp/wfd-go-fallback
                    ip addr add "$GO_IP/24" dev "$_gif" 2>/dev/null || true
                    log "   → 按 GO 形态服务: GO_IP=$GO_IP on $_gif + dhcpd + :$SINK_GO_PORT 监听"
                    start_dhcpd "$_gif"
                    # 只清 GO 专用的旧 watcher (不能调 stop_go_watchers: 它会连本
                    # client watcher 一起杀掉, 后面几步就没人执行了)
                    for _pf in "$GO_WATCH_PID" "$GO_SUP_PID"; do
                        [ -f "$_pf" ] && { kill_tree "$(cat "$_pf" 2>/dev/null)"; rm -f "$_pf"; }
                    done
                    start_go_sink_watch
                    SINK_GO_IP=""
                    start_sink_client
                    start_go_supervisor
                fi ;;
            *AP-STA-DISCONNECTED*)
                # client 模式下"意外当 GO"只是临时救场: 手机走了就把组拆掉回到
                # listen (纯 P2P 设备), 否则我们长期以一个 GO 组的身份出现, 手机又
                # 回到「发完 PD 就不说话」的死结 (2026-10-27 现场)。
                [ -f /tmp/wfd-go-fallback ] || continue
                log "⚠ source 离开 GO 组, 5 s 后若无 STA 就拆组回 listen"
                ( sleep 5
                  _sif="${GO_IF:-$IFACE}"
                  if [ -z "$(iw dev "$_sif" station dump 2>/dev/null | awk '/^Station/{print $2; exit}')" ]; then
                      wpa p2p_group_remove "$_sif" >/dev/null 2>&1 || true
                      sleep 1
                      wpa p2p_listen >/dev/null 2>&1 || true
                      rm -f /tmp/wfd-go-fallback
                      log "♻ 已拆临时 GO 组, 回到 listen (手机可重新发现)"
                  fi ) & ;;
            *P2P-GROUP-REMOVED*)
                log "⚠ 组已移除: $(printf '%s' "$line" | cut -c1-80)"
                stop_sink_client
                ip addr flush dev "${CLIENT_IF:-$IFACE}" 2>/dev/null || true
                SINK_GO_IP=""
                wpa p2p_group_remove "${CLIENT_IF:-$IFACE}" >/dev/null 2>&1 || true
                sleep 1
                wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || true
                # 2026-10-27 现场 (11:09:01 手机 GO_ENDING_SESSION): 这里只杀不建 ⇒
                #   断开后板上**一个 wfd-sink 都没有**, :7236 无人监听, 屏也停在最后一帧。
                #   对端再点投屏时 RTSP 直接被人拒 ⇒ 主人实测「连好几次才成功」。
                #   照 GO 侧 AP-STA-DISCONNECTED 的教训: 停完立刻以**纯监听**重建,
                #   空闲态也始终有人在 :$SINK_GO_PORT 等 source。
                start_sink_client
                start_blank
                log "♻ 已清理(进程+IP)并重挂 listen, 已重建纯监听 pipeline (:$SINK_GO_PORT)" ;;
            *P2P-GROUP-FORMATION-FAILURE*)
                # 组没建成 (对端取消/超时/信道不合): 必须重新挂 listen,
                # 否则 supplicant 停在 IDLE, 对端再点就"扫不到/无法连接"。
                log "⚠ 建组失败, 重挂 listen (可让对端再试一次)"
                stop_sink_client
                ip addr flush dev "${CLIENT_IF:-$IFACE}" 2>/dev/null || true
                SINK_GO_IP=""
                sleep 1
                wpa p2p_listen >/dev/null 2>&1 || wpa p2p_find >/dev/null 2>&1 || true
                # 2026-10-27: 同上 —— 失败后同样要留一个纯监听 pipeline,
                #   否则手机紧接着重试时 :$SINK_GO_PORT 无人应答。
                start_sink_client ;;
            *CTRL-EVENT-DISCONNECTED*)
                log "⚠ 与 GO 断开: $(printf '%s' "$line" | cut -c1-70)" ;;
            *P2P-FIND-STOPPED*|*P2P-LISTEN-STOPPED*)
                wpa p2p_listen >/dev/null 2>&1 || true ;;
        esac
    done ) &
    echo $! > "$CLIENT_WATCH_PID"
    log "client 事件监听已启动 (pid $(cat "$CLIENT_WATCH_PID" 2>/dev/null))"
}

# ------------------------------------------------------- GO 模式收流 (本机当 GO)
# 场景: 本机 p2p_group_add 当 GO, 手机/PC 以 P2P client 身份关联进来。
#   2026-10-26 实测 (K80): AP-STA-CONNECTED → EAPOL 4-way → dhcpd ACK 192.168.49.12,
#   但 GO 分支过去只建组+dhcpd+PBC, **没有任何 RTSP 进程** ⇒ 手机侧等待 7236 超时,
#   表现为「点了投屏, 连不上/一直转圈」。这里补上: 关联 + 拿租约后立刻起 pipeline。
#   wfd_sink.c 的 listener 无条件创建, 所以 GO 模式用 `-c <source-ip>:7236` 双模
#   (既监听也主动拨), 手机把 RTSP server 放哪一侧都能通。

# 从 dhcpd 日志 (或 ARP 邻居表) 查出某个 MAC 拿到的 IP。
#   dhcpd 行格式: [dhcpd] ACK → da:85:76:6f:f8:7d 192.168.49.12 (xid=0x...)
lease_ip_of() {
    _mac=$(printf '%s' "$1" | tr 'A-Z' 'a-z')
    _ip=$(awk -v m="$_mac" '$2=="ACK" && tolower($4)==m { ip=$5 } END { print ip }' "$DHCP_LOG" 2>/dev/null)
    if [ -z "$_ip" ]; then
        _ip=$(ip neigh show dev "${GO_IF:-${CLIENT_IF:-$IFACE}}" 2>/dev/null \
              | tr 'A-Z' 'a-z' | awk -v m="$_mac" '$5==m { print $1; exit }')
    fi
    printf '%s' "$_ip"
}

# 按 pid 连同它的子进程一起杀掉 (GO watcher 是 `( tail | while )&` 子 shell,
#   只杀子 shell 会把 tail 留成孤儿继续吃事件 ⇒ 之后重复 start_sink_client 抢启停)
# 杀一个 pid 及其所有子进程。
# 2026-10-26 加固 (板上现场): pidfile 可能是上一轮留下的陈旧 pid, 重启后 pid 会被
#   复用 —— 不核对就 kill 会误杀无关进程, 甚至把我们自己带走。现象是「用
#   `setsid ... &` 后台起 GO 时, 脚本在 4.6 步静默退出」: 两次都停在
#   `start_pbc_watcher` 之后 (`/tmp/go-start-*.log` 恰好 19 行就没了),
#   而同一版本在前台 `sh -x` 跑就一路到底 —— 唯一差别是那次提前删了
#   /run/wfd-client-watch.pid (陈旧 pid). 所以 kill 前必须: ① 是数字
#   ② 是本项目进程 (命令行白名单) ③ 不是我们自己/祖先/同进程组。
kill_tree() {
    [ -n "$1" ] || return 0
    case "$1" in *[!0-9]*) return 0 ;; esac                # 不是纯数字, 忽略
    [ "$1" = "$$" ] && return 0
    [ -r "/proc/$1/cmdline" ] || return 0                  # 进程已退出
    _cl=$(tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null)
    case "$_cl" in
        *wfd-p2p.sh*|*wfd-pbc-watcher*|*wfd-p2p.log*) ;;
        *) return 0 ;;                                     # 不是本项目的, 绝不碰
    esac
    _p=$$                                                 # 目标在自己的祖先链上 → 绝不杀
    while [ -n "$_p" ] && [ "$_p" != "0" ] && [ "$_p" != "1" ]; do
        [ "$_p" = "$1" ] && return 0
        _p=$(awk '{print $4}' "/proc/$_p/stat" 2>/dev/null)
        [ -z "$_p" ] && break
    done
    for _d in /proc/[0-9]*; do
        _pp=$(awk '{print $4}' "$_d/stat" 2>/dev/null)
        [ "$_pp" = "$1" ] && kill -9 "${_d#/proc/}" 2>/dev/null
    done
    kill -9 "$1" 2>/dev/null || true
}

# 反复 `start` 时先清掉旧的 GO 事件 watcher / 看护 / client watcher。
# 2026-10-26: 以前只清 pbc-watcher, 这两个按 pidfile 一版叠一版 ⇒ 多个 watcher
#   同时对同一个 AP-STA-CONNECTED 起 pipeline, 互相 kill→restart, 现场表现
#   「点几次才成」「有时连上没画面」。
stop_go_watchers() {
    for _pf in "$GO_WATCH_PID" "$GO_SUP_PID" "$CLIENT_WATCH_PID"; do
        [ -f "$_pf" ] || continue
        _old=$(cat "$_pf" 2>/dev/null || echo "")
        kill_tree "$_old"
        log "🧹 清旧 GO watcher: $(basename "$_pf") pid=${_old:-?}"
        rm -f "$_pf"
    done
    sleep 0.3
}

start_go_sink_watch() {
    [ -f "$P2P_LOG" ] || return 0
    ( tail -n 0 -F "$P2P_LOG" 2>/dev/null | while IFS= read -r line; do
        case "$line" in
            *AP-STA-CONNECTED*)
                _mac=$(printf '%s\n' "$line" | sed -n 's/.*AP-STA-CONNECTED \([0-9a-fA-F:]*\).*/\1/p')
                [ -n "$_mac" ] || _mac=$(printf '%s\n' "$line" | awk '{for(i=1;i<=NF;i++) if($i=="AP-STA-CONNECTED"){print $(i+1); exit}}')
                log "📶 source 已关联我们的 GO 组: ${_mac:-?} (等 DHCP 租约…)"
                _ip=""; _n=0
                while [ "$_n" -lt 20 ]; do
                    _ip=$(lease_ip_of "$_mac")
                    [ -n "$_ip" ] && break
                    _n=$((_n + 1)); sleep 0.5
                done
                if [ -n "$_ip" ]; then
                    SINK_GO_IP="$_ip"
                    log "✅ source IP=$_ip → 起双模 pipeline (监听 :$SINK_GO_PORT + 拨 $_ip:$SINK_GO_PORT)"
                else
                    SINK_GO_IP=""
                    log "⚠ 10s 没拿到 $_mac 的租约 (看 $DHCP_LOG) → 先只监听 :$SINK_GO_PORT 等对端拨入"
                fi
                start_sink_client ;;
            *AP-STA-DISCONNECTED*)
                log "⚠ source 已离开 GO 组: $(printf '%s' "$line" | cut -c1-90)"
                SINK_GO_IP=""
                # 2026-10-26 实测: 原来这里是 `stop_sink_client` + 黑屏占位,
                #   **不留任何监听器** ⇒ 一旦下一轮的 AP-STA-CONNECTED 事件漏看 /
                #   来不及处理 (watcher 正卡在 10 s 租约轮询里), 手机连上 GO、
                #   发了 RTSP 也没人应答, 现场就是「这次连不上, 再点一次又行」。
                #   改成停掉旧 pipeline 后**立刻以纯监听模式重建**, 空闲态也始终
                #   有人在 :7236 等 source (管道里的 drmplay 自己占着 CRTC, 不会紫屏)。
                start_sink_client
                log "♻ 已切回纯监听 :$SINK_GO_PORT (等下一个 source)" ;;
        esac
    done ) &
    echo $! > "$GO_WATCH_PID"
    log "GO 收流 watcher 已启动 (pid $(cat "$GO_WATCH_PID" 2>/dev/null))"
}

# GO 模式看护 (2026-10-26 实测补): pipeline 的外层 while 只重试 20 次, 用尽后
#   进程全没了; 此时手机**还连着**, 不会再有 AP-STA-CONNECTED 事件 ⇒ 7236 从此
#   无人监听, 再点投屏就是"连不上"。这里每 3 s 兜一次底。
# 2026-10-26 第二轮加固: 原来只在「有 STA + 没 sink」时补 ⇒ 没有 STA 时永不补,
#   空闲态可能长时间**根本没有监听器** (只要有一次 watcher 漏事件/被 restart 打断)。
#   现在改成「没 sink 就补」: 有 STA 就双模拨它, 没 STA 就纯监听。
start_go_supervisor() {
    ( _miss=0
      _pdead=0
      _plr=$(basename "${PLAYER_BIN:-/usr/bin/drmplay}")
      while :; do
        sleep 3
        _sif="${GO_IF:-$IFACE}"
        if pgrep -f "/usr/bin/wfd-sink" >/dev/null 2>&1; then
            _miss=0
            # 管线三段必须齐全: 上屏进程单独死了 (抢 CRTC 失败 / 异常退出) 时
            # sink 还在监听、7236 也通, 但画面永远出不来 ⇒ 只能整条重起。
            if pgrep -x "$_plr" >/dev/null 2>&1; then
                _pdead=0
            else
                _pdead=$((_pdead + 1))
                if [ "$_pdead" -ge 2 ]; then
                    _pdead=0
                    log "🛠 看护: 收流在但上屏进程($_plr)不在 → 整条 pipeline 重起"
                    start_sink_client
                fi
            fi
            continue
        fi
        _pdead=0
        # 连 2 次 (6 s) 都没有才补: 避开 watcher 正在 stop→start 换模式的瞬间
        _miss=$((_miss + 1))
        [ "$_miss" -lt 2 ] && continue
        _miss=0
        _st=$(iw dev "$_sif" station dump 2>/dev/null | awk '/^Station/{print $2; exit}')
        if [ -n "$_st" ]; then
            _sip=$(lease_ip_of "$_st")
            SINK_GO_IP="$_sip"
            log "🛠 看护: source($_st) 仍在线但收流进程不在 → 重起 pipeline (ip=${_sip:-未知})"
        else
            SINK_GO_IP=""
            log "🛠 看护: 收流进程不在且当前无 source → 恢复纯监听 pipeline (保证 :$SINK_GO_PORT 始终有人应答)"
        fi
        start_sink_client
      done ) &
    echo $! > "$GO_SUP_PID"
    log "GO 看护已启动 (pid $(cat "$GO_SUP_PID" 2>/dev/null), 每 3 s 兜底)"
}

cmd_client() {
    need_root
    ps_off
    backup_conf
    write_conf client
    stop_dhcpd            # client 模式不用 DHCP 服务端 (参考实现也是静态地址)
    # 监听模式的 pipeline 由 miracast.service 保活 (Restart=always), 必须显式停掉,
    # 否则它会把解码器/DRM 占住, 我们客户端模式起不来 (2026-10-21)。
    # K230 2026-10-24: 开机自启已改为本脚本(client)本身, 不再停 miracast.service (否则自杀)
    ip addr flush dev "$IFACE" 2>/dev/null || true
    rm -f /tmp/wfd-p2p.done /tmp/wfd-p2p.failed
    start_supplicant

    # 建组/重新关联都会把 power_save 重置回 on, 所以整个会话期每 5 s 复核一次
    # (ps_off 内部先判断, 只在真的是 on 时才动, 幂等且不刷日志)
    ( while :; do ps_off; sleep 5; done ) >/dev/null 2>&1 &

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

    # 2026-10-27: 进 client 模式就先把**纯监听 pipeline** 拉起来 (:7236 常驻有人应答)。
    #   原来只等 P2P-GROUP-STARTED 才起 ⇒ 空闲态板上根本没有 wfd-sink:
    #   手机/PC 建组后若先拨 RTSP (有的机型/Windows 就是这样), 直接被拒,
    #   现场表现就是「连好几次才成功」。纯监听不影响后面的双模建链
    #   (GROUP-STARTED 会 stop→start 换成带 -c 的形态)。
    start_sink_client
    log "📡 已常驻监听 :$SINK_GO_PORT (空闲也不撤, 等 source 随时拨入)"

    log "✅ 已进入 client 模式: P2P device (不建组), p2p_go_intent=0 ⇒ 对端(手机/PC)当 GO"
    log "现在用: 手机「无线投屏」/ Windows 10 「Win+K → 连接到无线显示器」里点 $DEVICE_NAME"
    log "  · 安卓 GO=192.168.49.1 → 静态 $CLIENT_IP; Windows GO=192.168.137.1 → DHCP 拿租约"
    log "  · Windows 侧注意: 只留一块支持无线显示器的网卡 (netsh wlan show drivers 有那一行为 是)"
    log "成功后应看到: grep -E 'P2P-GROUP-STARTED|✅ GO=|✅ DHCP' $P2P_LOG"
    # 驻留: 作为 systemd service 时进程不能退 (Type=simple + KillMode=control-group,
    # 一退就整组被杀 ⇒ 监听器/ supplicant 全没, 服务每 3 s 重启一次空转)。
    log "（前台驻留, Ctrl+C 或 $0 stop 结束）"
    # 开机/进入 client 模式后先把屏幕刷黑占住, 避免空闲时面板显示未定义内容 (紫色)
    start_blank
    wait
}

cmd_stop() {
    need_root
    # 2026-10-26: stop 的语义 = "停掉并且别自动回来" (调试独占)。
    #   留一个 hold 旗标, 开机自启 / systemd 重启都会空跑退出, 绝不跟调试抢 wlan1、7236、DRM。
    if [ "${WFD_HOLD:-1}" != "0" ]; then
        mkdir -p "$(dirname "$HOLD_FILE")" 2>/dev/null || true
        : > "$HOLD_FILE"
        log "⏸ 已置 $HOLD_FILE —— 开机自启不会抢回设备 (恢复: sh $0 resume)"
    fi
    stop_pbc_watcher
    stop_sink_client
    # 2026-10-26: 播放器不再由本脚本停 —— 屏归 k230-screen.service 所有 (常驻 drmplay)。
    #   停掉管线后 FIFO 没有写端, 播放器自己 800 ms 后刷黑, 屏上不会紫。
    #   停它反而会让"切去 AirPlay"那一下重新起播放器, 露出一次紫屏。
    # (start_blank 同理不再需要: 常驻播放器本身就是黑屏占位)
    if [ -f "$CLIENT_WATCH_PID" ]; then
        kill "$(cat "$CLIENT_WATCH_PID" 2>/dev/null || echo 0)" 2>/dev/null || true
        rm -f "$CLIENT_WATCH_PID"
    fi
    if [ -f "$GO_WATCH_PID" ]; then
        kill "$(cat "$GO_WATCH_PID" 2>/dev/null || echo 0)" 2>/dev/null || true
        rm -f "$GO_WATCH_PID"
    fi
    if [ -f "$GO_SUP_PID" ]; then
        kill "$(cat "$GO_SUP_PID" 2>/dev/null || echo 0)" 2>/dev/null || true
        rm -f "$GO_SUP_PID"
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
    # 2026-10-26: 这里原来会 `systemctl start miracast.service` —— 手动 stop 之后服务
    #   立刻把 supplicant/管线抢回去, 是"调试跟自启打架"的老根子。已删除。
    log "已停 P2P; station($STATION_IFACE) IP: $(ip -4 addr show "$STATION_IFACE" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\)\/.*/\1/p' | head -1)"
}

# 解除 hold 并启动 (self-heal 入口: 自启服务与人工都用它)
cmd_resume() {
    need_root
    rm -f "$HOLD_FILE"
    log "▶ 已清 $HOLD_FILE, 启动 GO..."
    cmd_start "$@"
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
    resume)  cmd_resume ;;
    listen)  cmd_listen ;;
    stop)    cmd_stop ;;
    client)  cmd_client ;;
    status)  cmd_status ;;
    devinfo) shift; cmd_devinfo "$@" ;;
    *)       usage; exit 1 ;;
esac
