# K230 — Miracast (WFD) 移植源码

> 目标: K230 当 **Sink** (被投屏端) + 反控, 对标海奇 E3103 的 `libmiracast.so` 闭源实现。
> 进度/计划/踩坑全在 [`../../docs/MIRACAST_PORT.md`](../../docs/MIRACAST_PORT.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `sink/wfd_sink.c` | **Sink 端 (单文件, 零依赖)**: M1~M8 RTSP 状态机 + `wfd_video_formats` 协商 + RTP 收流 + FU-A/STAP-A 重组 + SPS 位流解析 + `/tmp/rx.h264` 落盘 |
| `sink/drmplay.c` | **上屏 (裸 drm ioctl, 零依赖)**: AR24 dumb buffer + SETCRTC; 从 stdin 读 32bpp 帧写显存 |
| `sink/Makefile` | 本机 `make`; 板端交叉编见下 |
| `tools/fetch-source-ref.sh` | 拉参考 **Source** 实现 (PC 侧, 用来对打, 见 `tools/README` 注释) |
| `tools/interop-test.sh` | 一键对打: PC `wfd-source` → 目标机 `wfd-sink` |

## 编译

```sh
cd K230/src/K230-cast/miracast/sink

# 本机 (x86, 快速改代码 / 语法验证)
make

# K230 (riscv64 静态, 用 SDK 里自带的交叉链)
TC=../../../../sdk/k230_linux_sdk/output/k230_canmv_01studio_defconfig/host/bin/riscv64-unknown-linux-gnu-gcc
make CC=$TC CFLAGS="-O2 -g -static -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter"
```

> 产物默认生成在 `sink/` 下; 要入仓请不要把二进制放进 `src/` —— 拷到 `K230/build/miracast-sink/`
> (该目录被 `K230/.gitignore` 忽略), 或直接放进 Buildroot overlay 的 `usr/bin/`。

## 运行

```sh
# 目标机 = sink (默认 RTSP 7236)
wfd-sink -d /tmp/rx.h264 -t 60

# PC = source
./wfd-source stream -ip <sink_ip> -rtsp 7236 -rtp 49152 -720

# 出画面 (板端, 推荐): 硬解直通 720p→480x800 等比缩放
# 收流 (sink) → 硬解 (v4l2_dec) → 上屏 (drmplay), 全静态 riscv64
tools/build 见下方“交叉编译”
./wfd-sink -d /dev/stdout 2>/tmp/live.log \
  | ./v4l2_dec /dev/stdin /dev/stdout -H 2>/tmp/dec.log \
  | ./drmplay -H -p nv12 -w 480 -h 800 -r 30
```

## 硬解状态 (2026-10-20 已打通)

- `tools/v4l2_dec.c` = 自写 V4L2 M2M 解码器 (静态, 零第三方库), 自带 SPS 定分辨率 + AU 边界打 `END_OF_FRAME`
- 实测: 720p 干净流 60/60 帧、与 ffmpeg 软解 **PSNR=inf**; 吞吐 **720p 90.8 fps / 1080p 47.6 fps**
- 活体全链路 (PC `wfd-source` → 板 `wfd-sink | v4l2_dec -H | drmplay -H`): sink 173 帧 PASS / 解码 118 帧 / 上屏 118 帧
- 集成进两套固件并开机自启 (`S96miracast` / `miracast.service`), 见 `docs/MIRACAST_PORT.md` §8.9 与 `docs/FIRMWARE_BUILDROOT.md`

## 驱动 / wpa_supplicant 补丁现在归 SDK 仓 (2026-10-23 起)

> **一句话**: 本目录 `patches/` 与 `driver/` 下的 `.patch` 是**文档副本**;
> **真正被编译使用的权威副本在 SDK 仓** `buildroot-overlay/package/` 里, 由 buildroot 解包后自动 `patch -p1`。

原因: 这些改动原先只打在 `sdk/.../output/<conf>/build/` 里, 而 `output/` 被 `.gitignore` 掉,
`make <pkg>-dirclean` / `make clean` 一次就全丢, **换机器 clone 也复现不了**。

| 包 | SDK 仓内的补丁 | 本仓文档 |
|---|---|---|
| rtl8189fs | `K230/sdk/k230_linux_sdk/buildroot-overlay/package/rtl8189fs/0100-k230-miracast.patch` | `driver/README.md` (补丁 1~4) |
| wpa_supplicant 2.11 | `K230/sdk/k230_linux_sdk/buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch` | `patches/README.md` (§1 ~ §1.3) |

- SDK 仓: `ssh://git@ssh.github.com:443/aaa461534727/K230_linux.git` (分支 `main`)
- 换机器完整复原步骤 (拉仓 → 编译 → 上板): SDK 仓 `docs/K230_MIRACAST_PORT.md`
- 改了补丁要**两边同步**: 改 SDK 仓那份 → 重新生成本仓的文档副本 → `md5sum` 对齐 (见下)

```bash
SDK=K230/sdk/k230_linux_sdk

# 1) SDK 仓那份补丁的指纹 (换机器后应完全一致)
md5sum $SDK/buildroot-overlay/package/*/0100-k230-miracast.patch
#    rtl8189fs        : 0ffc54f01b47e4718909dfc96a1af605
#    wpa_supplicant   : 8b116fec13e7c55707d7129aa12660ef

# 2) 补丁里 K230 标记条数 (当前: rtl8189fs 16 行 / wpa_supplicant 13 行)
grep -c "K230" $SDK/buildroot-overlay/package/rtl8189fs/0100-k230-miracast.patch
grep -c "K230" $SDK/buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch

# 3) 编完确认真的打进去了
ls $SDK/output/k230_canmv_01studio_defconfig/build/wpa_supplicant-2.11/.stamp_patched
```

## 已知限制

- 旧版软解路径仍可用 (`ffmpeg -vf scale=480:800` + `drmplay`), 但只有 14 fps (swscale 是瓶颈), 仅在调试时用
- `canaan-drm` **只认 AR24/NV12 等**, 不认 XRGB8888 ⇒ 别指望 SDL2 kmsdrm / ffmpeg fbdev 直接上屏
- UIBC (反控) 与音频播放**未实现** (阶段 4/5)
