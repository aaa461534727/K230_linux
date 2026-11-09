#!/bin/sh
# carlife-ap.sh —— 无线 CarLife 热点 (wlan1) 起停
#
# 与 Miracast 的关系 (2026-11-07 起): 同一颗 RTL8189FS 的两个 vif, Miracast 固定 wlan0
# (P2P), 本 AP 固定 wlan1 ⇒ 起 AP 不再停 Miracast (只有 AP 落在 wlan0 时才让位)。
# 注意: 单 radio 单信道共存, 两边的信道要一致才能同时稳定工作。
# AirPlay 走 USB 以太网, 不受影响。
#
# 手机端 CarLife 的无线流程 (2026-10-30 逆�� APK 确认):
#   手机连上本热点 → App 每 2 s 向 255.255.255.255:7999 广播 "CarlifeHost"
#   → 车机听到后主动连手机的 7240/8240/9240/9241/9242/9340/9440
#
# 两种无线形态 (车机侧) —— 任选其一, 都会让手机打开 App 时自动连上:
#   ① 车机当热点:  sh carlife-ap.sh start              (手机连 K230-CARLIFE / carlife1234)
#   ② 车机连热点:  sh carlife-ap.sh sta <SSID> [PSK]   (K230 加入手机/路由器热点, wpa_supplicant + dhcpcd)
#   收尾:          sh carlife-ap.sh stop               (两种都能停, 恢复 USB CarLife)
#
# 用法: sh carlife-ap.sh start | sta <SSID> [PSK] | stop | status
set -u
IFACE="${CARLIFE_AP_IF:-wlan1}"
SSID="${CARLIFE_AP_SSID:-K230-CARLIFE}"
PSK="${CARLIFE_AP_PSK:-carlife1234}"
GW="${CARLIFE_AP_GW:-192.168.50.1}"
NET="192.168.50.0/24"
CONF=/tmp/carlife-hostapd.conf
DPID=/run/carlife-dnsmasq.pid
DCONF=/tmp/carlife-dnsmasq.conf
HLOG=/var/log/carlife-hostapd.log
DLOG=/var/log/carlife-dnsmasq.log
SCONF=/tmp/carlife-sta.conf
SLOG=/var/log/carlife-wpa.log
# 本板 wpa_supplicant 2.11 是针对 OpenSSL 3.2 编的, 系统 libcrypto 却是 3.0.13
# ⇒ 必须优先用 /root/wpalib 那套 (Miracast 那轮装好的), 否则报 OPENSSL_3.2.0 not found
WPA_LDPATH=/root/wpalib
[ -f "$WPA_LDPATH/libcrypto.so.3" ] || WPA_LDPATH=

# 该板 iptables 默认 nft 后端, 但内核没 nf_tables ⇒ 一律用 legacy
IPT="iptables"
iptables-legacy -t nat -S >/dev/null 2>&1 && IPT="iptables-legacy"

uplink() { ip route | awk '/^default/ {print $5; exit}'; }

nat_on() {
    UP=$(uplink)
    [ -z "$UP" ] && { echo "[ap] ! 没有默认路由, 手机将没有外网"; return; }
    sysctl -q -w net.ipv4.ip_forward=1
    $IPT -t nat -C POSTROUTING -s "$NET" -o "$UP" -j MASQUERADE 2>/dev/null || \
        $IPT -t nat -A POSTROUTING -s "$NET" -o "$UP" -j MASQUERADE
    $IPT -C FORWARD -i "$IFACE" -o "$UP" -j ACCEPT 2>/dev/null || \
        $IPT -A FORWARD -i "$IFACE" -o "$UP" -j ACCEPT
    $IPT -C FORWARD -i "$UP" -o "$IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
        $IPT -A FORWARD -i "$UP" -o "$IFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT
    echo "[ap] NAT: $NET -> $UP"
}

case "${1:-start}" in
start)
    # 2026-11-07: 本板 Miracast 已固定在 wlan0 (unit Environment=WFD_IFACE=wlan0),
    #   AP 固定 wlan1 ⇒ 两个 vif 可以共存, 起 AP 不再停 Miracast / 不再杀 wfd-p2p。
    #   只有 AP 真的落在 Miracast 那个接口上 (wlan0) 时才需要让位。
    if [ "$IFACE" = "wlan0" ]; then
        echo "[ap] 停 Miracast (AP 与 P2P 同一接口) + 清 P2P"
        timeout 10 systemctl stop k230-miracast 2>/dev/null
        pkill -f wfd-p2p.sh 2>/dev/null
    else
        echo "[ap] 保留 Miracast (在另一 vif, 不动)"
    fi
    # 被 oneshot 单元调用时同步 stop 会死锁 ⇒ 加 timeout 兜底 (最坏 10 s 后放弃)
    timeout 10 systemctl stop k230-cast 2>/dev/null
    pkill -f k230-fallback.py 2>/dev/null
    pkill -x hostapd 2>/dev/null
    pkill -x dnsmasq 2>/dev/null
    pkill -x dhcpd 2>/dev/null
    # STA 模式残留 (上次 sta 用过的 wpa_supplicant / dhcpcd lease) 必须清掉,
    # 否则它会把网卡又拽回 managed, hostapd 报 AP-ENABLED 但实际连不上。
    # ★ 2026-11-02: **只清本网卡上的 supplicant**。原来是 pkill -x wpa_supplicant (全局),
    #   会把 wlan0 连路由器的那个一起杀掉 (板卡就再也回不来, 现场两次失联);
    #   本板 wlan0/wlan1 是同一颗 8189fs 的两个 vif, 但进程可以各管一个。
    pkill -f "wpa_supplicant.*-i *\${IFACE}\b" 2>/dev/null
    pkill -f "wpa_supplicant.*-i *\${IFACE} " 2>/dev/null
    dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    sleep 2
    ip addr flush dev "$IFACE" 2>/dev/null
    ip link set "$IFACE" down 2>/dev/null
    iw dev "$IFACE" set type managed 2>/dev/null
    sleep 1
    ip link set "$IFACE" up
    sleep 1

    cat > "$CONF" <<EOF
interface=$IFACE
driver=nl80211
ssid=$SSID
hw_mode=g
channel=6
ieee80211n=1
wmm_enabled=1
auth_algs=1
wpa=2
wpa_passphrase=$PSK
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
# 2026-11-07: 禁用 600 s GTK 轮换. 现场铁证: AP 空闲跑到第 3 次 rekey (12:45:52) 后,
#   手机 associated 6 s 就断、零 DHCP, hostapd -f 详细日志无任何 EAPOL/4-way 行;
#   重启 hostapd 进程也无效, 只有 reboot 才恢复. SIGHUP 不会重读本段, 必须在启动前写入.
wpa_group_rekey=0
EOF

    hostapd -B -P /run/carlife-hostapd.pid "$CONF" >"$HLOG" 2>&1
    sleep 2
    if ! pgrep -x hostapd >/dev/null; then
        echo "[ap] x hostapd 没起来, 见 $HLOG"; tail -5 "$HLOG"; exit 1
    fi
    i=0
    while [ $i -lt 5 ]; do
        iw dev "$IFACE" info 2>/dev/null | grep -q "type AP" && break
        i=$((i+1)); sleep 1
    done
    if ! iw dev "$IFACE" info 2>/dev/null | grep -q "type AP"; then
        echo "[ap] x 网卡没进 AP 模式 (多半是 STA 残留的 wpa_supplicant 还占着)"; exit 1
    fi

    i=0
    while [ $i -lt 3 ]; do
        ip -4 addr show "$IFACE" | grep -q "$GW/" && break
        ip addr add "$GW/24" dev "$IFACE" 2>/dev/null
        i=$((i+1)); sleep 1
    done
    ip -4 addr show "$IFACE" | grep -q "$GW/" || echo "[ap] ! 网关地址没配上"

    cat > "$DCONF" <<EOF
interface=$IFACE
bind-dynamic
except-interface=lo
dhcp-range=192.168.50.10,192.168.50.60,255.255.255.0,12h
dhcp-option=3,$GW
dhcp-option=6,$GW
log-dhcp
EOF
    dnsmasq -C "$DCONF" -x "$DPID" --log-facility="$DLOG"
    sleep 1
    if pgrep -x dnsmasq >/dev/null; then
        echo "[ap] dnsmasq 已起 (DHCP 192.168.50.10-60, DNS=网关)"
    else
        echo "[ap] ! dnsmasq 没起来, 见 $DLOG"
    fi

    nat_on

    # 屏幕只有一块: 无线会话和 USB 会话不能同时跑 (各起一个 v4l2_dec 抢播放器)
    # ★ 2026-11-02: 本脚本现在会被 k230-shared-ap.service (Type=oneshot) 调用。
    #   **oneshot 的 ExecStart 里绝不能同步 systemctl start/restart 别的单元** ——
    #   systemd 事务排队会死锁到自己超时, 超时后 TERM 连带杀掉刚起好的 hostapd
    #   (现场 17:36:26: hostapd 已 AP-ENABLED, 却因 start 超时被全部回收)。
    #   所以: ① 由仲裁器负责起投屏端时设 CARLIFE_AP_SKIP_SERVICE=1;
    #        ② 否则一律用 --no-block (只排队, 不等待)。
    if [ "${CARLIFE_AP_SKIP_SERVICE:-0}" = "1" ]; then
        echo "[ap] 跳过 k230-carlife-wifi (由仲裁器负责)"
    else
        systemctl stop k230-carlife 2>/dev/null
        systemctl --no-block restart k230-carlife-wifi 2>/dev/null
        echo "[ap] 已请求启动 k230-carlife-wifi (--no-block)"
    fi
    echo "[ap] OK 热点: SSID=$SSID 密码=$PSK 网关=$GW"
    ;;
sta)
    STA_SSID="$2"; STA_PSK="$3"
    if [ -z "$STA_SSID" ]; then
        echo "用法: $0 sta <SSID> [PSK]"; exit 2
    fi
    echo "[sta] 停 Miracast / AP 进程 (同一颗网卡, 只能一个角色)"
    systemctl stop k230-miracast 2>/dev/null
    systemctl stop k230-cast 2>/dev/null
    pkill -f wfd-p2p.sh 2>/dev/null
    pkill -f k230-fallback.py 2>/dev/null
    pkill -x hostapd 2>/dev/null
    pkill -x dnsmasq 2>/dev/null
    # 只清本网卡上的 supplicant (全局 pkill 会把别的 vif 上的连接一起杀掉)
    pkill -f "wpa_supplicant.*-i *${IFACE}\b" 2>/dev/null
    dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    UP=$(uplink)
    [ -n "$UP" ] && $IPT -t nat -D POSTROUTING -s "$NET" -o "$UP" -j MASQUERADE 2>/dev/null
    sleep 1
    ip addr flush dev "$IFACE" 2>/dev/null
    ip link set "$IFACE" down 2>/dev/null
    iw dev "$IFACE" set type managed 2>/dev/null || { echo "[sta] x 切 managed 失败"; exit 1; }
    sleep 1
    ip link set "$IFACE" up
    sleep 1

    if [ -n "$STA_PSK" ]; then KM="key_mgmt=WPA-PSK"; PSKLINE="psk=\"$STA_PSK\""; else KM="key_mgmt=NONE"; PSKLINE=""; fi
    {
        echo "ctrl_interface=/run/wpa_supplicant-carlife"
        echo "update_config=1"
        echo "network={"
        echo "    ssid=\"$STA_SSID\""
        [ -n "$PSKLINE" ] && echo "    $PSKLINE"
        echo "    $KM"
        echo "}"
    } > "$SCONF"

    : > "$SLOG"
    env LD_LIBRARY_PATH="$WPA_LDPATH" wpa_supplicant -B -i "$IFACE" -c "$SCONF" -f "$SLOG" 2>/dev/null
    i=0
    while [ $i -lt 20 ]; do
        grep -q "CTRL-EVENT-CONNECTED" "$SLOG" 2>/dev/null && break
        i=$((i+1)); sleep 1
    done
    if ! grep -q "CTRL-EVENT-CONNECTED" "$SLOG" 2>/dev/null; then
        echo "[sta] x 20 s 内没连上 \"$STA_SSID\""; grep -E "CTRL-EVENT|WRONG_KEY|reason" "$SLOG" | tail -6; exit 1
    fi
    echo "[sta] 已关联 \"$STA_SSID\""

    dhcpcd -4 -t 20 "$IFACE" >>/var/log/carlife-dhcpcd.log 2>&1
    i=0
    while [ $i -lt 12 ]; do
        ip -4 addr show "$IFACE" | grep -q "inet " && break
        i=$((i+1)); sleep 1
    done
    if ! ip -4 addr show "$IFACE" | grep -q "inet "; then
        echo "[sta] x 没拿到 IP (dhcpcd), 见 /var/log/carlife-dhcpcd.log"
        tail -5 /var/log/carlife-dhcpcd.log; exit 1
    fi
    ip -4 addr show "$IFACE" | grep "inet "

    systemctl stop k230-carlife 2>/dev/null
    systemctl restart k230-carlife-wifi 2>/dev/null
    if systemctl is-active k230-carlife-wifi >/dev/null 2>&1; then
        echo "[sta] 无线 CarLife 服务已起 (k230-carlife-wifi), 等手机 App 广播 CarlifeHost"
    else
        echo "[sta] ! k230-carlife-wifi 没起来: journalctl -u k230-carlife-wifi"
    fi
    ;;
stop)
    [ -f /run/carlife-hostapd.pid ] && kill "$(cat /run/carlife-hostapd.pid)" 2>/dev/null
    pkill -x hostapd 2>/dev/null
    pkill -x dnsmasq 2>/dev/null
    pkill -f "wpa_supplicant.*-i *${IFACE}\b" 2>/dev/null
    dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    UP=$(uplink)
    [ -n "$UP" ] && $IPT -t nat -D POSTROUTING -s "$NET" -o "$UP" -j MASQUERADE 2>/dev/null
    ip addr flush dev "$IFACE" 2>/dev/null
    ip link set "$IFACE" down 2>/dev/null
    ip link set "$IFACE" up
    iw dev "$IFACE" set type managed 2>/dev/null
    timeout 10 systemctl stop k230-carlife-wifi 2>/dev/null
    # oneshot 的 ExecStop 里同样不能同步 start 别的单元
    systemctl --no-block start k230-carlife 2>/dev/null
    echo "[ap] 已停; 恢复 Miracast: systemctl start k230-miracast k230-cast"
    ;;
status)
    echo "== hostapd"; pgrep -a hostapd || echo "  (无)"
    echo "== dnsmasq"; pgrep -a dnsmasq || echo "  (无)"
    echo "== $IFACE"; iw dev "$IFACE" info 2>/dev/null | grep -E "type|ssid|channel"
    ip -4 addr show "$IFACE" | grep inet || true
    echo "== wpa (STA 模式)"; pgrep -a wpa_supplicant || echo "  (无)"
    echo "== NAT"; $IPT -t nat -S 2>/dev/null | grep MASQUERADE || echo "  (无)"
    echo "== 客户端"; iw dev "$IFACE" station dump 2>/dev/null | grep -E "^Station|signal" | head -6
    ;;
*)
    echo "用法: $0 start | sta <SSID> [PSK] | stop | status"; exit 2;;
esac
