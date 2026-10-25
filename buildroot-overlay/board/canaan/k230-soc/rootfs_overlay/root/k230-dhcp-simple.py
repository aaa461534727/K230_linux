#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# =============================================================================
# k230-dhcp-simple.py — 极简 DHCP 客户端 (只做 DISCOVER/OFFER/REQUEST/ACK)
#
# 干什么:
#   在 P2P 组接口 (wlan1) 上手工跑一次完整 DORA, 拿到手机 GO (192.168.49.1)
#   发的租约并直接 `ip addr add`, 绕开 dhcpcd 的 ARP 探测。
#
# 为什么 (2026-10-24 16:16 实测):
#   手机 DHCP 服务器会对 "它刚 OFFER 出去的地址" 回 proxy ARP reply;
#   dhcpcd 10 在 REQUEST 前先做 ARP probe (RFC 5227), 收到 reply 就判
#   "地址被占用" → 放弃/重来, **永远不发 REQUEST** ⇒ 租约不完成
#   ⇒ 手机 WFD 栈不知道 sink 的 IP ⇒ 手机不发起 RTSP (0 个 SYN)。
#
# 怎么用:
#   python3 k230-dhcp-simple.py wlan1 [超时秒=18]
#   成功: 打印 "IP=... SERVER=... LEASE=..." 并返回 0 (地址已配好)
#   失败: 打印 "DHCP-FAIL: ..." 并返回 1
#
# 依赖: Linux + root + python3 (板端 3.12, 只用标准库); AF_PACKET 原始套接字
# 在哪跑: K230 板子 (root); 只改运行时地址, 不写 /root、不改任何板端文件
# 产出: stdout (调用方重定向进 /tmp/*.log)
# =============================================================================
import os
import random
import socket
import struct
import subprocess
import sys
import time

ETH_P_IP = 0x0800

MSG = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK",
       6: "NAK", 7: "RELEASE", 8: "INFORM"}


def csum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def build_dhcp(xid: int, mac: bytes, mtype: int, extra: bytes = b"") -> bytes:
    p = struct.pack("!BBBBIHH", 1, 1, 6, 0, xid, 0, 0x8000)  # op/htype/hlen/hops/xid/secs/flags
    p += b"\x00" * 4   # ciaddr
    p += b"\x00" * 4   # yiaddr
    p += b"\x00" * 4   # siaddr
    p += b"\x00" * 4   # giaddr
    p += mac + b"\x00" * 10
    p += b"\x00" * 64  # sname
    p += b"\x00" * 128  # file
    p += b"\x63\x82\x53\x63"  # magic cookie
    p += bytes([53, 1, mtype])
    p += bytes([61, 7, 1]) + mac           # client identifier
    p += bytes([12, 4]) + b"k230"          # hostname
    p += bytes([55, 6, 1, 3, 6, 15, 28, 26])  # param request list
    p += extra
    p += b"\xff"
    return p


def build_frame(mac: bytes, payload: bytes) -> bytes:
    udp = struct.pack("!HHHH", 68, 67, 8 + len(payload), 0) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), random.randint(1, 0xFFFE),
                     0, 64, 17, 0,
                     socket.inet_aton("0.0.0.0"), socket.inet_aton("255.255.255.255"))
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return b"\xff" * 6 + mac + struct.pack("!H", ETH_P_IP) + ip + udp


def parse_dhcp(frame: bytes):
    if len(frame) < 14 + 20 + 8 + 240:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_P_IP:
        return None
    ip = frame[14:]
    if (ip[0] >> 4) != 4 or ip[9] != 17:
        return None
    ihl = (ip[0] & 0x0F) * 4
    udp = ip[ihl:]
    if len(udp) < 8:
        return None
    sp, dp, ln = struct.unpack("!HHH", udp[:6])
    if sp != 67 or dp != 68:
        return None
    p = udp[8:8 + max(0, ln - 8)]
    if len(p) < 240:
        return None
    opts = {}
    i = 240
    while i < len(p):
        o = p[i]
        if o == 255:
            break
        if o == 0:
            i += 1
            continue
        if i + 1 >= len(p):
            break
        l = p[i + 1]
        opts[o] = p[i + 2:i + 2 + l]
        i += 2 + l
    mt = opts.get(53, b"\x00")[0]
    return {
        "xid": struct.unpack("!I", p[4:8])[0],
        "yiaddr": p[16:20],
        "opts": opts,
        "type": mt,
    }


def wait_for(sock, xid: int, want: int, timeout: float):
    end = time.time() + timeout
    while time.time() < end:
        sock.settimeout(max(0.2, end - time.time()))
        try:
            fr = sock.recv(4096)
        except socket.timeout:
            return None
        d = parse_dhcp(fr)
        if not d or d["xid"] != xid or d["type"] != want:
            continue
        return d
    return None


def main() -> int:
    iface = sys.argv[1] if len(sys.argv) > 1 else "wlan1"
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0

    mac_s = open("/sys/class/net/%s/address" % iface).read().strip()
    mac = bytes(int(x, 16) for x in mac_s.split(":"))
    print("DHCP: iface=%s mac=%s timeout=%.0fs" % (iface, mac_s, timeout), flush=True)

    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_IP))
    s.bind((iface, 0))

    xid = random.randint(1, 0xFFFFFFFF)
    offer = None
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.send(build_frame(mac, build_dhcp(xid, mac, 1)))
        print("  -> DISCOVER xid=0x%08x" % xid, flush=True)
        offer = wait_for(s, xid, 2, 3.0)
        if offer:
            break
    if not offer:
        print("DHCP-FAIL: 没有收到 OFFER", flush=True)
        return 1

    yiaddr = offer["yiaddr"]
    server = offer["opts"].get(54, b"\x00" * 4)
    print("  <- OFFER ip=%s server=%s" % (socket.inet_ntoa(yiaddr), socket.inet_ntoa(server)),
          flush=True)

    extra = bytes([50, 4]) + yiaddr + bytes([54, 4]) + server
    ack = None
    for _ in range(3):
        s.send(build_frame(mac, build_dhcp(xid, mac, 3, extra)))
        print("  -> REQUEST ip=%s" % socket.inet_ntoa(yiaddr), flush=True)
        ack = wait_for(s, xid, 5, 3.0)
        if ack:
            break
    if not ack:
        print("DHCP-FAIL: 没有收到 ACK", flush=True)
        return 1

    ip = socket.inet_ntoa(ack["yiaddr"])
    lease = ack["opts"].get(51)
    lease_s = struct.unpack("!I", lease)[0] if lease and len(lease) == 4 else -1
    mask = ack["opts"].get(1, b"\xff\xff\xff\x00")
    print("  <- ACK ip=%s mask=%s lease=%ss" % (ip, socket.inet_ntoa(mask), lease_s), flush=True)

    subprocess.call(["ip", "addr", "flush", "dev", iface, "scope", "global"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = subprocess.call(["ip", "addr", "add", "%s/24" % ip, "dev", iface])
    subprocess.call(["ip", "link", "set", iface, "up"])
    if r != 0:
        print("DHCP-FAIL: ip addr add 失败", flush=True)
        return 1
    print("IP=%s SERVER=%s LEASE=%s" % (ip, socket.inet_ntoa(server), lease_s), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
