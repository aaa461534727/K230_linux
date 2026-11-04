#!/usr/bin/env python3
"""
dhcp_test.py — 自测板上的 dhcpd (不需要手机)

做法: 伪造一个 MAC, 从本机发 DHCPDISCOVER 到 255.255.255.255:67,
      在 :68 上收回 OFFER, 再发 REQUEST 收 ACK。
      收发都 SO_BINDTODEVICE 锁在指定网卡, 不会跑到别的网卡上。

用法: python3 dhcp_test.py [-i wlan0] [-m 02:11:22:33:44:55]

在板子上跑 (K230/Ubuntu): 验证 dhcpd 是否真能给客户端发 IP。
"""
import argparse
import random
import socket
import struct
import sys
import time

MAGIC = b"\x63\x82\x53\x63"
DISCOVER, OFFER, REQUEST, ACK = 1, 2, 3, 5


def build(xid, mac, msg_type, ciaddr="0.0.0.0"):
    """组一个最小 BOOTREQUEST"""
    chaddr = bytes.fromhex(mac.replace(":", "")) + b"\x00" * 10
    pkt = struct.pack(
        "!BBBBIHH4s4s4s4s16s64s128s",
        1, 1, 6, 0,                      # op, htype, hlen, hops
        xid, 0, 0x8000,                  # xid, secs, flags(broadcast)
        socket.inet_aton(ciaddr), socket.inet_aton("0.0.0.0"),
        socket.inet_aton("0.0.0.0"), socket.inet_aton("0.0.0.0"),
        chaddr, b"\x00" * 64, b"\x00" * 128,
    )
    opts = MAGIC + bytes([53, 1, msg_type]) + bytes([55, 3, 1, 3, 6]) + b"\xff"
    return pkt + opts


def parse(pkt):
    """返回 (msg_type, yiaddr, 其它 options dict)"""
    if len(pkt) < 240 or pkt[236:240] != MAGIC:
        return None, None, {}
    yiaddr = socket.inet_ntoa(pkt[16:20])
    opts, i, out = pkt[240:], 0, {}
    while i < len(opts):
        code = opts[i]
        if code == 255:
            break
        if code == 0:
            i += 1
            continue
        if i + 1 >= len(opts):
            break
        ln, val = opts[i + 1], opts[i + 2:i + 2 + opts[i + 1]]
        out[code] = val
        i += 2 + ln
    mt = out.get(53, b"\x00")[0]
    return mt, yiaddr, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--iface", default="wlan0")
    ap.add_argument("-m", "--mac", default="02:11:22:33:44:55")
    ap.add_argument("-t", "--timeout", type=float, default=5.0)
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, 25, a.iface.encode() + b"\x00")  # SO_BINDTODEVICE
    s.bind(("0.0.0.0", 68))
    s.settimeout(a.timeout)

    xid = random.getrandbits(32)
    dst = ("255.255.255.255", 67)

    print(f"[test] iface={a.iface} mac={a.mac} xid=0x{xid:08x}")

    for step, mtype, want, ciaddr in (
        ("DISCOVER", DISCOVER, OFFER, "0.0.0.0"),
        ("REQUEST", REQUEST, ACK, "0.0.0.0"),
    ):
        s.sendto(build(xid, a.mac, mtype, ciaddr), dst)
        deadline = time.time() + a.timeout
        got = None
        while time.time() < deadline:
            try:
                data, _ = s.recvfrom(2048)
            except socket.timeout:
                break
            mt, yiaddr, opts = parse(data)
            if mt == 2:      # 忽略自己可能收到的 OFFER 之外的东西
                pass
            if mt == want:
                got = (mt, yiaddr, opts)
                break
        if not got:
            print(f"[test] ✗ {step}: {a.timeout}s 内没收到 {'OFFER' if want == OFFER else 'ACK'}")
            return 1
        mt, yiaddr, opts = got
        name = "OFFER" if mt == OFFER else "ACK"
        mask = socket.inet_ntoa(opts[1]) if 1 in opts else "?"
        router = socket.inet_ntoa(opts[3]) if 3 in opts else "?"
        lease = struct.unpack("!I", opts[51])[0] if 51 in opts else "?"
        print(f"[test] ✅ {step} → {name}: yiaddr={yiaddr} mask={mask} router={router} lease={lease}s")

    s.close()
    print("[test] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
