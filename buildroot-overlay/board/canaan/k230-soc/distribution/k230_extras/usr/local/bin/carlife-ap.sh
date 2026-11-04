#!/bin/sh
# carlife-ap.sh —— 无线 CarLife 热点 (wlan1) 起停
#
# 与 Miracast 互斥: 板子只有一颗 2.4G 网卡 (RTL8189FS), 开 AP 前必须停 P2P。
# AirPlay 走以太网, 不受影响。
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
    echo "[ap] 停 Miracast (同一颗网卡) + 清 P2P"
    systemctl stop k230-miracast 2>/dev/null
    systemctl stop k230-cast 2>/dev/null
    pkill -f wfd-p2p.sh 2>/dev/null
    pkill -f k230-fallback.py 2>/dev/null
    pkill -x hostapd 2>/dev/null
    pkill -x dnsmasq 2>/dev/null
    pkill -x dhcpd 2>/dev/null
    # STA 模式残留 (上次 sta 用过的 wpa_supplicant / dhcpcd lease) 必须清掉,
    # 否则它会把网卡又拽回 managed, hostapd 报 AP-ENABLED 但实际连不上。
    pkill -x wpa_supplicant 2>/dev/null
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
    systemctl stop k230-carlife 2>/dev/null
    systemctl restart k230-carlife-wifi 2>/dev/null
    if systemctl is-active k230-carlife-wifi >/dev/null 2>&1; then
        echo "[ap] 无线 CarLife 服务已起 (k230-carlife-wifi)"
    else
        echo "[ap] ! k230-carlife-wifi 没起来: journalctl -u k230-carlife-wifi"
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
    pkill -x wpa_supplicant 2>/dev/null
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
    pkill -x wpa_supplicant 2>/dev/null
    dhcpcd -k "$IFACE" >/dev/null 2>&1 || true
    UP=$(uplink)
    [ -n "$UP" ] && $IPT -t nat -D POSTROUTING -s "$NET" -o "$UP" -j MASQUERADE 2>/dev/null
    ip addr flush dev "$IFACE" 2>/dev/null
    ip link set "$IFACE" down 2>/dev/null
    ip link set "$IFACE" up
    iw dev "$IFACE" set type managed 2>/dev/null
    systemctl stop k230-carlife-wifi 2>/dev/null
    systemctl start k230-carlife 2>/dev/null
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
