# K230-CAST — 投屏接收端总目录

> 2026-10-29 由 `src/miracast/` 重命名并重组。原名只体现安卓(Miracast), 实际已同时支持
> 苹果(AirPlay), 故升格为 **K230-CAST**(板上广播名 / 仲裁服务名都叫 K230-CAST)。

一块 K230 同时当 **Miracast Sink** / **AirPlay 接收端** / (新增) **CarLife 有线车机端**, 但**同一时刻只服务一台设备**
(安卓连上撤苹果广播, 苹果连上撤安卓; 仲裁见 `common/k230-cast`)。

## 目录结构

```
K230-cast/
├── miracast/      安卓 Miracast (WFD) 相关源码
│   ├── sink/        wfd-sink (RTSP/RTP/UIBC 收流) + drmplay 上屏 + Makefile
│   ├── tools/       miracast 侧工具 (dhcpd/gen_nv12/drmdump/v4l2_probe/测试脚本/wfd-autotune)
│   │                ★ wfd-autotune = 按手机自适应选频率档 (python3, 随镜像装 /usr/bin)
│   │                ★ 其中 pcmplay.c / v4l2_dec.c 为两条链路共用 (见 common 说明)
│   ├── driver/      rtl8189fs P2P/WFD 驱动补丁
│   ├── patches/     wpa_supplicant 2.11 P2P/GO 协商补丁
│   └── k230-miracast.service
├── airplay/       苹果 AirPlay 相关源码
│   ├── k230-airplay         AirPlay 接收启动脚本 (uxplay -> H.264 ES 管道)
│   ├── k230-airplay.service systemd 单元 (AIRPLAY_AUDIO=1 默认开音频)
│   ├── k230-bt-hid          蓝牙反控 (经典 HID 设备端: 触摸/键盘 -> 手机)
│   ├── k230-bt-hid-logic-test  触摸手势 → HID 报告的离线逻辑自测
│   ├── k230-bt-hid-int-test.py 中断通道 accept/GET_REPORT 离线回归 (PASS 4/4)
│   ├── build-uxplay.sh      板端 native 编译 uxplay 的脚本 (打 patches/)
│   ├── patches/             uxplay 3 个 K230 补丁 (视频管道/落盘不缓冲/音频管道)
│   ├── vendor/              uxplay 上游源码 tarball (pristine, 可复现)
│   ├── uxplay/              解压 + 打完全部补丁后的**最新源码树** (可直接改/编)
│   └── mac/                 **Mac 端 AirPlay 自动化 (VNC 点连接) 方案** -> 见其 README
├── carlife/       **百��� CarLife 车机端 (有线 USB AOA)** -> 见 docs/PROTOCOL.md
│   ├── carlife_usb.c        AOA 2.0 握手 (51/52/53) + accessory bulk 收发
│   ├── carlife_proto.c      私有帧 (channel/len + inner) + 极简 protobuf
│   ├── carlife_sink.c       会话状态机 + 视频/音频/触摸桥接
│   ├── build.sh             板上本机编译 (gcc + libusb-1.0-dev)
│   └── k230-carlife.service systemd 单元 (默认不 enable)
└── common/        两条链路共用
    ├── k230-cast / k230-cast.service     互斥仲裁器
    ├── k230-screen.sh / .service          常驻上屏 (drmplay -F, 空闲刷黑)
    ├── k230-dhcp-simple.py                极简 DHCP 客户端 (P2P 组接口兜底拿租约)
    └── k230-audio.sh / .service          常驻音频 (pcmplay 独占 ALSA, 吃 /run/wfd-audio.pcm)
```

## 视频/音频公共链路 (两条线复用)

    H.264 ES -> /run/airplay.h264 或 wfd 管道 -> v4l2_dec(VPU 硬解) -> /run/wfd-player.fifo -> drmplay
    音频(PCM S16LE/48k/2ch) -> /run/wfd-audio.pcm -> pcmplay -> ALSA

- 视频: Miracast 走 wfd-sink 收流; AirPlay 走 uxplay 的 -vdmp 直通。都交给 v4l2_dec 硬解 + 常驻 drmplay。
- 音频: Miracast 由 wfd-sink 重组 LPCM 写入 /run/wfd-audio.pcm; AirPlay 由 uxplay 的 GStreamer
  pipeline (avdec_alac/avdec_aac) 解成 PCM 写同一管道。pcmplay 独占 ALSA 出声 (空闲补静音)。
  板上依赖: `apt install gstreamer1.0-plugins-base gstreamer1.0-libav` (AirPlay 解码必需)。

## 历史路径说明

2026-10-29 前的 `docs/PROGRESS.md` 等历史记录里的 `src/miracast/...` 是**当时的真实路径**,
保留不改 (记录不篡改)。当前权威路径以本目录为准: `src/K230-cast/{miracast,airplay,common}`。
