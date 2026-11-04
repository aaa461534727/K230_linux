#!/usr/bin/env python3
"""rtsp_ts_client.py — 模拟 Android 手机 (stagefright) 推 MPEG-TS 的最小 RTSP 客户端

用途: 不需要真机, 就能回归验证 sink 的:
  - 服务端应答 (OPTIONS / GET_PARAMETER / SET_PARAMETER / SETUP / PLAY)
  - RTP payload type 33 (MPEG-TS) 解包 → H.264 dump
流程严格照抄 K230/captures/p2p0.pcap 里那台能投屏的手机:
  OPTIONS → GET_PARAMETER(问能力) → SET_PARAMETER(M3 选格式) →
  SET_PARAMETER(trigger=SETUP) → SETUP → PLAY → RTP(PT=33)

用法:
  python3 rtsp_ts_client.py --host 127.0.0.1 --ts /tmp/t.ts [--rtsp 7236]

配合:
  ./wfd-sink -p 7236 -d /tmp/sink_ts.h264 -t 25
  ffprobe -count_frames /tmp/sink_ts.h264     # 应与源 TS 帧数一致
"""
import argparse
import socket
import sys
import time


def rtsp_connect(host, port, timeout=5.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    return s


def recv_message(sock):
    """读一条 RTSP 消息 (headers + 可选 body, 用 Content-Length 判断)"""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            return None, b""
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    while len(rest) < clen:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return head.decode("utf-8", "replace"), rest[:clen]


class Rtsp:
    def __init__(self, sock):
        self.sock = sock
        self.cseq = 0

    def request(self, method, url, headers=None, body=""):
        self.cseq += 1
        lines = [f"{method} {url} RTSP/1.0",
                 "User-Agent: stagefright/1.2 (Linux;Android 16)",
                 f"CSeq: {self.cseq}"]
        for k, v in (headers or {}).items():
            lines.append(f"{k}: {v}")
        if body:
            lines.append(f"Content-Length: {len(body.encode())}")
        msg = "\r\n".join(lines) + "\r\n\r\n" + body
        self.sock.sendall(msg.encode())
        head, data = recv_message(self.sock)
        code = 0
        if head:
            parts = head.split("\r\n")[0].split()
            if len(parts) >= 2 and parts[1].isdigit():
                code = int(parts[1])
        return code, head or "", data.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--rtsp", type=int, default=7236)
    ap.add_argument("--ts", required=True, help="MPEG-TS 文件 (ffmpeg -f mpegts 生成)")
    ap.add_argument("--duration", type=float, default=8.0)
    ap.add_argument("--rate", type=float, default=1500.0,
                    help="发包速率 (RTP 包/秒); 0 = 不限速")
    args = ap.parse_args()

    ts = open(args.ts, "rb").read()
    pkt = 7 * 188                          # Miracast 常见 RTP 载荷 (7 个 TS 包)
    chunks = [ts[i:i + pkt] for i in range(0, len(ts), pkt)]

    sock = rtsp_connect(args.host, args.rtsp)
    r = Rtsp(sock)
    url = f"rtsp://{args.host}/wfd1.0"

    code, _, _ = r.request("OPTIONS", "*", {"Require": "org.wfa.wfd1.0"})
    print("M1 OPTIONS     →", code)

    q = "wfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\nwfd_uibc_capability\r\n"
    code, head, body = r.request("GET_PARAMETER", url, {}, q)
    print("M2 GET_PARAMETER →", code)
    rtp_port = 0
    for line in body.split("\r\n"):
        if line.startswith("wfd_client_rtp_ports:"):
            rtp_port = int(line.split("unicast")[1].split()[0])
    if not rtp_port:
        print("!! sink 没给 wfd_client_rtp_ports, 用 27460 兜底")
        rtp_port = 27460
    print("   sink 通告 RTP 端口:", rtp_port)

    m3 = ("wfd_video_formats: 00 00 02 10 00000100 00000000 00000000 00 0000 0000 00 none none\r\n"
          "wfd_audio_codecs: AAC 00000001 00\r\n"
          f"wfd_presentation_URL: rtsp://{args.host}/wfd1.0/streamid=0 none\r\n"
          f"wfd_client_rtp_ports: RTP/AVP/UDP;unicast {rtp_port} 0 mode=play\r\n"
          "wfd_uibc_setting: disable\r\n")
    code, _, _ = r.request("SET_PARAMETER", url, {}, m3)
    print("M3 SET_PARAMETER →", code)

    code, _, _ = r.request("SET_PARAMETER", url, {}, "wfd_trigger_method: SETUP\r\n")
    print("M4 trigger SETUP →", code)

    code, head, _ = r.request("SETUP", f"{url}/streamid=0",
                              {"Transport": "RTP/AVP/UDP;unicast;client_port=50000"})
    print("M5 SETUP       →", code)
    session = ""
    for line in (head or "").split("\r\n"):
        if line.lower().startswith("session:"):
            session = line.split(":", 1)[1].split(";")[0].strip()

    code, _, _ = r.request("PLAY", f"{url}/streamid=0", {"Session": session})
    print("M6 PLAY        →", code)

    # ---- 发 RTP (PT=33) ----
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, rtp_port)
    t0 = time.time()
    seq = 0
    sent = 0
    interval = (1.0 / args.rate) if args.rate > 0 else 0.0
    for c in chunks:
        hdr = bytes([0x80, 33, (seq >> 8) & 0xff, seq & 0xff,
                     0, 0, 0x2d, 0x1e,        # timestamp (随意)
                     0xde, 0xad, 0xbe, 0xef])  # SSRC
        us.sendto(hdr + c, dst)
        seq = (seq + 1) & 0xffff
        sent += 1
        if seq % 30 == 0:
            pass
        if interval:
            time.sleep(interval)
        if time.time() - t0 > args.duration:
            break
    print(f"RTP 已发 {sent} 包 / {len(ts)} 字节源")
    time.sleep(1.0)
    try:
        r.request("TEARDOWN", f"{url}/streamid=0", {"Session": session})
    except Exception:
        pass
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
