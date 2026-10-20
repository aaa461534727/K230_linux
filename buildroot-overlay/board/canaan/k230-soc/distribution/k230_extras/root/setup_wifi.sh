#!/bin/bash
# =============================================================================
# setup_wifi.sh — K230 Ubuntu 一键配 WiFi (板载 RTL8189FS, wlan0)
# 由 SDK 构建预置到镜像 /root/setup_wifi.sh (见 distribution.sh: k230_apply_extras)
#
# 用法:
#   setup_wifi.sh <PASSWORD>                       # 连默认 SSID carystudio-Meeting-2.4G
#   setup_wifi.sh <SSID> <PASSWORD>                # 连指定 SSID
#   setup_wifi.sh <SSID> <PASSWORD> [wlan0|wlan1]  # 指定接口 (默认 wlan0)
#
# 注意: 配置文件是 /etc/wpa_supplicant/wpa_supplicant-<iface>.conf
#       SSID 大小写敏感 (carystudio-Meeting-2.4G, 不是小写 meeting)
# =============================================================================
set -e
SSID_DEFAULT="carystudio-Meeting-2.4G"
IFACE="${3:-wlan0}"
if [ -n "${2:-}" ]; then
  SSID="$1"; PASS="$2"
elif [ -n "${1:-}" ]; then
  SSID="$SSID_DEFAULT"; PASS="$1"
else
  echo "用法: $0 <PASSWORD>  (默认连 $SSID_DEFAULT)  或  $0 <SSID> <PASSWORD> [wlan0|wlan1]"
  exit 1
fi

echo "=> 配置 WiFi: SSID=[$SSID] 接口=[$IFACE]"

CONF="/etc/wpa_supplicant/wpa_supplicant-${IFACE}.conf"
# wpa_passphrase 输出的 #psk="明文" 行要删掉, 避免密码明文落盘
{
  echo "ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev"
  echo "update_config=1"
  echo "country=CN"
  wpa_passphrase "$SSID" "$PASS"
} > "$CONF"
sed -i '/^[[:space:]]*#psk=/d' "$CONF"
chmod 600 "$CONF"

# systemd-networkd 接管该接口 (DHCP)
cat > "/etc/systemd/network/20-${IFACE}.network" <<EOF
[Match]
Name=${IFACE}

[Network]
DHCP=yes
EOF

mkdir -p /run/wpa_supplicant
systemctl enable systemd-networkd.service >/dev/null 2>&1 || true
systemctl enable "wpa_supplicant@${IFACE}.service" >/dev/null 2>&1 || true
systemctl restart systemd-networkd
systemctl restart "wpa_supplicant@${IFACE}.service"

# 等 DHCP
echo '--- 等待 DHCP (最多 30s) ---'
ADDR=""
for i in $(seq 1 30); do
  ADDR=$(ip -4 addr show "$IFACE" 2>/dev/null | grep -oP 'inet \K[0-9.]+' || true)
  if [ -n "$ADDR" ]; then echo "✓ ${IFACE} 获得 IP: $ADDR"; break; fi
  sleep 1
done
if [ -z "$ADDR" ]; then
  echo "⚠ 未获得 IP: 检查密码/信号 -> wpa_cli -i $IFACE status ; iw dev $IFACE scan | grep SSID"
  exit 2
fi

# 连通性快测
if ping -c 2 -W 3 223.5.5.5 >/dev/null 2>&1; then
  echo "✓ 外网连通 (223.5.5.5)"
else
  echo "⚠ 有 IP 但外网不通 (可能是路由/网关问题)"
fi
