#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_phone_rtsp_server.py — 冒充安卓手机 (WFD Source)，按 captures/p2p0.pcap 的**真实顺序**
给 wfd-sink 的 `-c` (RTSP 客户端) 模式做离线自测。

为什么要这个: 抓包里手机 (192.168.49.1) 是 P2P GO 并且**自己就是 RTSP 服务端** (:7236,
Server: stagefright/1.2)，海奇 sink (192.168.49.128) 反过来当 RTSP 客户端。
真机没法无人值守反复点，所以这里把手机的台词照抄一遍，用来验证 sink 的握手/收流。

抓包原始台词 (帧号→内容):
  45  手机 200 SYN+ACK (监听 7236)
  48  → OPTIONS * (Require: org.wfa.wfd1.0)
  51  ← 200 Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER
  52  ← OPTIONS *  (sink 发起, 200 由手机回)
  55  → 200 Public: ... SETUP, TEARDOWN, PLAY, PAUSE ...
  56  → GET_PARAMETER (问 wfd_video_formats/audio_codecs/client_rtp_ports)
  58  ← 200 (能力: video_formats/audio/client_rtp_ports 42030/uibc none)
  64  → SET_PARAMETER (选定格式 + wfd_presentation_URL + client_rtp_ports)
  65  ← 200
  66  → SET_PARAMETER (wfd_trigger_method: SETUP)
  67  ← 200
  68  ← SETUP rtsp://192.168.49.1/wfd1.0/streamid=0 (Transport client_port=42030)
  72  → 200 Session: ...;timeout=60  Transport: ...;server_port=48136
  73  ← PLAY (Session: ...)
  78  → 200 Range: npt=now-
  ==> RTP PT=33 从 server_port → client_port

用法:
  python3 fake_phone_rtsp_server.py --port 12736 --ts /tmp/t.ts [--rtp-port 42031]

  # 另开一个终端:
  ./wfd-sink -c 127.0.0.1:12736 -r 42031 -d /tmp/rx.h264 -t 25 -v
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time

SESSION_ID = 1605031605
SERVER_PORT = 48136


class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def send(self, text):
        self.sock.sendall(text.encode("utf-8", "replace"))

    def _read_more(self, timeout):
        self.sock.settimeout(timeout)
        d = self.sock.recv(65536)
        if not d:
            raise EOFError("对端关闭")
        self.buf += d

    def read_msg(self, timeout=5.0):
        """返回 ('req', method, uri, headers, body) 或 ('rsp', code, headers, body)"""
        deadline = time.time() + timeout
        while True:
            idx = self.buf.find(b"\r\n\r\n")
            if idx >= 0:
                head = self.buf[:idx].decode("utf-8", "replace")
                rest = self.buf[idx + 4:]
                headers = {}
                lines = head.split("\r\n")
                first = lines[0]
                for ln in lines[1:]:
                    if ":" in ln:
                        k, v = ln.split(":", 1)
                        headers[k.strip().lower()] = v.strip()
                clen = int(headers.get("content-length", "0") or 0)
                if len(rest) < clen:
                    self._read_more(0.5)
                    continue
                body = rest[:clen].decode("utf-8", "replace")
                self.buf = rest[clen:]
                if first.startswith("RTSP/"):
                    code = int(first.split()[1])
                    return ("rsp", code, headers, body)
                parts = first.split()
                return ("req", parts[0], parts[1] if len(parts) > 1 else "", headers, body)
            self._read_more(max(0.2, deadline - time.time()))
            if time.time() > deadline:
                raise TimeoutError("等报文超时")


def make_rtp_packets(ts_bytes, pkt_per_rtp=7):
    """把 MPEG-TS 切成 RTP 负载 (188*7=1316 字节, 与真实手机一致量级)"""
    chunk = 188 * pkt_per_rtp
    for i in range(0, len(ts_bytes), chunk):
        yield ts_bytes[i:i + chunk]


def stream_rtp(ts_bytes, dst_ip, dst_port, fps=30, duration=10.0, verbose=True,
              pps=0, frames=0):
    """按恒定包速率推 RTP。

    2026-10-27 修 (原来 --fps 是**死参数**):
      旧代码是 per_sec = min(len(pkts), 200) —— 速率被硬顶在 200 包/s, 与 --fps 无关。
      720p 一路约 4.8 包/帧 ⇒ 实际只有 ~41 fps, 而 RTP 时间戳每包 +=3600 (40 ms)
      与墙钟差了近 10 倍。后果: (a) 用 --fps 60 测出来的「60fps 链路」其实是 41fps;
      (b) 想测 sink 解复用上限时把 --fps 写成 500 也毫无作用。
      现在: 给了 --pps 就按包速率发; 给了 --frames (TS 里总帧数) 就按 fps 反算包速率
      并把 RTP 时间戳步进改成 90000/fps/每帧包数 (真·实时); 都没给才退回旧行为。
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 0
    ts = 0
    pkts = list(make_rtp_packets(ts_bytes))
    if not pkts:
        return 0
    ts_step = 3600
    if pps > 0:
        per_sec = pps
    elif frames > 0:
        ppf = max(1.0, len(pkts) / float(frames))       # 每帧 RTP 包数
        per_sec = max(1, int(round(fps * ppf)))
        ts_step = max(1, int(round(90000.0 / max(1, fps) / ppf)))
    else:
        per_sec = min(len(pkts), 200)
    delay = 1.0 / max(per_sec, 1)
    t0 = time.time()
    next_t = t0
    sent = 0
    while time.time() - t0 < duration:
        for p in pkts:
            now = time.time()
            if now - t0 >= duration:
                break
            # 2026-10-27: 原来是无条件 time.sleep(delay) —— 板端 python 每次 sleep 至少
            # 多睡 1 ms (定时器粒度), 要求 286 包/s 时实际只跑到 ~211 包/s ⇒ 「60fps
            # 源」其实只有 43fps, 测不出链路能不能吃 60。改成按绝对节拍睡: 落后就
            # 立刻补发, 追上再睡 —— 只要 sendto 本身够快就能跑满设定速率。
            if now < next_t:
                time.sleep(next_t - now)
            hdr = struct.pack("!BBHII", 0x80, 33, seq & 0xFFFF, ts & 0xFFFFFFFF, 0x5F5F5F5F)
            s.sendto(hdr + p, (dst_ip, dst_port))
            seq += 1
            ts += ts_step
            sent += 1
            next_t += delay
    s.close()
    if verbose:
        print("[phone] RTP 发完: %d 包 → %s:%d" % (sent, dst_ip, dst_port), flush=True)
    return sent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=12736, help="本机冒充手机的 RTSP 端口")
    ap.add_argument("--ts", default="/tmp/t.ts", help="要推的 MPEG-TS 文件")
    ap.add_argument("--rtp-port", type=int, default=None,
                    help="强制 RTP 目标端口 (默认用 sink 在 wfd_client_rtp_ports 里报的)")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--pps", type=int, default=0,
                    help="RTP 包速率 (包/s); >0 时覆盖 --fps, 用于打满 sink 测上限")
    ap.add_argument("--frames", type=int, default=0,
                    help="TS 里总帧数; 给了才能把 --fps 换算成真实包速率")
    args = ap.parse_args()

    ts_bytes = open(args.ts, "rb").read()
    print("[phone] TS %d 字节 (%d 个 RTP 包)" % (len(ts_bytes), (len(ts_bytes) + 1315) // 1316),
          flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print("[phone] RTSP 服务端监听 :%d (冒充安卓手机 stagefright)" % args.port, flush=True)

    srv.settimeout(120)
    sock, addr = srv.accept()
    print("[phone] sink 已连入: %s:%d" % (addr[0], addr[1]), flush=True)
    c = Conn(sock)

    state = "sent_options"
    client_port = args.rtp_port or 42030
    rtp_started = False
    ok_count = 0

    # 帧48: 手机先发 OPTIONS
    c.send("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\nRequire: org.wfa.wfd1.0\r\n\r\n")

    while True:
        try:
            msg = c.read_msg(timeout=8.0)
        except (TimeoutError, EOFError) as e:
            print("[phone] 读报文结束: %s" % e, flush=True)
            break

        if msg[0] == "req":
            _, method, uri, hdr, body = msg
            cseq = hdr.get("cseq", "0")
            print("[phone] <== %s %s (CSeq %s)" % (method, uri, cseq), flush=True)
            if method == "OPTIONS":
                c.send("RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                       "Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, "
                       "GET_PARAMETER, SET_PARAMETER\r\n\r\n" % cseq)
            elif method == "SETUP":
                tr = hdr.get("transport", "")
                for part in tr.replace(",", ";").split(";"):
                    if part.strip().startswith("client_port="):
                        client_port = int(part.strip().split("=")[1].split("-")[0])
                print("[phone]     SETUP client_port=%d" % client_port, flush=True)
                c.send("RTSP/1.0 200 OK\r\nCSeq: %s\r\n"
                       "Session: %d;timeout=60\r\n"
                       "Transport: RTP/AVP/UDP;unicast;client_port=%d;server_port=%d\r\n\r\n"
                       % (cseq, SESSION_ID, client_port, SERVER_PORT))
            elif method == "PLAY":
                c.send("RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: %d;timeout=60\r\n"
                       "Range: npt=now-\r\n\r\n" % (cseq, SESSION_ID))
                print("[phone] ✅ PLAY 完成, 开始发 RTP 到 %s:%d" % (addr[0], client_port), flush=True)
                if not rtp_started:
                    rtp_started = True
                    t = threading.Thread(target=stream_rtp,
                                         args=(ts_bytes, addr[0], client_port),
                                         kwargs={"fps": args.fps, "duration": args.duration,
                                                 "pps": args.pps, "frames": args.frames},
                                         daemon=True)
                    t.start()
            elif method == "GET_PARAMETER":
                c.send("RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n" % cseq)
            elif method == "TEARDOWN":
                c.send("RTSP/1.0 200 OK\r\nCSeq: %s\r\n\r\n" % cseq)
                break
            else:
                c.send("RTSP/1.0 501 Not Implemented\r\nCSeq: %s\r\n\r\n" % cseq)

        else:
            _, code, hdr, body = msg
            cseq = hdr.get("cseq", "?")
            print("[phone] ==> %d (CSeq %s)" % (code, cseq), flush=True)
            if code != 200:
                print("[phone] ❌ 期望 200 却拿到 %d" % code, flush=True)
                break
            ok_count += 1

            if state == "sent_options":
                # 帧56: 问 sink 能力
                body_q = ("wfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\n"
                          "wfd_mtk_video_formats\r\nwfd_uibc_capability\r\nwfd_xm_extension\r\n")
                c.send("GET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\nCSeq: 2\r\n"
                       "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\n%s"
                       % (len(body_q), body_q))
                state = "sent_getparam"

            elif state == "sent_getparam":
                # 从 sink 的能力里取它报的 RTP 端口
                if not args.rtp_port:
                    for ln in body.splitlines():
                        if ln.startswith("wfd_client_rtp_ports"):
                            seg = ln.split()[1] if len(ln.split()) > 1 else ""
                            for tok in ln.replace(";", " ").split():
                                if tok.isdigit() and int(tok) >= 1024:
                                    client_port = int(tok)
                                    break
                    print("[phone]     sink 报的 RTP 端口 = %d" % client_port, flush=True)
                # 帧64: 选格式 + 指定播放地址
                sel = ("wfd_video_formats: 00 00 02 10 00000100 00000000 00000000 00 0000 0000 00 "
                       "none none\r\n"
                       "wfd_audio_codecs: AAC 00000001 00\r\n"
                       "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n"
                       "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d 0 mode=play\r\n"
                       "wfd_uibc_setting: disable\r\n" % (addr[0], client_port))
                c.send("SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\nCSeq: 3\r\n"
                       "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\n%s"
                       % (len(sel), sel))
                state = "sent_setparam"

            elif state == "sent_setparam":
                trig = "wfd_trigger_method: SETUP\r\n"
                c.send("SET_PARAMETER rtsp://localhost/wfd1.0 RTSP/1.0\r\nCSeq: 4\r\n"
                       "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\n%s"
                       % (len(trig), trig))
                state = "sent_trigger"

            elif state == "sent_trigger":
                pass
            else:
                pass

            if rtp_started and state == "sent_trigger":
                pass

    sock.close()
    srv.close()
    print("[phone] 结束 (收到 %d 个 200)" % ok_count, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
