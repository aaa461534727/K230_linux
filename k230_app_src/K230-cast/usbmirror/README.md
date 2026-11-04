# usbmirror — 安卓 USB 有线投屏 (免 root)

> 目标: 手机用 **USB 线**插到 K230 的 USB-A, 屏上就出现手机画面 + 能摸 —— 功能上等价于 Miracast,
> 只是码流不走 WiFi/RTSP, 走 USB/adb。

## 为什么这样做

Miracast 链路里「上屏」那一半已经完全复用:

```
Miracast : 手机 ──WiFi/P2P──> wfd-sink ──H.264──> v4l2_dec ──NV12──> /run/wfd-player.fifo ──> drmplay
USB 镜像 : 手机 ──USB/adb──> usb_mirror ──H.264──> v4l2_dec ──NV12──> /run/wfd-player.fifo ──> drmplay
```

**解码/上屏/竖屏适配/屏所有权/空闲刷黑一行都不用改**, 新写的只有左边那一格 (约 800 行 C)。

安卓侧能免 root 拿到「整屏 H.264」的唯一入口是 **ADB**:
`screenrecord --output-format=h264 - ` 把裸 Annex-B 流吐到 stdout, 正好喂给 v4l2_dec。

不用系统自带的 `adb` 命令的原因:
1. 板子 rootfs 里的 android-tools 是 2013 年的 4.2.2, **没有 `exec-out`**;
2. 我们要裸字节流, 必须走 `exec:` 服务 —— `shell:` 会经过 PTY, `\n` 被改成 `\r\n`, 码流直接废掉;
3. 少一层进程和管道, 单核板上省一次 memcpy。

## 前置条件 (缺一个都不成)

| 条件 | 说明 |
|---|---|
| 手机开**USB 调试** | 设置 → 开发者选项 → USB 调试。**第一次插上要在手机弹窗点「允许」(勾选一律允许)** —— 之后这台手机就永久授权了 |
| USB-A 口 | 板子只有 1 个 USB-A。**接 hub** 才能同时插手机 + 调试网卡 (RTL8153); 不接 hub 就得拔网卡走串口 |
| hub 要带供电 | 板子 Type-C 输入标称 5V@1A, 且 VBUS 无软件限流。手机 + 网卡同时插, 用 **带供电的 hub** 或换 5V/3A 电源, 否则会随机掉线/重启 |
| `k230-screen` 在跑 | 它独占 `drmplay` + `/run/wfd-player.fifo`。没起就没地方画 |
| libusb | 板上要 `libusb-1.0`(CarLife 已经装过 `libusb-1.0-0-dev`), 编译要 gcc |

## 部署 (板上编译, 和 CarLife 一样的路子)

```sh
# 1. 送到板上
scp -r K230/src/K230-cast/usbmirror root@<板子IP>:/root/usbmirror

# 2. 板上编译
cd /root/usbmirror && sh build.sh

# 3. 装服务
cp k230-usbmirror.service /etc/systemd/system/
systemctl daemon-reload

# 4. 跑
systemctl start k230-usbmirror
tail -f /var/log/usbmirror.log        # 或直接 journalctl -u k230-usbmirror -f
```

**默认不 enable**: 没插手机时它只是空转等设备, 需要时手动 `systemctl start` 即可。

## 用法

```sh
./usb_mirror                                   # 只管画面
./usb_mirror --touch                           # 画面 + 反控
./usb_mirror --bit-rate 12000000               # 加码率 (默认 8 Mbps)
./usb_mirror --size 720x1280                   # 手工指定码流尺寸 (默认按物理分辨率缩到 ≤1280)
./usb_mirror --panel 480x800                   # 本板屏尺寸 (触摸映射用, 默认就是 480x800)
./usb_mirror | /usr/bin/v4l2_dec /dev/stdin /run/wfd-player.fifo -H -D
```

第一次连着手机启动会看到:

```
[adb] 设备 2717:ff48 sn=xxxx iface=0 ep_in=0x81 ep_out=0x02
[adb] 已发送公钥 (指纹 2ee276d2f9900694) —— **请在手机上点『允许』并勾选『一律允许』**
[adb] 设备: device::ro.product.name=...
[mirror] ✓ adb 已连上
[mirror] ✓ 视频通道已开 (第 1 次), 手机开始吐 H.264
[mirror] 速率 8.1 Mbit/s  (累计 9.7 MB)
```

## 已知限制 (物理/系统层面, 不是 bug)

| 限制 | 影响 | 缓解 |
|---|---|---|
| `screenrecord` 单次最长 **180 s** | 每 3 分钟画面断 0.2~1 s 再续 | 已做自动重开 (v4l2_dec 靠 SPS 自动重建, 不需要人等) |
| `screenrecord` **不带声音** | 只有画面, 无声 | 要声音得走 scrcpy 的 audio 通道 (需 Android 11+ 且要推服务端), 另开任务 |
| 反控走 `/dev/input` 注入 | 需要手机 shell 在 `input` 组 (绝大多数机器都在) | 不在就自动降级成 `input tap/swipe`: 点按、滑动能用, 但**没有实时拖动** |
| 一次只有一路占用屏 | 和 Miracast/CarPlay/AirPlay 抢同一块屏 | 手动 `systemctl stop` 另一路, 或等接进仲裁器 |
| 只能有线 | 2 米线内 | 无线请继续用 Miracast |

## 排障

| 现象 | 先查 |
|---|---|
| `未找到 adb 设备` | 手机开了 USB 调试吗? 线是数据线吗? `lsusb` 能看到手机吗? |
| `等待超时 / 未授权` | 手机解锁看弹窗; 没弹就拔插一次; 之前拒过的话在「开发者选项 → 撤销 USB 调试授权」重来 |
| 连上了但屏黑 | 看 `v4l2_dec` 那边日志有没有 SPS; `pgrep -af drmplay` 玩家在不在; `/run/wfd-player.fifo` 有没有人读 |
| 画面卡/断 | `[mirror] 速率` 掉到 1 Mbit/s 以下 = USB 供电/线材问题; 换带供电 hub |
| 触摸位置偏 | 用 `--size` 手工指定手机分辨率, 或确认 `wm size` 能被正常读到 |

## 自测 (不需要手机)

```sh
cd tools && make test
# 用 socketpair 假冒 adbd, 把 CNXN -> AUTH(签名) -> CNXN -> OPEN -> WRTE/OKAY -> CLSE
# -> 重开通道 全流程跑一遍, 期望 PASS
```

签名实现 (`rsa2048.c`) 也单独对过: 与 `cryptography`/`openssl` 的 PKCS#1 v1.5 SHA-1 结果逐字节一致,
公钥 blob 与真 adb 的 524 字节布局 (len=64 | n0inv | n | rr | e=65537) 完全对齐。

## 文件

| 文件 | 作用 |
|---|---|
| `usb_mirror.c` | 主程序: 取流 + 触摸 |
| `adb.c/.h` | 最小 adb host (libusb + ADB 协议) |
| `rsa2048.c/.h` | 自带的 RSA-2048 PKCS#1 v1.5 签名 (无外部依赖) |
| `adb_key.h` | 内置 adb 密钥 (由 `tools/gen_adb_key.py` 生成) |
| `build.sh` / `run.sh` / `k230-usbmirror.service` | 编译 / 管线循环 / systemd |
| `touch.c/.h` | 反控: 板端触摸 → 手机 evdev 注入 |
| `tools/mock_adbd_test.c` | 无手机的协议自测 |
| `tools/adb_cmd.c` | 手工跑一条 adb 服务, 排查通道通不通 |
| `tools/cnxn_try.c` | CNXN 握手变体试探 (排查手机不应答) |
| `tools/usb_probe.c` | USB 分层探针 (枚举/claim/端点/收发) |



## 反控怎么做的 (Android 7 也能摸)

一开始按 Android 10+ 的 `input motionevent` 写的, 实测这台手机是 **Android 7.1.2 / arm64**,
`input` 只有 `tap/swipe/keyevent`, 没有 `motionevent` ⇒ 那套完全不可用。

改走 **原始 evdev 注入**:

```
板上 /dev/input/event0 (ft5x06, MT-B)  --读-->  usb_mirror  --24B struct input_event-->  手机 /dev/input/eventN
```

- 手机 `shell` 用户在 `input` 组 (`uid=2000 groups=...,1004(input)`), 能直接写 `/dev/input/eventN`
- 通道必须是 `exec:tee <dev>`: **`exec:` 没有 PTY**, 二进制原样过去。
  `shell:` 有 PTY, 终端驱动会把 0x0a/0x0d/0x03 当换行/Ctrl-C 处理, 坐标必乱
- `struct input_event` 大小随 ABI 变: arm64/x86_64 = **24 B**, arm/x86 = **16 B**
  (差在 `struct timeval`), 由 `ro.product.cpu.abi` 推断
- MT-B 序列: `ABS_MT_SLOT=0` → `ABS_MT_TRACKING_ID=1` → `POSITION_X/Y` → `BTN_TOUCH=1` → `SYN_REPORT`;
  抬起时 `TRACKING_ID=-1` + `BTN_TOUCH=0` + `SYN_REPORT`
- 60 Hz 上限, 每次移动把一个批次打成**一个 WRTE**, 不逐事件发

## 授权弹窗为什么老弹 (不是「每次都要授权」)

签名本身是对的 —— 板上 `tools/sig_dump` 算出的 256 B 签名拿回主机验
`pow(sig,65537,n)` 与标准 PKCS#1 v1.5 SHA-1 的 EM **逐字节相同**, `n0inv`/`rr` 也复核过。

问题在手机侧: **这台 nubia ROM 不把我们的公钥写进 `/data/misc/adb/adb_keys`**
(连续三次连接每次都重新挑战 ⇒ 存的 key 里没有我们), 于是每开一个 adb 会话都退回
「发公钥 → 弹窗」。`/data/misc/adb/adb_keys` 有 SELinux 保护 (`u:r:shell:s0` 读不到),
板端无法直读比对。

因此设计上把弹窗压到最少:

- 一个进程生命周期内**只连一次**, `screenrecord` 180 s 上限重开在同一会话里完成 ⇒ 最多弹一次
- USB 读错误**不再退出进程**, 改成同进程内重连 (30 × 2 s) —— 进程死一次就必然是新 adb 会话,
  也就是必然再弹一次; 见 `usb_mirror.c` 的 `goto reconnect`
- 自测/取数一律走单会话 (`--touch-selftest`), 不反复重连
- 常驻用 `systemctl start k230-usbmirror`, 起来授权一次后可长连

**结论: 弹窗次数 = 新建 adb 会话的次数, 与线材无关。** 2026-11-03 实测: 单个进程连跑 10 min
(手机仍在吐 H.264, `screenrecord` 重开 3 次) ⇒ `USB disconnect` **0 次**, 公钥**只发 1 次**;
而同一块板在前一小时里我反复 `systemctl restart` / `adb_cmd` / `--hold` 的窗口内,
dmesg 出现 5 次 `USB disconnect → new high-speed USB device` (还伴随 `idProduct ffcf→ffcd→ffcf`)。
判据: 只有 `reset high-speed USB device` 那行才指向 host 端口复位/接触问题;
单纯的 `disconnect + new device number` 且 PID 变了 = **手机自己在重新配置 USB 复合**。

## 和其它投屏源共存 (`k230-source-arbiter` 第 6 路)

```
/run/usbmirror-active     # 会话标记: 只在真开出 screenrecord 通道时存在, SIGTERM 时清
```

仲裁器靠这个标记判断「有线正在投」: 在投时把 CarPlay / AirPlay / Miracast / CarLife 全停掉
(两条管线同时写 `/run/wfd-player.fifo` 会花屏), 有线这一路排最高优先级。

- `start_usbmirror`: 先查 USB 总线上有没有安卓手机, **没有就跳过** (免得空转 + 免得被起停)
- `stop_usbmirror`: **只在标记存在时才真停** —— 空转的 usb_mirror 不碰 fifo, 杀掉再起
  只会白送一次授权弹窗
- `fast_off` 杀进程前会查 `/proc/<pid>/cgroup` 属不属于本 unit: 各路的 `v4l2_dec /dev/stdin`
  命令行**一模一样**, 不按 cgroup 过滤会把别的源正在用的解码器一起杀掉

## 上车实测 (2026-11-03, 中兴 NX589J / Android 7.1.2 / arm64)

| 项 | 实测 |
|---|---|
| 握手 | auth → 公钥被接受 → CNXN `device::` (已授权) |
| 码流 | `--size 720x1280`, 一帧 NV12 = 1350 KB |
| 解码 | `v4l2_dec` 定型 720x1280 → NV12 |
| 上屏 | drmplay `显示区 450x800 @ 15,0`, `内容变化 60/60`, `上屏 flip 60/60` |
| 指纹 | `2ee276d2f9900694` (换密钥后要在手机里删旧的再允许一次) |
| 反控 | `dumpsys input` RecentQueue 出现 `MotionEvent(deviceId=9, action=2 … (540.0,1132→1440)) + action=1` ⇒ 事件真的进了 Android 输入系统 |

## 四个协议坑 (都是真机才暴露, 记下来省下一次踩)

1. **data_check 是字节求和, 不是 crc32** —— 用 crc32 时手机对 CNXN 一个字节都不回 (连 AUTH TOKEN 都没有)。
   AOSP 的 `ComputeChecksum()` 就是 `sum += data[i]`。
2. **24 B 头和载荷必须拆成两个 USB 传输** —— 合成一个 31 B 的 URB 时 adbd 同样不理你; 拆开立刻回 AUTH TOKEN。
   真正的 adb host 本来就是 `write(hdr); write(payload)` 两次。
3. **不能为了读序列号先 open 一个临时 handle** —— 开/关一次之后 adbd 就彻底不应答。只 open 一次, 序列号用同一个 handle 读。
4. **`A_WRTE` 的小端常量是 `0x45545257`(线上字节 W,R,T,E), 不是 `0x57525457`** —— 写错时通道开得起来但收到 0 字节,
   而且自测用的 mock 也照着写错, **假通过**。两边一起改才对。
## 延迟从哪来 (2026-11-03 实测拆解, 别猜)

主人的疑问: 「海奇 E3103 (700 MHz / 只支持 1080p60) 有线都比我流畅, K230 性能高得多, 怎么反而卡?」

先给结论: **不是 K230 弱, 是我们把活干在了 CPU 上**。原参数下每一帧 12.6 ms 的
CPU 都花在 drmplay 的「软件 NV12→ARGB + 重采样」, 而一帧的预算只有 16.7 ms (60 fps)。

### 逐段实测 (同一段 12 s / 30 fps 720x1280 合成码流, 板上跑)

| 参数 | 解码吞吐 | drmplay「转换」 | drmplay 每帧 CPU | 管道延迟 | 判定 |
|---|---|---|---|---|---|
| `--size 720x1280` → 绘制区 450x800 (软件缩放) | 52.2 fps | **12.7 ms** | **12.6 ms** | **48~69 ms** | ✗ CPU 打满 |
| `--size 448x800` → 绘制区 448x800 (1:1 直贴) | 59.7 fps | **3.1 ms** | **3.8 ms** | — | 好, 但帧体还在管道里排队 |
| 上面 + `WFD_SHM` 零拷贝 + VPU `-s` 缩到 480x800 | 59.8 fps | **3.1 ms** | **3.5 ms** | **5~8 ms** | ✅ 现在这套 |

三段的关键差别:

1. **缩放由谁做**: 720x1280 → 450x800 是 drmplay 用标量循环逐像素重采样, 12.7 ms/帧;
   让 VPU 在解码时就做 (`-s 480 800`, 走 `VIDIOC_S_MVX_DSL_FRAME` / MVE DEC_DOWNSCALE),
   解码器多花不到 1 ms, drmplay 变成纯 1:1 拷贝 → 3.1 ms。
2. **帧体走哪**: FIFO 定容 4 MB (`/proc/206/environ` 里 `WFD_PIPE_CAP=2097152`),
   720p 一帧 1.35 MB ⇒ 管道里能囤 2~3 帧 = 40~60 ms 纯排队延迟。
   开 `WFD_SHM` 后管道只走 16 B 帧头 + 4 B 槽号, dec 靠槽头读序号回压, 最多领先 1 帧。
3. **等不等 chunk**: `DEC_AU=2` 让解码器一拿到完整 AU 就投, 不攒满 64 KB 再切。

### 现在这套的余量

`整圈 16.6 ms` = 正好一个 60 Hz vblank (等vblank 11.7 ms + 转换 3.1 ms)。也就是说
**drmplay 已经到 60 fps 天花板, 再快也没有意义** (屏就 60 Hz)。`每帧CPU 3.5 ms` 意味着
CPU 侧还能跑到 285 fps。剩下的限制只在手机 `screenrecord` 给多少帧。

### 关于「用 VDEC → VO Scaler → 屏」这条建议

方向对一半, 但**这块板上没有 VO**:

| 模块 | 本板是否存在 | 说明 |
|---|---|---|
| VPU / VDEC | ✅ `/dev/video0` (name=`mvx`) | H.264/HEVC 硬解, 且**自带下采样** (`-s`) —— 就是本次用的缩放器 |
| AI2D | ✅ `/dev/video4` (`canaan-non-ai-2d`), 用户态 `/dev/k230-ai2d` | 通用 2D 缩放, 但**只吃 RGB888**; 解码器出的是 NV12, 得先转色 ⇒ 比 VPU 下采样贵 |
| VGLite | ✅ `/dev/vg_lite` | 2D GPU (填充/混合), 不做 YUV 缩放 |
| VO (Video Output, 带图层缩放) | ❌ **没有** `/dev/vo*`, 也没有 `canaan-vo` | 那是 CanMV RT-Smart 那套 `DisplayConfig`/`media` 框架的接口, 本板 Linux 侧没编进去 |
| DRM | ✅ `/dev/dri/card0` (ARGB8888 dumb buffer) | 本板的上屏方式, 只能整帧写 |

所以正确的映射是:

```
手机 screenrecord (H.264) → USB bulk → 板:VPU 硬解 + 硬件下采样 (一次搞定)
   → 共享内存两槽 (零拷贝) → drmplay: 1:1 NV12→ARGB → DRM → 屏
```

不需要 AI2D, 也不需要 VO。**缩放交给解码器**是最省的: 它本来就要把 YUV 写出来,
顺手按 `-s` 写小一号即可, 不额外多一趟内存带宽。

## 开关一览 (run.sh 里已默认打开)

| 环境变量 | 默认 | 作用 | 关掉会怎样 |
|---|---|---|---|
| `USBMIRROR_DSL` | 1 | 让 VPU 解码时缩到屏尺寸 | drmplay 退回软件重采样, 每帧 +9 ms |
| `WFD_SHM` | `/run/wfd-frame` | 帧体走共享内存两槽 | 帧体走 4 MB FIFO, 多 40~60 ms 排队 |
| `DEC_AU` | 2 | 一拿到完整 AU 就投 | 攒满 64 KB 才投, 多一个 NAL 的等待 |
| `USBMIRROR_VIDEO_W/H` | 448x800 | 请求的码流尺寸 (16 对齐) | — |

