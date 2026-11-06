# AirPlay 接收 (K230-CAST)

> 苹果 iPhone/iPad/Mac 的「屏幕镜像 / AirPlay」到 K230。协议由 UxPlay 实现, **视频交给本板 VPU 硬解**,
> 音频解成 PCM 交给常驻 pcmplay 出声。与 Miracast 互斥 (仲裁在 common/k230-cast)。

## 链路

| 段 | 实现 |
|---|---|
| 协议/接收 | uxplay (HTTP/RTSP 7000 + mDNS 广播 K230-CAST + FairPlay/pair-verify) |
| 视频 | `-vdmp` 把解密后的 H.264 Annex-B 直通到命名管道 -> v4l2_dec 硬解 -> drmplay 上屏 |
| 音频 | `UXPLAY_AUDIO_PIPE=/run/wfd-audio.pcm`: GStreamer 解 ALAC/AAC-ELD -> S16LE/48k/2ch -> pcmplay |

优点: **不用 UxPlay 自带的 GStreamer 视频上屏** (单核 RISC-V 软解 720p/1080p 跑不动),
只借它的协议层 + 音频解码, 视频走本板硬解 (与 Miracast 同一条播放链路)。

## 目录

| 路径 | 内容 |
|---|---|
| `k230-airplay` | 启动脚本: 起硬解看护 + uxplay; `AIRPLAY_AUDIO` 默认 1 (开音频) |
| `k230-airplay.service` | systemd 单元 (`Environment=UXPLAY_VIDEO_PIPE=1` / `AIRPLAY_AUDIO=1`) |
| `k230-bt-hid` | 蓝牙反控主程序 (经典 HID 设备端: 触摸/键盘 → 手机; 源码真相在本目录, SDK 侧 `k230_app_src/K230-cast/airplay/` 同步, 构建时由 `k230_extras_build.sh` 装进 `/usr/bin`) |
| `k230-bt-hid-logic-test` | 触摸手势 → HID 报告的离线逻辑自测 (16 项) |
| `k230-bt-hid-int-test.py` | 中断通道 accept / GET_REPORT 离线回归 (PASS 4/4, 不需要手机) |
| `build-uxplay.sh` | 板端 native 编译 uxplay (vendored 源码 + 打 patches/) |
| `patches/` | 0001 视频管道 / 0002 落盘不缓冲 / 0003 音频管道 |
| `vendor/uxplay-master.tar.gz` | 上游 pristine 源码 (可复现) |
| `uxplay/` | **解压 + 打完三个补丁的最新源码树** (直接改这里再 build) |
| `mac/` | Mac 端 AirPlay 连接自动化 (VNC 点连接), 详见 `mac/README.md` |

## 板端依赖 (音频必需)

    apt-get install -y gstreamer1.0-plugins-base gstreamer1.0-libav

- plugins-base: appsrc / audioconvert / audioresample / volume / filesink
- libav:        avdec_alac / avdec_aac
- 不装则 uxplay 的音频 pipeline 解析失败, 一连音频就断; 这也是历史上默认 `-a` 关音频的原因。
- 单核板实测: ALAC/AAC-ELD 软解 CPU 占用极低 (远低于视频硬解链路), 故现在默认开。

## 编译 & 装机 (板上)

    apt-get install -y --no-install-recommends build-essential cmake pkg-config \
        libplist-dev libssl-dev libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev libglib2.0-dev
    sh build-uxplay.sh                 # vendored 源码 -> 打 3 补丁 -> 装 /usr/local/bin/uxplay
    cp k230-airplay /root/ && chmod +x /root/k230-airplay
    cp k230-airplay.service /etc/systemd/system/
    systemctl daemon-reload && systemctl enable --now k230-airplay.service

## 已知限制

- **必须二层同网**: iPhone/Mac 与 K230 同一 LAN (mDNS 发现 K230-CAST)。wlan1 被 Miracast P2P GO 占用时不兼做 STA。
- 音频格式: 实测 Mac 走 **AAC-ELD 44100/2**, iPhone 可能 ALAC; 两者都有对应 avdec 解码器。
- 源分辨率由客户端决定; `-s` 只是"我们能收多大"的声明。
