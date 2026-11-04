# K230 无线 CarPlay 接收端 (carplay/)

本目录 = K230 上"把 iPhone 的无线 CarPlay 投到这块 480x800 竖屏上、并能触摸回控"的全部代码与资料。

> 方案与决策依据在 `K230/docs/CARPLAY_K230_NATIVE_PLAN.md` (落地版)、`CARPLAY_K230_TRACK_C.md` (B 路线+决策树)、
> `hichip/docs/CARPLAY_NATIVE_STACK.md` (协议事实基线: 端点 / 流类型 / 三套密钥 / HID 报文 / iAP2 编码规则)。
> 本 README 只讲**这一份代码怎么编、怎么跑、现在到哪一步**。

## 分层与"谁复用了谁的代码"

```
 ┌ 控制面 (Rust, 上游 carplayd 的 Unlicense crate) ─────────────┐
 │ L3 蓝牙  : BlueZ 原生 socket —— SDP record / SSP Just-Works / RFCOMM server │
 │ L4 iAP2  : 0xFF5A 帧 · 识别 1D00-1D06 · MFi 认证 AA00-AA06                 │
 │            无线交接 4300/4301/5702/5703 (crate 里已带编解码)               │
 │ L5 AirPlay: TCP 7000 / 配对 / SETUP / RECORD / 解密  ← **待补**             │
 └──────────────────────────────────────────────────────────────┘
              │ 明文 H.264 Annex-B     │ AAC-LC
              ▼ FIFO                   ▼ FIFO
 ┌ 媒体面 (C, 本目录 src/) ─────────────────────────────────────┐
 │ 硬解 : /usr/bin/v4l2_dec (仓内现成, mvx V4L2 M2M) —— 零新代码 │
 │ 上屏 : /run/wfd-player.fifo → 常驻 drmplay (k230-screen 服务)  │
 │ 出声 : /run/wfd-audio.pcm  → 常驻 pcmplay (k230-audio 服务)    │
 │ 回控 : /dev/input/event0 (FT5x06) → 12 字节 HID 报告           │
 └──────────────────────────────────────────────────────────────┘
```

**关键点**: K230 的屏和音频**各有一个常驻所有者**(drmplay / pcmplay), Miracast 和 AirPlay 早就走"往 FIFO 写"这条路。
CarPlay 直接接进同一套管道 ⇒ 不用碰 DRM、不用碰 ALSA 独占、不用重写解码器。

## 目录

| 路径 | 内容 |
|---|---|
| `src/hal/` | 四个 HAL: 上屏 / 出声 / 硬解看护 / 触摸转 HID |
| `src/media_plane.c` | **媒体面常驻进程**: `/run/carplay.h264` → 硬解 → 屏, 触摸 → `/run/carplay.touch` |
| `src/selftest.c` | 板端冒烟: `screen` / `touch` / `audio` / `decode` / `all` |
| `src/arbiter_stub.c` | 传输仲裁器桩 (K230 只有无线, 一律批准) |
| `systemd/` | 板端单元 (`carplay-media.service`) |
| `docs/PIPELINE.md` | **控制面 ↔ 媒体面的 FIFO 契约** (协议层只需照这个写) |
| `tools/k1probe/` | Rust 工具链探针 (plist / HKDF-SHA512 / ChaCha20-Poly1305 / Curve25519) |
| `scripts/fetch-deps.sh` | 拉上游 carplayd (锁定 commit) |
| `docs/STATUS.md` | 现场验收记录 (每条都有板端实测数据) |

## 构建 / 部署

```sh
make                 # → build/carplay-hal, build/carplay-arbiter (SDK 自带 Xuantie gcc)
make install         # scp 到板子 + 跑 2 s 彩条
make rust            # K1 探针 (需要 rustup target add riscv64gc-unknown-linux-gnu)
```

### airplayd (Rust 协议层, 当前主战场)

```sh
cd rust/airplayd
CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER=$SDK/output/*/host/bin/riscv64-unknown-linux-gnu-gcc \
  cargo build --release --target riscv64gc-unknown-linux-gnu

# 部署 (换二进制要先 stop, 否则 Text file busy)
scp -O target/riscv64gc-unknown-linux-gnu/release/carplay-airplayd root@<板>:/usr/local/bin/carplay-airplayd.new
ssh root@<板> 'chmod +x /usr/local/bin/carplay-airplayd.new && systemctl stop carplay-airplayd && \
  mv /usr/local/bin/carplay-airplayd.new /usr/local/bin/carplay-airplayd && \
  systemd-run --unit=carplay-airplayd --collect --property=Restart=always \
    /bin/sh -c "CARPLAY_SRP_PASS=3939 exec /usr/local/bin/carplay-airplayd"'
```

板端状态 (配对身份与 pairing 都在板上, 换机器不用搬):

```sh
journalctl -u carplay-airplayd -n 50 --no-pager   # ← 看 M3 校验 / M5 解出 LTPK / M4 之后控制面加密
ls /etc/carplay/                                 # ed25519.seed / pairing_id / pairings/
```

> ⚠ 判据口径: 手机每连接会重跑 pair-verify (有时 pair-setup), **M4 之后这条 TCP 最后一条明文** —— 后续双向都是 ChaCha20-Poly1305 分帧, 见 `docs/STATUS.md` §8.9。

板端验证:

```sh
carplay-hal screen 10        # 屏上出彩条 + 移动方块
carplay-hal touch 20         # 手点屏幕, 打印 12 字节 HID 报告
carplay-hal audio 2          # 440 Hz 正弦 (2 s)
carplay-hal decode /root/attic-20261027/phone1080.h264   # 文件当码流, 验证解码→上屏
carplay-hal all 10           # 上屏 + 触摸同时跑
```

## 进度 (2026-10-31)

| 里程碑 | 状态 | 证据 |
|---|---|---|
| K0 内核 BT | ✅ 模块已编好并加载 (`bluetooth/rfcomm/hidp/btusb`) | `lsmod` 有 8 个引用; vermagic `6.6.36 SMP mod_unload riscv` |
| K0 硬件 | ⛔ **缺 USB 蓝牙 dongle** (`lsusb` 只有 RTL8153) | `hciconfig` → `No such device` |
| K1 工具链 | ✅ Rust riscv64gc + 6 个 crate 板上跑通 | `K1 OK: plist 75B / hkdf+chacha ct 29B` |
| HAL 上屏 | ✅ 彩条 97% 非黑 | `drmdump` 采样 |
| HAL 硬解 | ✅ 1080p 文件 → 屏 100% 非黑 | `v4l2_dec` 子进程 |
| HAL 音频 | ✅ 384000 B PCM 进管道 | 有声否待主人确认 |
| HAL 触摸 | 🟡 设备已接上 (479x799), 待人工点按 | — |
| 媒体面 | ✅ `carplay-media` 常驻跑通: 码流 FIFO → 硬解 → 屏 **100% 非黑** | `journalctl -u carplay-media` |
| L3/L4 | ✅ 上游 `carplay-wireless` riscv64 跑起来, 卡在无 hci0 | 日志见 docs/STATUS.md |
| L5 AirPlay | ⬜ 未开始 | — |

## 已知坑 (都是本仓踩过的, 别再踩)

- 写 `/run/wfd-player.fifo` **必须带 16 字节帧头** `[NV12][w][h][len][ts_ms]`, 否则 drmplay 一直在 resync 找 magic,
  现象是"写了几百帧、屏全黑"(2026-10-31 实测)。
- 屏/音频管道**写端 open 会阻塞**, 没有读者时是 ENXIO, 不能用它判断"播放器活没活"; 10 s 超时更可靠。
- 板上常驻两个 `v4l2_dec` 时**不许再起第三个**做实验 (MVX 会卡死到重启, 见 WORKLOG 2026-10-31 12:37)。
