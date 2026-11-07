#!/bin/sh
# k230-mode.sh —— 四功能模式切换 (WiFi 射频域 + 蓝牙形态联动)
#
# 模式:
#   miracast  P2P 域, 蓝牙关
#   carplay   AP 域 + 车载蓝牙 (iAP2)
#   carlife   AP 域, 蓝牙关 (CarLife 不用蓝牙)
#   airplay   AP 域 (无线, iPhone 连 K230-CARPLAY WiFi 走 mDNS 投屏), 反控 HID 蓝牙
#
# 规则:
#   · WiFi 只在 P2P ↔ AP 互切时重置 (modprobe -r/modprobe 8189fs)
#   · 蓝牙只在 AirPlay ↔ CarPlay 之间切换 (HID ↔ 车载); CarLife/Miracast 关蓝牙
#   · 切完必须过门禁, 失败退出 1 (调用方可 reboot 兜底)
set -u
if [ $# -lt 1 ]; then echo "用法: $0 miracast|airplay|carlife|carplay" >&2; exit 2; fi
MODE=$1
case "$MODE" in miracast|airplay|carlife|carplay) ;; *) echo "用法: $0 miracast|airplay|carlife|carplay" >&2; exit 2 ;; esac
STATE=/run/k230-mode.state
PERSIST=/root/.k230-mode
LOG=/var/log/k230-mode.log
say(){ printf '[%s] %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }
active(){ systemctl is-active --quiet "$1" 2>/dev/null; }
wait_active(){ _i=0; while [ "$_i" -lt 40 ]; do active "$1" && return 0; sleep 0.5; _i=$((_i+1)); done; return 1; }
wait_7236(){ _i=0; while [ "$_i" -lt 30 ]; do ss -lnt 2>/dev/null | grep -q ":7236 " && return 0; sleep 1; _i=$((_i+1)); done; return 1; }
wait_ap(){ _i=0; while [ "$_i" -lt 20 ]; do iw dev wlan1 info 2>/dev/null | grep -q "type AP" && return 0; sleep 1; _i=$((_i+1)); done; return 1; }
sdp_serving(){ _s=$(systemctl show -p ActiveEnterTimestamp --value carplay-wireless 2>/dev/null); [ -n "$_s" ] || return 1; journalctl -u carplay-wireless --since "$_s" --no-pager 2>/dev/null | grep -q "serving .*Wireless iAPv2"; }
wait_sdp(){ _i=0; while [ "$_i" -lt 20 ]; do sdp_serving && return 0; sleep 1; _i=$((_i+1)); done; return 1; }
domain(){ case "$1" in miracast) echo p2p;; carplay|carlife|airplay) echo ap;; *) echo unknown;; esac; }
bt_name(){ hciconfig hci0 name 2>/dev/null | awk -F"'" '/Name:/{print $2}'; }
hci_pscan(){ hciconfig hci0 2>/dev/null | grep -q 'UP RUNNING PSCAN'; }
# hci0 无响应时做 USBDEVFS_RESET (CSR dongle 连续切模式后会卡在 BD=00:00 / Broken pipe)
bt_recover(){
    hciconfig hci0 up >/dev/null 2>&1 || true
    hciconfig hci0 2>/dev/null | grep -q "UP RUNNING" && return 0
    say "BT 适配器无响应, 做 USBDEVFS_RESET"
    python3 - <<'PY' >/dev/null 2>&1 || return 1
import glob, os, fcntl
target = None
for d in glob.glob("/sys/bus/usb/devices/*"):
    try:
        vid = open(d + "/idVendor").read().strip()
        bus = int(open(d + "/busnum").read().strip())
        dev = int(open(d + "/devnum").read().strip())
    except Exception:
        continue
    if vid == "0a12":
        target = "/dev/bus/usb/%03d/%03d" % (bus, dev)
        break
if not target:
    raise SystemExit(1)
fd = os.open(target, os.O_WRONLY)
fcntl.ioctl(fd, 0x5514, 0)
os.close(fd)
PY
    sleep 2
    hciconfig hci0 up >/dev/null 2>&1 || true
}

prev=$(cat "$STATE" 2>/dev/null || echo unknown)
[ "$prev" = "$MODE" ] && { say "已经是 $MODE"; exit 0; }
pd=$(domain "$prev"); td=$(domain "$MODE")
say "切换: $prev($pd) -> $MODE($td)"

# 1) 停旧模式 (含蓝牙与投屏端)
timeout 20 systemctl stop k230-miracast k230-airplay k230-carlife-wifi k230-carlife \
    carplay-wireless carplay-airplayd carplay-arbiter \
    k230-bt-hid.path k230-bt-hid k230-bt-agent >/dev/null 2>&1
[ "$td" != ap ] && timeout 15 systemctl stop k230-shared-ap >/dev/null 2>&1

# 2) 非 AirPlay/CarPlay 的目标: 蓝牙关
if [ "$MODE" != airplay ] && [ "$MODE" != carplay ]; then
    timeout 10 systemctl stop bluetooth >/dev/null 2>&1
    hciconfig hci0 down >/dev/null 2>&1 || true
fi

# 3) WiFi 重置 (仅 P2P <-> AP)
# 只要目标域是 AP 或 P2P, 且当前不是同一域, 就重置 (保守做法:
# USB 模式不占 WiFi, 但上一次 P2P/AP 的射频状态可能还在)
if { [ "$td" = ap ] && [ "$pd" != ap ]; } || { [ "$td" = p2p ] && [ "$pd" != p2p ]; }; then
    say "WiFi 重置 (modprobe -r/modprobe 8189fs)"
    timeout 15 systemctl stop k230-shared-ap >/dev/null 2>&1
    timeout 20 modprobe -r 8189fs; sleep 2
    timeout 20 modprobe 8189fs; sleep 3
fi

# 4) 起目标 WiFi 域
case "$MODE" in
    miracast)
        rm -f /run/wfd-p2p.hold
        systemctl start k230-miracast >/dev/null 2>&1
        wait_active k230-miracast || { say "✗ k230-miracast 起不来"; exit 1; }
        wait_7236 || { say "✗ :7236 没起来"; exit 1; }
        ;;
    carplay|carlife)
        systemctl start k230-shared-ap >/dev/null 2>&1
        wait_active k230-shared-ap || { say "✗ k230-shared-ap 起不来"; exit 1; }
        wait_ap || { say "✗ wlan1 没进 AP"; exit 1; }
        ;;
    airplay)
        # ★ 2026-11-07: 无线 AirPlay —— 进 AirPlay 模式时板子起 AP, 让 iPhone 连
        #   K230-CARPLAY WiFi 后走 mDNS 发现 K230-CAST 并投屏 (替代原本的 USB 以太网)。
        #   AirPlay 反控(蓝牙 HID) 仍在 step 6 起, 与 AP 互不冲突。
        systemctl start k230-shared-ap >/dev/null 2>&1
        wait_active k230-shared-ap || { say "✗ k230-shared-ap 起不来"; exit 1; }
        wait_ap || { say "✗ wlan1 没进 AP"; exit 1; }
        ;;
esac

# 5) 起应用服务
case "$MODE" in
    airplay) systemctl start k230-airplay >/dev/null 2>&1 ;;
    carlife) systemctl start k230-carlife-wifi >/dev/null 2>&1 ;;
esac

# 6) 蓝牙形态
case "$MODE" in
    airplay)
        timeout 10 systemctl stop bluetooth >/dev/null 2>&1
        bt_recover
        systemctl start bluetooth >/dev/null 2>&1; sleep 2
        hciconfig hci0 up >/dev/null 2>&1 || true
        systemctl start k230-bt-hid >/dev/null 2>&1
        _i=0; while [ "$_i" -lt 40 ]; do grep -q '注册=是' /run/k230-bt-hid.status 2>/dev/null && break; sleep 0.5; _i=$((_i+1)); done
        hciconfig hci0 name K230-CAST >/dev/null 2>&1 || true
        ;;
    carplay)
        timeout 10 systemctl stop bluetooth >/dev/null 2>&1
        bt_recover
        hciconfig hci0 up >/dev/null 2>&1 || true
        systemctl start carplay-arbiter >/dev/null 2>&1; sleep 1
        systemctl start carplay-airplayd >/dev/null 2>&1
        systemctl start carplay-wireless >/dev/null 2>&1; wait_sdp || true
        hciconfig hci0 name K230-CARPLAY >/dev/null 2>&1 || true
        ;;
esac

# 7) 门禁
ok=0
case "$MODE" in
    miracast)
        active k230-miracast && ss -lnt 2>/dev/null | grep -q ':7236 ' && ok=1 ;;
    carlife)
        active k230-shared-ap && active k230-carlife-wifi && ok=1 ;;
    airplay)
        active k230-shared-ap && iw dev wlan1 info 2>/dev/null | grep -q "type AP" \
            && active bluetooth && active k230-bt-hid && hci_pscan \
            && grep -q '注册=是' /run/k230-bt-hid.status 2>/dev/null \
            && [ "$(bt_name)" = "K230-CAST" ] && ok=1 ;;
    carplay)
        active k230-shared-ap && active carplay-wireless && active carplay-airplayd && ! active bluetooth && hci_pscan \
            && sdp_serving \
            && [ "$(bt_name)" = "K230-CARPLAY" ] && ok=1 ;;
esac
if [ "$ok" != 1 ]; then
    say "✗ 门禁未通过: mode=$MODE (AP/P2P/BT 自检失败)"
    exit 1
fi
echo "$MODE" > "$STATE"
echo "$MODE" > "$PERSIST"
say "✅ 模式 = $MODE (WiFi 域=$td, 蓝牙已联动)"
exit 0
