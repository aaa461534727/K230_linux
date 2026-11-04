#!/usr/bin/env python3
import sys, time, subprocess
sys.path.insert(0, "/tmp")
from ardvnc import ArdVNC
from shot2bmp import ppm2bmp

def k230_display():
    out = subprocess.run(["system_profiler", "SPDisplaysDataType"],
                         capture_output=True, text=True).stdout
    for ln in out.splitlines():
        s = ln.strip()
        if s.startswith("K230-AirPlay:"):
            return True
    return False

def ocr_rows(v, tag):
    v.update()
    ppm2bmp(v.save_ppm("/tmp/%s.ppm" % tag), "/tmp/%s.bmp" % tag)
    txt = subprocess.run(["/tmp/ocr", "/tmp/%s.bmp" % tag],
                         capture_output=True, text=True).stdout
    rows = []
    for ln in txt.splitlines():
        if ln.startswith("IMG"): continue
        p = ln.split("\t")
        if len(p) < 3: continue
        b = p[0].split()
        if len(b) < 4: continue
        rows.append((int(float(b[0])), int(float(b[1])), int(float(b[2])), int(float(b[3])), p[2]))
    return rows

def click_target(v, tag):
    for a in range(3):
        v.key(0xff1b, True); v.key(0xff1b, False); time.sleep(0.8)
        v.click(903, 12); time.sleep(2.2)
        rows = ocr_rows(v, "%s_%d" % (tag, a))
        tgt = [r for r in rows if "K230" in r[4]]
        if tgt:
            x, y, w, h, lab = tgt[0]
            print("  OCR found %r at (%d,%d) -> click (%d,%d)" % (lab, x, y, x + w//2, y + h//2), flush=True)
            v.click(x + w//2, y + h//2)
            return True
        print("  attempt %d: K230 not listed" % a, flush=True)
        for r in rows[:10]: print("    ", r, flush=True)
        time.sleep(3)
    return False

print("state0 K230 display =", k230_display(), flush=True)
for shot in range(3):
    if k230_display():
        print("connected already", flush=True); break
    print("connect attempt", shot, flush=True)
    try:
        v = ArdVNC()
        click_target(v, "cc%d" % shot)
    except Exception as e:
        print("  vnc error:", e, flush=True)
    for i in range(10):
        time.sleep(1.5)
        if k230_display():
            print("  OK connected after %ds" % int((i+1)*1.5), flush=True); break
    if k230_display(): break
    time.sleep(2)
print("final K230 display =", k230_display(), flush=True)
