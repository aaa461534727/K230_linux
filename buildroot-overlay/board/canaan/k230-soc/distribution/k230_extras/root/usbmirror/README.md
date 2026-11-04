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
./usb_mirror --touch                           # 画面 + 触摸 (Android 10+)
./usb_mirror --bit-rate 12000000               # 加码率 (默认 8 Mbps)
./usb_mirror --size 720x1280                   # 手工指定手机分辨率 (触摸映射用)
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
| 触摸需要 **Android 10+** (`input motionevent`) | 老机器 (如努比亚 7.1) 只有画面 | 启动时探测 SDK 自动降级为「不开触摸」并打日志 |
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
| `tools/mock_adbd_test.c` | 无手机的协议自测 |

