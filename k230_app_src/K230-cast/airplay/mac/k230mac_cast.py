#!/usr/bin/env python3
import sys, time, subprocess
sys.path.insert(0, "/tmp")
from ardvnc import ArdVNC
from shot2bmp import ppm2bmp

TARGET = (986, 216)
v = ArdVNC()
v.click(TARGET[0], TARGET[1])
print("clicked", TARGET, flush=True)
for i in range(14):
    time.sleep(1.5)
    v.update()
    bmp = "/tmp/cast%d.bmp" % i
    ppm2bmp(v.save_ppm("/tmp/cast%d.ppm" % i), bmp)
    out = subprocess.run(["/tmp/ocr", bmp], capture_output=True, text=True).stdout
    rows = []
    for ln in out.splitlines():
        if ln.startswith("IMG"): continue
        p = ln.split("\t")
        if len(p) < 3: continue
        rows.append(p[2])
    print("[%d] %s" % (i, " | ".join(rows[:14])), flush=True)
