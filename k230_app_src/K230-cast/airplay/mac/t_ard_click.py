
#!/usr/bin/env python3
import sys, time
sys.path.insert(0, "/tmp")
from ardvnc import ArdVNC
from shot2bmp import ppm2bmp

v = ArdVNC()
v.update()
ppm2bmp(v.save_ppm("/tmp/c0.ppm"), "/tmp/c0.bmp")
print("click apple menu (12,12)")
v.click(12, 12)
time.sleep(1.2)
v.update()
ppm2bmp(v.save_ppm("/tmp/c1.ppm"), "/tmp/c1.bmp")
print("done")
