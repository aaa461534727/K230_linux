# 媒体面接口契约 (控制面 ↔ 媒体面)

控制面 = CarPlay 协议实现 (蓝牙/iAP2/AirPlay), 媒体面 = `carplay-media` (本目录)。
两者**只通过三个 FIFO 通信**, 不共享内存、不互相链接。这样协议面崩了不会带走 MVX 硬件。

| 管道 | 方向 | 内容 | 写入方 | 读取方 |
|---|---|---|---|---|
| `/run/carplay.h264` | 控制面 → 媒体面 | **明文 H.264 Annex-B** (解密后的 AU) | 协议层 | `v4l2_dec` (由 carplay-media 拉起) |
| `/run/carplay.touch` | 媒体面 → 控制面 | **12 字节 HID 报告** / 事件, 固定长度无分隔 | carplay-media | 协议层 → 打包成 `POST /command` |
| `/run/wfd-player.fifo` | 媒体面 → 屏幕 | NV12 + **16 字节帧头** | `v4l2_dec` | 常驻 `drmplay` (k230-screen) |
| `/run/wfd-audio.pcm` | 控制面 → 声音 | S16LE 48k 2ch 裸 PCM | 协议层 (AAC 解码后) | 常驻 `pcmplay` (k230-audio) |

## 为什么 H.264 走 FIFO 而不走本地 socket

`v4l2_dec` 本来就把"文件/FIFO 路径"当输入 (`v4l2_dec <in> <out>`), 复用它的代价是 0 行代码。
代价是 FIFO 是**字节流、无边界**: 控制面必须保证按 AU 顺序写完整 NAL, 不能交错、不能半帧收尾。
(解码器侧已经做了 AU 拼包, 见 `tools/v4l2_dec.c` 的 "AU 边界拼包" 段。)

## 触摸 HID 报告 (12 字节)

```
byte 0 : finger0 id      byte 1 : finger0 down (1=按下)
byte 2-3 : finger0 x (u16 LE)   byte 4-5 : finger0 y (u16 LE)
byte 6..11 : finger1 同构 (空闲时全 0)
```

坐标已由媒体面按 `displays.widthPixels/heightPixels` (480x800) 归一; 控制面**不要**再缩放。
协议层收到后包成 bplist: `{type:"hidSendReport", uuid:<触摸设备uuid>, hidReport:<12B>}` → `POST /command`。

## 启动顺序

```
k230-screen (常驻 drmplay)   ← 必须在媒体面之前 (否则 FIFO 没有读者, 解码器 10 s 后放弃)
k230-audio  (常驻 pcmplay)
carplay-media                ← 起硬解 + 发布触摸
carplay-arbiter              ← 批准 wireless claim (K230 没有有线 CarPlay)
carplay-wireless / 协议面    ← 蓝牙 → iAP2 → AirPlay 会话
```
