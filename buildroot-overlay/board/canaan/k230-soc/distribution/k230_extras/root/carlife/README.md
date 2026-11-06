# K230 CarLife（有线 / 无线 均已真机打通）

> 本目录 = `K230/src/K230-cast/carlife/` (CarLife 车机端源码与文档)。调研背景见 `K230/docs/CARLIFE_RESEARCH.md`。

## 现状

| 阶段 | 状态 |
|---|---|
| AOA 握手 + accessory 重枚举 | ✅ **真机已通**（2026-10-30, Nubia NX589J / Android 7.1.1 / CarLife 6.0.0） |
| 私有帧编解码（外层/内层/protobuf） | ✅ **真机已通**（手机回包被完整解出） |
| 会话状态机（HU_VERSION → VIDEO_START） | ✅ **真机已通**（VIDEO_INIT_DONE 832x480@30） |
| H.264 → VPU 硬解 → drmplay | ✅ **真机出画**（抓屏 95.8% 非黑, 15~18 fps） |
| PCM 音频 → pcmplay | 🟡 代码就绪, 该机暂未推音频流（未验证） |
| 单点触摸上行 | ✅ 已发到手机（`TOUCH_ACTION` 回包 ok）；**坐标按旋转做了逆变换, 待主人复验落点** |
| OEM 鉴权 | ❌ 未实现（无密钥）；手机若强制鉴权会拒 |
| 多点触摸 / 键盘 / 麦克风 | ⏳ 未做 |
| 无线（WiFi）· **两种形态** | ✅ **真机已通**（2026-10-30）：UDP 7999 收 `CarlifeHost` 广播 → 连手机 7240/8240/9240-9242/9340/9440。**A 手机连车机热点** 出画 19.4 fps · **B 车机连手机热点**（`carlife-ap.sh sta <SSID> [PSK]`）出画 17.6 fps；两种都是**打开 App ≤2 s 自动连**；手机换网 keepalive 8~11 s 自愈。协议细节见 `docs/WIRELESS.md` |


## 真机验证记录（2026-10-30, Nubia NX589J / Android 7.1.1 / CarLife 6.0.0）

```
[usb] AOA52 idx=0 'Baidu' ok (6 B utf-8) ... idx=5 '0720SerialNo.'
[usb] AOA53 已下发 → accessory 就绪: if=0 ep_in=0x81 ep_out=0x02 mps=512
[tx] ch=1 msg=0x18001 24 B ok          ← 拆包: 先 8 B 外层头, 再内层
[sess] 版本匹配 → 已发 HU_INFO + VIDEO_INIT (1280x720@30)
[sess] 收到 MD_INFO (303 B)            ← 0a07 Android / 12 07 msm8952 / 22 05 nubia
[sess] VIDEO_INIT_DONE: 832x480@30 → 发 VIDEO_START
[raw] rx ch2 帧, H.264 Annex-B (00 00 00 01 67 ...)
```

- 画面: `drmdump` 非黑 **95.8%**, bbox 460x800, drmplay `内容变化 60/60 / flip 60/60`，**15~18 fps**
- 手机侧: CarLife 显示「为了确保您的行驶安全，请在汽车屏幕上进行操作」= 正常车机模式
- 切换: 拔插后自动重连（主人连续拔插多次都成功）

## 三个必须知道的坑（都踩过）

1. **识别串必须是 UTF-8 + NUL，不是 AOA 规范写的 UTF-16LE**
   发 UTF-16LE 时该机 `f_accessory` 按 C 串处理，每个串只剩首字符
   （`dumpsys usb` 显示 `mManufacturer=B, mModel=C ...`），Android 就不会派发
   `USB_ACCESSORY_ATTACHED`，App 永远不开 accessory ⇒ 写 bulk 一直 TIMEOUT。

2. **发送必须拆包：先 8 B 外层头，再内层**
   手机 `AOAConnectManager` 用 `bulkTransferIn(buf, 8)` 读头；一次发整帧会
   `bulkTransferIn error 3: dataLen=20, len=8`（OVERFLOW）→ 250 ms 内断链。

3. **绝不在投屏进行中重启 `carlife_sink`**
   重启会让 App 那边的 touch/video 通道断掉（`CarlifeTouchManager: mReader close fail`），
   而 accessory 是粘住的、App 不会再去抓已连接的 accessory（`am force-stop` + 重开也无效）
   ⇒ 只能拔插 USB。改代码请等空闲或让用户拔插。

## 自动连接（已实现）

`k230-carlife.service` 已 `enable`（开机自起）+ `Restart=always`，平时就在等：

```
手机打开 CarLife（不开也行）→ 插 USB 线
  → 板端发现 AOA 设备 → 下发 AOA53 → 手机重枚举成 18d1:2d01
  → Android 按识别串自动拉起 CarLife → 本进程握手 → 出画
拔线 → 会话结束 → 回到等待（下次插上自动重来）
```

## 触摸坐标

drmplay 会把 832x480 横屏源**顺时针转 90°** 铺满 480x800（`/run/wfd-uibc-geom = 9 0 462 800 0 0 480 832`）。
触摸走：屏像素 → 绘制矩形 → 旋转后坐标 → **逆旋转**回源坐标系（`vx = fy, vy = 479 - fx`）。

## 编译（板上本机）

```sh
apt-get install -y libusb-1.0-0-dev      # 一次性
sh build.sh                              # 产出 ./carlife_sink
```

## 运行 (有线)


```sh
# 前台跑, 日志同时写 /var/log/carlife.log
./carlife_sink --width 1280 --height 720 --fps 30 --verbose
```
- 插上安卓手机, 打开 **CarLife App**（手机端要装），程序自动 AOA 切换 → 等重枚举 → 建链；
- 画面走 `v4l2_dec → /run/wfd-player.fifo → drmplay`（与 Miracast/AirPlay 共用上屏进程）；
- 触摸读 `/dev/input/event0` 并按 480x800 → 视频坐标线性映射上行。

## 运行 (无线)

两种形态任选其一, 都会让手机**打开 App 就自动连**:

```sh
# 形态 A: 车机当热点 (手机连 K230-CARLIFE / carlife1234)
sh carlife-ap.sh start                  # 起 hostapd + dnsmasq + NAT, 会自动停 Miracast (同一颗网卡)

# 形态 B: 车机连手机热点 (手机开热点即可)
sh carlife-ap.sh sta "手机的SSID" "密码"   # wpa_supplicant(需 /root/wpalib) + dhcpcd, 自动切服务

sh carlife-ap.sh status                 # 看网卡模式 / 地址 / 服务
sh carlife-ap.sh stop                   # 两种都能停, 恢复 USB CarLife
```

详见 `docs/WIRELESS.md`（含"端口未公开"等三个待确认项）。

## 开机自启

```sh
cp carlife_sink /root/ && cp k230-carlife.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now k230-carlife
```

## 判据（真机验证时看）

```sh
tail -f /var/log/carlife.log      # [usb] AOA53 已下发 → accessory 就绪 → 版本匹配 → VIDEO_INIT_DONE
grep -c VIDEO_DATA /var/log/carlife.log
drmdump /dev/dri/card0 /tmp/c.jpg  # 有画面时非黑 >0%
```

## 已知限制
1. **OEM 鉴权**：不伪造 ACK；遇到会明确打日志。
2. 触摸映射是**线性比例**（屏 480x800 → 视频 WxH），drmplay 若做信箱化会有边距误差，后续按 `/run/wfd-uibc-geom` 同款做法改成发布真实绘制矩形。
3. 无线两种形态均已真机打通（车机当热点 / 车机连热点），但**同一颗 2.4G 网卡** ⇒ 与 Miracast 互斥（`carlife-ap.sh` 起停时自动切换）。
4. 与 Miracast/AirPlay 共用上屏管道：CarLife 起来时若另一路在投，会抢 `/run/wfd-player.fifo`，需要接仲裁（二期）。
