#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AirPlay 连接 (自适应分辨率)

之前的问题: k230mac_recast.py 里控制中心图标坐标写死 (903,12), 那是 1280x720 时量的。
Mac 改成 1920x1080 之后点空了, 一直报 "K230 not listed"。

现在: 先在菜单栏右侧横向扫描, 找到能把控制中心点开的 x 坐标 (判据是弹出层里
出现 "AirPlay"/"隔空播放"/"屏幕镜像" 这类字样), 再在里面点 K230-AirPlay。
"""
import subprocess, sys, time, os

sys.path.insert(0, "/tmp")
from ardvnc import ArdVNC

W = H = 0


def ocr(ppm, tag):
    subprocess.run(["/tmp/ocr", ppm], stdout=open(ppm + ".txt", "w"),
                   stderr=subprocess.DEVNULL, cwd="/tmp")
    rows = []
    try:
        for ln in open(ppm + ".txt"):
            p = ln.rstrip("\n").split("\t")
            if len(p) < 3:
                continue
            xywh = p[0].split()
            if len(xywh) != 4:
                continue
            x, y, w, h = (int(float(v)) for v in xywh)
            rows.append((x, y, w, h, p[2]))
    except IOError:
        pass
    return rows


def shot(v, tag):
    p = "/tmp/cc_%s.ppm" % tag
    v.update()
    v.save_ppm(p)
    return ocr(p, tag)


def has_target(rows):
    for r in rows:
        t = r[4]
        if "K230" in t:
            return r
    return None


def main():
    v = ArdVNC(host="127.0.0.1")
    v.update()
    print("screen %dx%d" % (v.w, v.h))

    # 1. 先把碍事的窗口关掉
    for _ in range(4):
        v.key(0xff1b, True); v.key(0xff1b, False); time.sleep(0.6)

    rows = shot(v, "00")
    # NoMachine 更新弹窗: 点 "Update later"
    for r in rows:
        if "Update later" in r[4] or "later" in r[4].lower():
            print("close popup at", r[:2], repr(r[4]))
            v.click(r[0] + r[2] // 2, r[1] + r[3] // 2)
            time.sleep(1.5)
            break

    # 2. 扫菜单栏右侧找控制中心
    #    菜单栏高度 ~24px, 从屏幕右边缘往左扫到 60% 宽度处
    cands = []
    x = v.w - 60
    while x > v.w * 0.55:
        # ★ 必须真的点一下菜单栏 (之前漏了这步, 只是截图 ⇒ 永远看不到弹出层)
        v.click(x, 12)
        time.sleep(1.1)
        sb = shot(v, "s%04d" % x)
        hit = has_target(sb)
        if hit:
            print("FOUND K230 directly at scan x=%d: %s" % (x, hit))
            return finish(v, hit)
        # 判据: 控制中心弹出层必然出现"显示器"/"镜像"/"隔空"
        popup = any(("镜像" in r[4]) or ("隔空播放" in r[4]) or
                    ("AirPlay" in r[4]) or ("控制中心" in r[4]) for r in sb)
        if popup:
            cands.append(x)
            print("  x=%d popup opened (rows=%d)" % (x, len(sb)))
            # 在这个弹出层里找 K230
            hit = has_target(sb)
            if hit:
                return finish(v, hit)
            for a in range(3):
                time.sleep(1.2)
                sb2 = shot(v, "w%d_%d" % (x, a))
                hit = has_target(sb2)
                if hit:
                    return finish(v, hit)
            # 没找到就关掉继续扫
            v.key(0xff1b, True); v.key(0xff1b, False); time.sleep(0.8)
        else:
            # 点开的不是控制中心 (可能是别的菜单), 关掉
            if any(r[4].strip() for r in sb if r[1] < 40):
                v.key(0xff1b, True); v.key(0xff1b, False); time.sleep(0.5)
        x -= 14

    print("控制中心候选: %s" % cands)
    print("FAIL: 没找到 K230-AirPlay")
    return 1


def finish(v, hit):
    x, y, w, h, lab = hit
    cx, cy = x + w // 2, y + h // 2
    print("click K230 at (%d,%d) label=%r" % (cx, cy, lab))
    v.click(cx, cy)
    for i in range(8):
        time.sleep(1.0)
        out = subprocess.run(["system_profiler", "SPDisplaysDataType"],
                             capture_output=True, text=True).stdout
        if "K230" in out:
            print("OK connected after %ds" % (i + 1))
            return 0
    print("clicked but no K230 display")
    return 1


if __name__ == "__main__":
    sys.exit(main())
