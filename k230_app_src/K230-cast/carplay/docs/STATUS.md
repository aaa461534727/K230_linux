# K230 CarPlay — 现场验收记录

> 本文件只记"**在板子上实测过什么、结果是多少**"。方案与推理在 `K230/docs/CARPLAY_K230_NATIVE_PLAN.md`。
> 板: CanMV K230 (01Studio) · kernel 6.6.36 riscv64 · Ubuntu 24.04 · nproc=1 · 969 MB

## 0. 本轮环境

| 项 | 值 |
|---|---|
| 板 IP | **192.168.31.130** (USB 网卡 `enx00e04c88106c`; 主人给的 192.168.1.130 不通 —— 本机 192.168.23.128 只有 ens33, 跨网段不可达) |
| 登录 | `root/root` |
| 上屏链 | `k230-screen.service` → `drmplay -H -p nv12 -w 480 -h 800 -F /run/wfd-player.fifo` |
| 出声链 | `k230-audio.service` → `pcmplay` 读 `/run/wfd-audio.pcm` (S16LE 48k 2ch) |
| 触摸 | `generic ft5x06 (79)` = `/dev/input/event0`, 绝对范围 **479x799** |

## 1. K0-a 内核: 3 个 BT 选项 → ✅ 完成

改前 (`.config` 实读): `# CONFIG_BT_HCIBTUSB is not set` / `# CONFIG_BT_RFCOMM is not set` / `# CONFIG_BT_HIDP is not set`。

改法 (在 SDK 内核树内, 不动 defconfig 的其它项):

```sh
./scripts/config --module BT_HCIBTUSB --module BT_RFCOMM --module BT_HIDP
make ARCH=riscv CROSS_COMPILE=<sdk>/host/bin/riscv64-unknown-linux-gnu- olddefconfig
make ARCH=riscv CROSS_COMPILE=<sdk>/host/bin/riscv64-unknown-linux-gnu- -j8 modules
```

产物与板端加载结果:

```
btusb 53248 0 / btrtl / btintel / btbcm / hidp 24576 0 / rfcomm 36864 0
bluetooth 561152 8 ...   ← 8 个引用
modinfo vermagic = 6.6.36 SMP mod_unload riscv   ← 与板载内核一致, 无 version magic 报错
```

## 2. K0-b 硬件: USB 蓝牙 dongle → ⛔ 未插

`lsusb` 只有 `35d6:2510` (hub) 和 `0bda:8153` (RTL8153 网卡), **没有蓝牙**;
`ls /sys/class/bluetooth/` 空; `hciconfig hci0 up` → `Can't get device info: No such device`。

已装 BlueZ (`apt-get install bluez` → `/usr/bin/hciconfig`、`bluetoothctl`、`rfcomm` 就位)。
插上 CSR8510 A10 (`0a12:0001`) 后应当自动出 `hci0`, 无需额外固件。

## 3. K1 工具链 → ✅ 完成

`rustup target add riscv64gc-unknown-linux-gnu` + SDK gcc 作 linker, `tools/k1probe` 交叉编译 10 s,
板上输出:

```
K1 OK: plist 75B / hkdf+chacha ct 29B / ristretto-ish pk ce1a3299
```

覆盖: bplist 编解码、HKDF-SHA512、ChaCha20-Poly1305、Curve25519/Ed25519 —— CarPlay 三套密钥派生要用的全部原语。

## 4. HAL 上屏 → ✅ 97.0% 非黑

```
[carplay] 显示管道已接上: /run/wfd-player.fifo (480x800, 每帧 576000 B)
[carplay] screen 结果: 写入 600 帧, 累计 600 帧, 平均 30.0 fps
drmdump → crtc=52 fb=55 480x800 pitch=1920
非黑 97.0%
```

**踩坑记录**: 第一版直接写裸 NV12, 屏 **0.0% 非黑** —— drmplay 跑的是 `-H` (带帧头) 模式,
它在字节流里滑动找 `NV12` magic, 找不到就把我们的数据全丢了。加 16 字节帧头后立刻 97%。

## 5. HAL 硬解 → ✅ 100% 非黑

`carplay-hal decode /root/attic-20261027/phone1080.h264`:

```
[carplay] 硬解已起: pid=48651, /run/carplay-48649.h264 → /run/wfd-player.fifo (480x800)
[dec] 定型: 码流 1920x1080 H264 → 图像 1920x1080 NV12 (2 planes, stride=[1920,1920])
decode 非黑 100.0%
```

⇒ CarPlay 侧**不需要任何 V4L2 代码**, `v4l2_dec` 子进程 + FIFO 已经把"码流 → 上屏"包完了。

## 6. HAL 音频 → ✅ 写通 (有声否待主人确认)

```
[carplay] 音频管道已接上: /run/wfd-audio.pcm (S16LE 48000 Hz 2 ch)
[carplay] audio 结果: 写了 384000 字节 PCM      ← 2 s @48k/2ch/S16
```

## 7. HAL 触摸 → 🟡 设备接上, 待人工点按

```
[carplay] 触摸已接上: /dev/input/event0 原始 479x799 → CarPlay 480x800 (独占)
```

EVIOCGABS 自动读范围并线性映射, 不用写死分辨率。HID 报文 = 12 字节 / 2 触点, 与 hichip 文档 §2.4 同构。

**人工点按实测 (2026-11-01 14:01)**: 主人点屏 → **455 个 HID 报告**, 单指与双指都出, 坐标落在屏内
(`f0 (473,129)` / `f1 (473,450)` 双指同时按下), 方向正确 (右上角点出 x 大 y 小)。

实测暴露两个 bug, 已修:

| bug | 现象 | 修法 |
|---|---|---|
| 空闲触点未清零 | f1 抬起后仍发上一次的 `(425,128)`, iOS 会当成幽灵手指一直按着 | 未按下的触点整条写 0 |
| 触点 id 用了 `ABS_MT_TRACKING_ID` | 实测值 94/95/109…, 每次按下递增; 而 HID digitizer 里它只是"第几个触点", 超出触点数的值 iOS 侧行为未定义 | 改用 slot 序号 0/1 |

## 8. L3/L4 (上游 carplayd 的两个 crate) → ✅ riscv64 可跑, 卡在无 hci0

```
cargo build --release -p carplay-wireless --target riscv64gc-unknown-linux-gnu   → 9m57s, 0 error
```

板上:

```
[wireless] starting -- Phase A1 (Bluetooth discoverability + pairing only)
[arbiter-client] claimed wireless                  ← 我们的 arbiter 桩批准了
Can't get device info: No such device              ← 没有 dongle, hci0 不存在
[wireless] Bluetooth bring-up failed: hciconfig ["hci0","up"] failed: retrying
```

⇒ 只要插上 dongle, 这条链就能往下走到 SDP 广播 + 配对 + RFCOMM + iAP2 识别。

## 8.1 蓝牙真机链路 (2026-11-01 14:2x) — 手机已连上, 卡在 MFi

`0a12:0001` CSR8510 插上后立刻出 `hci0` (免固件), 走完全部蓝牙层:

```
[wireless] discoverable as "CarLink" -- waiting for pairing or preempt
[sdp] serving 'Wireless iAPv2' (handle 0x00010000, RFCOMM ch 1) on L2CAP PSM 0x0001
[sdp] ServiceSearchRequest -- returning handle 0x00010000        ← iPhone 查了 3 轮 SDP
[ssp-agent] Just-Works auto-accept agent running
[wireless] RFCOMM client connected -- starting iAP2 handshake
[bt-driver] SYN-ACK -- link up                                    ← 0xFF5A 链路建立
[bt-driver] RX 0xAA00: action failed, state held                  ← ⛔ 手机要 MFi 认证
[bt-driver] handshake budget expired before Identify
```

**踩坑**: 第一次起 `SDP server exited: Address already in use (98)` —— 系统 `bluetoothd` 占着 L2CAP PSM 1。
本栈自带 SDP/SSP/RFCOMM, **必须 `systemctl stop bluetooth && systemctl disable bluetooth`**。

**当前缺口**: `0xAA00` = 手机请配件出示 MFi 证书。上游 `iap2-core` 的 `mfi.rs` 是"调外部 MFi 芯片 oracle"
(TCP 5290, 返回 RSA-1024 的 128 B 签名), K230 没有芯片; 但仓里有**离线 MFi 身份**
(`rk3528-armbian/build/identity/offline-mfi/`, **EC P-256 / ECDSA**, p7b 单张 leaf + pk8)。
⇒ 要补的是 `MfiSigner` 的本地实现: `AA01` 回证书 (p7b 里的 DER), `AA03` 回 **64 B** ECDSA 签名
(不是上游假设的 128 B RSA)。PlayPort 就是拿这份 EC 身份在 RK3528 上真机跑通的。

## 8.2 内核日志噪声: rtl8189fs 的 K230P2P 打印 (2026-11-01 已修)

现象: 每个 probe request 都 `pr_info("K230P2P: probe req -> cfg80211 ...")`, 一秒几十行, 把真正要看的日志冲掉。

修法: 该驱动是**树外模块** (`buildroot-overlay/package/rtl8189fs`), 把两处每包打印改成 `pr_debug`:

```sh
cd output/.../build/rtl8189fs-<hash>
patch -p1 < K230/src/K230-cast/carplay/patches/0001-rtl8189fs-quiet-k230p2p-probe-log.patch
make -j8 ARCH=riscv CROSS_COMPILE=<sdk>/host/bin/riscv64-unknown-linux-gnu- \
     CONFIG_RTL8189FS=m CONFIG_RTW_DEBUG=n KVER=6.6.36 KSRC=<linux 树> \
     USER_EXTRA_CFLAGS="-UCONFIG_LITTLE_ENDIAN -DCONFIG_LITTLE_ENDIAN" modules
# 板上: rmmod 8189fs → 覆盖 /lib/modules/6.6.36/updates/8189fs.ko → depmod → modprobe
```

实测: 换模块后 8 s 内 `dmesg` **0 行** (改前几十行/秒); wlan0/wlan1 正常, 五个 k230-* 服务全 active。
`CONFIG_RTW_DEBUG=n` 不能漏 —— 漏了会换来另一批 `RTW: ...` 调试行。
回滚: 板上备份 `/root/8189fs.ko.bak-20261101` (md5 `44917f96`)。

## 8.3 本地 MFi 签名器 (2026-11-01 已实现, 待真机复核)

目标: **K230 上没有 MFi 芯片也先把链路彻底跑通**, 后续换真芯片时只换后端、不动协议。

实现 (`patches/0002-iap2-core-local-ec-mfi-signer.patch`, 只改 `iap2-core` 的 `mfi.rs` + 依赖):

| 项 | 值 | 依据 |
|---|---|---|
| 身份来源 | `/etc/carplay/mfi/{certificate.p7b,identity.pk8}` (可用 `CARPLAY_MFI_DIR` 覆盖) | 复用 `rk3528-armbian/build/identity/offline-mfi/` 的 EC P-256 身份 |
| 证书 (0xAA01) | **p7b 整个 PKCS#7 DER 原样** 返回 (607 B) | PlayPort `LocalMfiAuthenticationClient.readCertificate` 就是 `certificate.copyOf()` |
| 签名 (0xAA03) | `NONEwithECDSA` 语义: 输入是 **32 B 摘要**, 不再哈希; 输出 **64 B raw r‖s** | 同上 `signChallenge` (先 `require(size==32)`, 再 `derToRaw`) |
| 回落 | 目录不存在 → 原来的 **oracle TCP `192.168.50.2:5290`** | 换真芯片时**只需删掉 `/etc/carplay/mfi`** 或改指芯片 oracle, 协议层零改动 |

Rust 侧用 `p256` crate: `SigningKey::from_pkcs8_der` + `sign_prehash` (= NONEwithECDSA) + `Signature::to_bytes` (= raw r‖s)。

⚠ 上游假设的是 RSA-1024 的 **128 B** 签名, 这里出的是 **64 B** —— iOS 按证书类型自行判断, 报文结构不用改。

## 8.4 真机结果: MFi 认证 + 识别全通 (2026-11-01 14:50) — ✅

iPhone 连上 `CarLink` 后的完整日志 (板端 `journalctl -u carplay-wireless`):

```
[sdp] client connected on SDP PSM → ServiceSearchRequest (3 轮)
[wireless] RFCOMM client connected -- starting iAP2 handshake
[bt-driver] SYN-ACK -- link up
[mfi] 本地身份: 证书 607 B (/etc/carplay/mfi)      ← 本地 EC 身份生效
[bt-driver] RX 0xAA00 -> CertSent
[bt-driver] RX 0xAA02 -> SignSent                  ← 64 B ECDSA 签名被接受
[bt-driver] AuthSuccess / RX 0xAA05 -> Authenticated
[bt-driver] TX 0x1D01 IdentificationInformation (314 B, wireless transport)
[bt-driver] RX 0x1D00 -> IdentSent
[bt-driver] IdentifyAccept                          ← 识别被接受
[bt-driver] TX 0x5000 / 0x5200 (订阅)
[bt-driver] RX 0x1D02 -> Identified
[bt-driver] RX 0x4E0A (未识别, 14 B: 40 40 00 0d 4e 0a 00 07 00 00 7a 68 00 32)
[bt-driver] RX 0x5001 NowPlaying / 0x5201 RouteGuidance
```

**结论**: 离线 EC P-256 身份在 K230 上**完全可用** —— iOS 收下了我们的证书与 64 B ECDSA 签名,
并且接受了 314 B 的无线传输识别声明。MFi 这一关彻底通了。

**当前停在**: 手机没有发 `0x4300 CarPlayAvailability`/ `0x5702 RequestAccessoryWiFiConfiguration`,
所以无线交接还没开始。对照 PlayPort `Iap2WirelessControlClient` 的判定表:

| 收到 | 该回什么 | 现状 |
|---|---|---|
| `0x5702` | `0x5703` (SSID / passphrase / channel / securityType / wirelessIP) | 未收到 |
| `0x4300` | `0x4301` CarPlayStartSession (9 组) | 未收到 |
| `0x4E0E` 设备传输标识 | 重发一次 `0x5703` | 未收到 |

⇒ 下一步: ① 先把板载 AP 起起来 (SSID/pass/channel 与 5703 一致) ② 实现 `5702→5703` 与 `4300→4301` 的编解码与派发
③ 若手机仍不发 5702, 试"识别完成后主动发一次 5703"(PlayPort 不这么做, 但可作为兜底实验)。

## 8.5 无线交接第一步: 0x5702 → 0x5703 (2026-11-01 14:55 已上板)

PlayPort `Iap2WirelessControlClient` 的规则: **只在收到 0x5702 时**回 0x5703 (最多 5 次 pre-transport / 2 次 post-transport),
不主动发。已按此实现 (`patches/0003`):

| 参数 id | 字段 | 值 (环境变量可覆盖) |
|---|---|---|
| 1 | WiFiSSID (utf8, NUL 结尾) | `CARPLAY_AP_SSID` = **K230-CARPLAY** |
| 2 | Passphrase (utf8) | `CARPLAY_AP_PASS` = **k230carplay** |
| 3 | SecurityType (u8) | **2** = WPA2 Personal |
| 4 | Channel (u8) | `CARPLAY_AP_CHANNEL` = **6** |

⚠ 0x5703 **没有 param 0** (上游 `wifi_handoff.rs` 的注释已标注: 5703 是 1..=4, 5701 才是 0..=3)。

AP 侧直接复用仓内 `carlife/carlife-ap.sh` (hostapd + dnsmasq):

```sh
CARLIFE_AP_SSID=K230-CARPLAY CARLIFE_AP_PSK=k230carplay \
  CARLIFE_AP_IF=wlan1 sh /usr/local/bin/carlife-ap.sh start
# → wlan1 192.168.50.1/24, hostapd 起, SSID=K230-CARPLAY, type AP
```

**真机实测 (2026-11-01 14:59): iOS 根本不发 0x5702** —— 它发的是 **0x4E0E DeviceTransportIdentifierNotification**
(74 B, 里面是手机 BT MAC `30:30:...` 风格串 + USB transport id), 按 PlayPort 规则这时要**重发 0x5703** (上限 2 次)。
已按此补上 (`0x5702` 分支保留, 两条路都回同一份配置):

```
RX 0x4E0E DeviceTransportIdentifierNotification (body 68 B) -> 重发 0x5703 [1]
TX 0x5703 AccessoryWiFiConfigurationInformation (ssid=K230-CARPLAY ch=6, ...)
```

同时确认了另两个此前不认识的报文: `0x4E0A` DeviceLanguageUpdate (14 B) 与 `0x4E0B` DeviceTimeUpdate (30 B),
都是设备端上报, 不影响交接。

注意: 8201fs 只有一颗 2.4G 射频 ⇒ 开 AP 必须停 Miracast/P2P (脚本已自动做), AirPlay 走以太网不受影响。

## 8.6 0x4301 CarPlayStartSession (2026-11-01 15:12 已上板)

PlayPort `Iap2CarPlayMessages.startSession` 的 9 个参数, 逐条照抄:

| 参数 | 类型 | 本机取值 |
|---|---|---|
| group 1 (wireless) | 0=SSID(str) 1=passphrase(str) 2=channel(u8) 3=IP[](str) 4=securityType(u8) | K230-CARPLAY / k230carplay / 6 / 192.168.50.1 / 2 |
| field 2 | u32 | **airPlayPort = 7000** |
| field 3 | str | deviceIdentifier = 本机 BT MAC `04:7F:0E:0E:FB:E3` |
| field 4 | str | **publicKey = Curve25519 公钥的 64 字符 hex**(不是 32 B 二进制) |
| field 5 | str | sourceVersion = `366.0` |
| field 6 | str | sdkVersion = `366.0` |
| group 7 | 0=assetId(str) 1=assetVersion(u32) | `com.apple.carplay.asset` / 1 |
| field 8 | u8 | mutualAuth = 1 |

长钥持久化在 `/etc/carplay/airplay.key` (32 B), 首次自动生成 —— **必须落盘**, 换钥等于让已配对手机重新配对。

**触发**: 两条路都支持 ——
1. 收到 `0x4300 CarPlayAvailability` → 立刻回 0x4301 (PlayPort 原生行为)
2. **识别后 3 s 主动发** (`CARPLAY_AUTO_4301=1`, 默认开) —— 因为真机实测 iOS 在这条路上压根不发 0x4300

## 8.7 🎉 无线交接闭合: iOS 连上 7000 并发 /pair-setup (2026-11-01 15:35)

真机日志 (三处同时对上):

```
[bt-driver] RX 0x4300 CarPlayAvailability -> 回 0x4301        ← iOS 终于发 0x4300
[bt-driver] TX 0x4301 CarPlayStartSession (173 B) ssid=K230-CARPLAY ch=6 ip=192.168.50.1 port=7000 dev=04:7F:0E:0E:FB:E3
[sniff7000] connect from ('::ffff:192.168.50.42', 49188, 0, 0)
[sniff7000] 218 B: POST /pair-setup RTSP/1.0
            X-Apple-AbsoluteTime: 815211320   X-Apple-HKP: 0
            X-Apple-Client-Name: iPhone        Content-Length: 6
            Content-Type: application/x-apple-binary-plist
            CSeq: 0    User-Agent: AirPlay/620.8.2
            body = 00 01 00 06 01 01            (bplist: pair-setup method 1)
```

⇒ **蓝牙侧 iAP2 → 无线交接 → WiFi → AirPlay 端口, 整条链全部打通**。

### 之前为什么一直"静默" (根因, 值得记住)

不是 4301 报文的问题, 是 **WiFi 数据面断了**。`tcpdump -i wlan1` 只能看到手机**反复 ARP 问网关**
(`who-has 192.168.50.1 tell 192.168.50.42`), 我们的 ARP 回复它收不到 —— 关联挂着 (信号 -39 dBm)、
但三次握手密钥已经和 hostapd 对不上 (中间反复重启过 AP)。
**修法**: 手机上"忽略此网络"再重连 (= 强制重做四次握手), 立刻恢复: DHCP 新租约 → 4300 → 7000 连接。

⚠ 判据: **只看 station dump 会被骗** ("已关联"但数据面死)。要么看 dnsmasq 有没有新的 DHCPACK,
要么看 `tcpdump -i any` 里 wlan1 上有没有非 ARP 的流量。

## 8.8 AirPlay 服务端骨架 (2026-11-01 15:41 上板)

新工程 `rust/airplayd/` (独立 crate, 不是 third_party 的补丁):

| 文件 | 内容 |
|---|---|
| `src/rtsp.rs` | RTSP 解析/应答 (含 Content-Length 二进制体) |
| `src/tlv8.rs` | TLV8 编解码 (HAP 配对报文的基本单元) |
| `src/info.rs` | `GET /info` 的 bplist: **displays 480x800 + hidDevices(触摸) + audioFormats** |
| `src/main.rs` | 双栈监听 7000 + 分发 + 全量日志 (每个请求的 method/path/body 十六进制) |

**当前定位 = 抓包骨架, 不是成品**:
- `OPTIONS` → 200
- `GET /info` → 真的 bplist (R1 竖屏问题的第一次正面回答)
- `POST /pair-setup` → 回 M2 (salt 16 B + 384 B 公钥)。这个公钥**不是真 SRP 值**, 校验必然失败;
  目的是**确认 iOS 走的是不是 SRP 配对** —— 只要它回 M3 (proof, TLV 0x04), 就说明方向对,
  下一轮把 SRP-6a 的 proof 校验补上即可。
- 未实现: SRP 校验、`/pair-verify`、`/auth-setup`(MFi-SAP)、SETUP/RECORD、流解密

## 8.9 AirPlay 配对全通: pair-setup + pair-verify + 控制面加密 (2026-11-01 17:12~17:28, 真机)

复核上一个 AI 的「卡在 SRP 口令」结论, **判定有误**, 三个真凶全部找到并修复 (全部真机验证):

| # | 上一个 AI 的判断 | 实际情况 |
|---|---|---|
| 1 | 口令 3939/空串均 FAIL ⇒ 口令未知 | 口令 **3939 就是对的** (rk3528 + PlayPort 真机验证, 源码在 `rk3528-armbian/sdk/rk3528_armbian/artifacts/ref/playport-upstream/`) |
| 2 | 算法已按 uxplay Apple 变体逐行对齐 | **变体抄错**: CarPlay iPhone 客户端走 RFC 5054 全补零 + `K=H(S)` 64B (PlayPort `Srp6a.kt` 口径), uxplay 变体 (不补零 + 交错 128B K) 不吃 |
| 3 | (未察觉) | **致命 bug**: M3 处理 `new` 了新 Server, salt/b 与 M2 不是一套, M1 永不可能匹配 |

修复后真机一轮全通:
- **pair-setup M1~M6 完整走通**: SRP M3 校验通过 → M5 解出 iPhone LTPK (id=`00000000-...-00b362a549c5`) 验签 OK → M6 回我们 Ed25519 → pairing 落盘 `/etc/carplay/pairings/`
- **pair-verify M1~M4 走通**: X25519 临时钥 + PV-Msg02/03 ChaCha 加解密 + iOS 签名验签 OK
- **发现控制面铁律** (PlayPort `AirPlaySession.runControl`): **pair-verify M4 是该连接最后一条明文, 之后双向 RTSP 全部 ChaCha20-Poly1305 分帧** — 帧 = `[u16LE 密文长][密文][16B tag]`, 长度头做 AAD, nonce = 4×0+u64LE 方向计数器, 读写密钥 = HKDF(shared, "Control-Salt", "Control-Read/Write-Encryption-Key"), 块 ≤0x4000。已实现 `ControlCipher` 上板 (17:28:50)
- 另外两个坑: 配对端点应答必须带 `Content-Type: application/pairing+tlv8`; OPTIONS `Public` 必须含 `POST, GET` (rk3528 血泪同因)

代码: `rust/airplayd/src/{srp.rs,main.rs}` (verify 重写 + 每连接 Session + M5/M6 + pair-verify + ControlCipher)。
身份与配对落盘: `/etc/carplay/{ed25519.seed,pairing_id,pairings/}` (板端, 不入仓)。

## 8.10 当前卡点: iOS 收 0x4301 后不连 7000 (2026-11-01 19:02~20:44 取证, 仍未解)

**已实测通过的（全部有日志）**：蓝牙 SDP/RFCOMM/iAP2 握手 · MFi 认证 0xAA05 · iAP2 识别 IdentifyAccept · 0x4300→0x4301 · **AirPlay 配对链全通**（pair-setup M1~M6 / pair-verify M1~M4 / M4 后控制面 ChaCha 分帧加解密 / POST /auth-setup 密文正确解出）· auth-setup 应答已实现（PlayPort 口径 MFi-SAP，板上 /etc/carplay/mfi/）。

**卡点现象**：手机在 AP 上（-46~-66 dBm、ping 通、还在用板子 NAT 上网），蓝牙侧每 3 s 收一次 0x4301，**但从不向 192.168.50.1:7000 发起 TCP 连接**。

**证据链（按排除法，全部可复核）**：

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 1 | SRP 口令未知 | 证伪 | 3939 是对的；真凶是 SRP 变体抄错（uxplay 变体 vs PlayPort RFC5054 全补零 + K=H(S) 64B）+ M3 时重开 Server 导致 salt/b 两套 |
| 2 | uxplay 抢 7000/7001 | 真因之一（已修） | 19:03 k230-cast 自启把 uxplay 拉回来；已 disable + 条件门 |
| 3 | carplay-airplayd 进程死了 | 真因之一（已修） | 18:48 起进程消失、7000 无人监听；已改常驻 unit |
| 4 | iOS 退避需忘记重配 | 证伪 | 19:02/19:21/19:39 三次忘记+重配对，每轮 MFi/iAP2 都过，仍不连 |
| 5 | 车机名 CarLink 撞 iOS 特殊路由 | 证伪 | 改名 K230 + 忘记 + 重配对，20:39:57 仍零连接 |
| 6 | 4301 在手机上线前就发完 | 证伪 | 上限 3 改 120 次/3 s，20:44 手机在 AP 上已收 13 次，仍零 SYN |
| 7 | 端口/AP 数据面/防火墙 | 证伪 | 180 s SYN 抓包：**0 个 TCP SYN** 发往 192.168.50.1；ping 通、手机 NAT 上网正常 |
| 8 | 手机手动加入 AP 不等于 iOS 自动加入 | 当前主假设 | 17:45 成功轮前置条件 = 手机走过 0x5703 然后 iOS 自动加入；本轮手机是用户手动连的。已加「识别后 1.5 s 主动推 0x5703」待验 |

**下一步判据**：手机「忽略此网络」K230-CARPLAY（只忽略网络，不用忘记车）→ 点 CarPlay → 若 7000 出现连接日志，假设 8 坐实；若仍零连接，用 btmon 抓 iOS 在 4301 之后有没有发 0x4302/0x4E0D 等我们没回的报文。

## 9. 下一步 (按依赖顺序)

0. **加密通道验证 + SETUP** (新头号): 手机重试时确认 ControlCipher 口径对 (M4 后能收到加密 OPTIONS), 然后照 PlayPort `handleSetup` 答 streams bplist, 走 auth-setup (MFi-SAP) → RECORD → 收视频流。
1. ~~插 dongle~~ ✅ 已完成 (CSR8510 hci0)。
2. **R1 门槛**: 用 480x800 报 `/info displays`, 看 iPhone 接不接受竖屏 (这条决定项目能不能成)。
3. **L5 AirPlay RTSP 服务端**: 7000 端口 + `/pair-*` `/auth-setup` `/info` `SETUP` `RECORD` + 控制/事件/流三套密钥。
   明文 H.264 写 `/run/carplay.h264` (由 `carplay-hal decode` 之类消费), AAC 写 `/run/carplay.aac`。
4. 触摸回控接 `/command` (HID 报文已就绪)。
