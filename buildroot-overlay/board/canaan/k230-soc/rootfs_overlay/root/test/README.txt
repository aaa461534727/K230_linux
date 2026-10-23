# 硬解 (mvx) 验证素材 —— 刷 Buildroot 镜像后直接用

## 1. 厂商解码器
mvx_decoder /root/test/t.ivf /tmp/out.yuv
# 期望: "Total size" = 640*360*1.5*90 ≈ 31104000 字节
ls -l /tmp/out.yuv

## 2. 备选参数
mvx_decoder --one_frame_per_packet 1 --frames 90 /root/test/t.ivf /tmp/out.yuv
mvx_decoder -o yuv420_nv12 /root/test/t.ivf /tmp/out.yuv

## 3. 对比: ffmpeg 软解 (证明输入文件本身没问题)
ffmpeg -i /root/test/t720.h264 -f rawvideo -pix_fmt yuv420p /tmp/sw.yuv
ls -l /tmp/sw.yuv

## 4. 驱动是否认到
dmesg | grep -i mvx
ls /dev/video*
ls /usr/bin/mvx_*
