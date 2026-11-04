#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""k230-fallback —— K230 投屏「按设备自动兜底」看护

默认永远走 client 模式 (K230 当 P2P client, 手机当 GO)。
只有当**同一时间段内连续失败 2 次**(P2P 建组后被对端删组 / 关联被拒, 且期间没有
PLAY 200 成功过), 才临时切到 GO 模式 (K230 当 GO, 手机当 client) 救一次;
GO 会话结束或窗口超时立刻切回 client。

为什么需要: Redmi K20 Pro (HyperOS/高通) 的 AP 模式在 WPS 完成后会自己删组,
client 模式永远连不上; 它当 client 则完全正常 (2026-10-29 实测 1280x720/30fps)。
其它机型 (K80/努比亚/Mac/荣耀V30) 一次就成, 永远碰不到这条分支。

状态:
  /run/miracast-mode      client | go
  /run/k230-fallback.log  本脚本日志
"""
import os, re, subprocess, sys, time

LOG = "/tmp/wfd-p2p.log"
OFF = "/run/k230-fallback.off"
MODE = "/run/miracast-mode"
FLOG = "/run/k230-fallback.log"
GO_UNIT = "k230-miracast-go.service"
CLIENT_UNIT = "k230-miracast.service"
SUP_CLIENT = "/root/wpa_supplicant.client"
SUP_GO = "/root/wpa_supplicant.go"
SUP = "/usr/sbin/wpa_supplicant"

FAIL_WINDOW = 180      # 秒: 统计窗口
FAIL_NEED = 2          # 窗口内失败次数达到就兜底
GO_IDLE_NO_STA = 150   # GO 模式: 一直没设备连进来 → 放弃
GO_IDLE_AFTER = 45     # GO 模式: 连过之后断开 N 秒 → 收工
COOLDOWN = 300         # 回 client 后的静默期, 防止来回抖


def log(msg):
    line = "[%s] %s\n" % (time.strftime("%H:%M:%S"), msg)
    with open(FLOG, "a") as f:
        f.write(line)
    print(line, end="")


def sh(cmd, timeout=25):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=timeout).stdout
    except Exception as e:
        return ""


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return default


def write(path, s):
    try:
        with open(path, "w") as f:
            f.write(s)
    except Exception:
        pass


def offset():
    try:
        return int(read(OFF, "0"))
    except Exception:
        return 0


def new_lines():
    """增量读 supplicant 日志, 返回新行列表"""
    try:
        size = os.path.getsize(LOG)
    except OSError:
        return []
    pos = offset()
    if size < pos:          # 日志被重写
        pos = 0
    if size == pos:
        return []
    if size - pos > 1048576:      # 落后太多 (调试日志刷屏) 直接跳到最近 1 MB
        pos = size - 1048576
    try:
        with open(LOG, "rb") as f:
            f.seek(pos)
            data = f.read(1048576)
    except Exception:
        return []
    write(OFF, str(pos + len(data)))
    return data.decode("utf-8", "replace").splitlines()


def station_count():
    out = sh("iw dev wlan1 station dump 2>/dev/null | grep -c '^Station'")
    try:
        return int(out.strip() or 0)
    except Exception:
        return 0


def set_mode(mode):
    write(MODE, mode + "\n")


def to_go():
    log("🔄 连续失败 %d 次 → 临时切 GO 模式 (仅本轮)" % FAIL_NEED)
    sh("systemctl stop %s" % CLIENT_UNIT)
    # client 模式的监听管线可能还占着 :7236 / wlan1, 必须收干净再起 GO
    sh("pkill -9 -f '/usr/bin/wfd-sink' 2>/dev/null; pkill -9 -f 'v4l2_dec /dev/stdin' 2>/dev/null")
    time.sleep(1)
    if os.path.exists(SUP_GO):
        sh("cp -f %s %s && chmod 755 %s" % (SUP_GO, SUP, SUP))
    set_mode("go")
    sh("systemctl start %s" % GO_UNIT)


def to_client(reason):
    log("↩ 切回 client 模式 (%s)" % reason)
    sh("systemctl stop %s" % GO_UNIT)
    if os.path.exists(SUP_CLIENT):
        sh("cp -f %s %s && chmod 755 %s" % (SUP_CLIENT, SUP, SUP))
    set_mode("client")
    sh("systemctl start %s" % CLIENT_UNIT)


def main():
    fails = []            # 失败时间戳
    mode = read(MODE, "client") or "client"
    go_t0 = time.time()
    seen_sta = False
    last_sta = 0.0
    cooldown_until = 0.0
    # 丢掉启动前的老日志
    try:
        write(OFF, str(os.path.getsize(LOG)))
    except OSError:
        pass
    while True:
        time.sleep(3)
        now = time.time()
        # 模式可能被外部改 (人工/仲裁), 每轮以文件为准
        _m = read(MODE, "client") or "client"
        if _m != mode:
            log("ℹ 模式文件变为 %s (同步)" % _m)
            mode = _m
            if mode == "go":
                go_t0 = now; seen_sta = False
            else:
                fails = []

        # ---- GO 模式: 判断何时收工 ----
        if mode == "go":
            n = station_count()
            if n > 0:
                if not seen_sta:
                    log("📶 GO 模式: 已有设备连入")
                seen_sta = True
                last_sta = now
            else:
                if seen_sta and now - last_sta > GO_IDLE_AFTER:
                    to_client("设备已断开 %ds" % GO_IDLE_AFTER)
                    mode = "client"; cooldown_until = now + COOLDOWN
                    seen_sta = False; fails = []
                    continue
                if not seen_sta and now - go_t0 > GO_IDLE_NO_STA:
                    to_client("%ds 无设备连入" % GO_IDLE_NO_STA)
                    mode = "client"; cooldown_until = now + COOLDOWN
                    fails = []
                    continue
            # GO 模式下不看失败计数
            new_lines()
            continue

        # ---- client 模式: 数失败 ----
        for ln in new_lines():
            if "reason=FORMATION_FAILED" in ln:
                fails.append(now)
            elif "✅ PLAY 200" in ln or "PLAY 200" in ln:
                fails = []       # 成功过一次, 清零
        fails = [t for t in fails if now - t <= FAIL_WINDOW]
        # 真正关联上 GO 才算这一轮成功 (注意: client 模式无论成败都会配 192.168.49.128,
        # 用 IP 判断会把失败也当成功 ⇒ 必须看 link 状态)
        if "Connected to" in sh("iw dev wlan1 link 2>/dev/null"):
            fails = []

        if now >= cooldown_until and len(fails) >= FAIL_NEED:
            to_go()
            mode = "go"; go_t0 = now; seen_sta = False; fails = []
            # GO 模式期间 supplicant 会重写同一个日志, 重新对齐偏移
            try:
                write(OFF, str(os.path.getsize(LOG)))
            except OSError:
                pass


if __name__ == "__main__":
    main()
