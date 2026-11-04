################################################################################
# Miracast Sink (阶段 1: RTSP Sink + RTP 收流) — 本机 & 交叉编译
#
# 本机编译:                 make
# K230 (riscv64) 交叉编译:  make CROSS=riscv64-linux-gnu-
# 清理:                     make clean
#
# 依赖: 无第三方库 (POSIX socket)
################################################################################

CROSS   ?=
CC      := $(CROSS)gcc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_GNU_SOURCE $(WARN)

BIN  = wfd-sink
BIN2 = drmplay
BIN3 = pcmplay
BIN4 = audio_out_test
BIN5 = ai2dplay

# vgplay: 硬件缩放上屏 (VGLite GPU), 只有 riscv64 板端才有 libvg_lite.so
# 本机 (x86) 不编; 交叉编译见下 (不要带 -static, 它要动态链 libvg_lite)
VG_SYSROOT ?= ../../../../sdk/k230_linux_sdk/output/k230_canmv_01studio_defconfig/host/riscv64-buildroot-linux-gnu/sysroot

.PHONY: all clean hw
all: $(BIN) $(BIN2) $(BIN3) $(BIN5)

$(BIN): wfd_sink.c ts_demux.c ts_demux.h uibc.c uibc.h audio_out.c audio_out.h
	$(CC) $(CFLAGS) -o $@ wfd_sink.c ts_demux.c uibc.c audio_out.c -lpthread

# pcmplay: 常驻 ALSA 播放器 (吃 /run/wfd-audio.pcm 的 S16LE/48k/2ch)。
# 交叉编译时用板端自带的 libasound.so.2, 头文件取 SDK sysroot。
ALSA_SYSROOT ?= $(VG_SYSROOT)
pcmplay: ../tools/pcmplay.c
	$(CC) $(CFLAGS) -I$(ALSA_SYSROOT)/usr/include -o $@ $< \
	    -L$(ALSA_SYSROOT)/usr/lib -lasound

# drmplay: 裸 ioctl 上屏 (AR24), 静态链接好随镜像带走, 不依赖目标 libdrm
# drmplay 的 NV12→ARGB 用 RVV 向量化 (C908 = rv64imafdcv, 实测快 3~3.8 倍)。
# 只在 riscv 工具链上加 -march (本机 x86 编译不认识该参数);
# 源码里 __riscv_vector 不成立时自动退回标量, 行为完全一致。
ifneq (,$(findstring riscv,$(shell $(CC) -dumpmachine)))
DRM_ARCH ?= -march=rv64gcv
endif

drmplay: drmplay.c
	$(CC) $(CFLAGS) $(DRM_ARCH) -static -o $@ $<

# ai2dplay: 方案A —— 硬件 NV12→AR24 + 缩放(AI2D M2M) 上屏, 替代 drmplay 的 CPU 转码
# 只用 libc + 裸 ioctl (DRM + V4L2), 静态链接随镜像带走
ai2dplay: ai2dplay.c
	$(CC) $(CFLAGS) $(DRM_ARCH) -static -o $@ $<

# 硬件缩放播放器 (板端): make hw CC=<riscv64 gcc>
hw: vgplay
vgplay: vgplay.c
	$(CC) $(CFLAGS) -I$(VG_SYSROOT)/usr/include -o $@ $< -L$(VG_SYSROOT)/usr/lib -lvg_lite

clean:
	rm -f $(BIN) $(BIN2) $(BIN3) $(BIN5) vgplay

# 离线自测: TS(PT=33) → H.264 ES 是否与参考一致 (host 编译, 不需要板子)
# 用法: make test-ts
#   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 -c:v libx264 \
#          -g 30 -bf 0 -f mpegts /tmp/t.ts && make test-ts
.PHONY: test-ts
TEST_TS ?= /tmp/t.ts
test-ts: ts_demux.c ts_demux.h ../tools/ts_demux_test.c
	$(CC) $(CFLAGS) -o /tmp/ts_demux_test ../tools/ts_demux_test.c ts_demux.c
	/tmp/ts_demux_test $(TEST_TS) /tmp/out.h264
