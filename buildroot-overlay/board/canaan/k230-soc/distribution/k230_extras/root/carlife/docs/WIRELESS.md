# 无线 CarLife（WiFi）—— 协议已逆向确认，真机已通

> 2026-10-30：无线这条链路**已打通并真机出画**（K230 抓屏 94.8~95.8% 非黑，16~19 fps）。
> 本文记录的是**从手机端 APK 逆向出来的一手事实**，不是猜的，也不是抓包猜端口。

## 1. 逆向来源

手机：Nubia NX589J / Android 7.1.1 / CarLife **6.0.0**（`com.baidu.carlife`，base.apk 107 MB）。

用 jadx 反编译 `classes.dex`，读这三个类：

| 类 | 作用 |
|---|---|
| `com.baidu.carlife.core.connect.e`（ConnectManager） | 起 7 个 ServerSocket + UDP 发现广播 + 连接态计数 |
| `com.baidu.carlife.core.connect.g`（ConnectSocket） | 单通道 TCP 读写与分帧 |
| `com.baidu.carlife.core.connect.c`（CarlifeCmdMessage） | 8 字节消息头的定义 |

## 2. 协议（事实）

### 2.1 发现：手机广播，车机只听

```
每 2 s（首次延迟 4 s）：
  手机 → 本网段广播地址 : UDP/7999   payload = ASCII "CarlifeHost" (11 B)
```

* 代码位置：`e.j()` 的 TimerTask；`e.m()` 取 `wlan0/ap0/softap0` 的**广播地址**。
* 手机**只发不收**（全工程只有这一处 `CarlifeHost`），所以车机**不需要回包**。
* 车机拿到的是**报文的源 IP**＝手机 IP。

### 2.2 建链：车机是 client，手机是 server

手机侧 `ConnectManager.c()` 起了 7 个 `ServerSocket`（端口常量在 `core/f.java` 的 `jB..jH`）：

| 通道 | 端口 | 名字常量 | 说明 |
|---|---:|---|---|
| Cmd | **7240** | `jJ` | 会话/命令（HU_VERSION…VIDEO_START） |
| Video | **8240** | `jK` | 裸 H.264 Annex-B 码流 |
| Media | **9240** | `jL` | 裸 PCM |
| TTS | **9241** | `jM` | 播报 |
| VR | **9242** | `jN` | 语音 |
| Touch | **9340** | `jO` | 触摸上行 |
| Data | **9440** | `jP` | 数据通道（不计数） |

* **除 Data 外的 6 条都连上，手机才算「已连接」**（`e.a(g)`：`B++`，`B >= 6` 触发）。7 条都要连。
* 连上后手机侧日志：`One client connected in CmdAcceptThread`。

### 2.3 分帧：没有 AOA 外层头

USB/AOA 是「8 B 外层头(channel|len) + 内层」，**无线每通道一条 TCP，因此只有内层 8 字节头**：

```
u16 data_len (<= 32768) | u16 reserved | u32 msg
```

* `c.a(byte[8])` 就是这么解的；`c.d(i)` 校验 `0..32768`。
* Video / Media 通道**完全裸写**（`g.a(byte[], int)`），没有任何头 —— 车机按字节流灌给解码器即可。
* Cmd / Touch 用上面的 8 字节头；`msg` 高 16 位仍是通道号（`0x18001` 等）。

## 3. 车机实现（本目录）

| 文件 | 内容 |
|---|---|
| `carlife_wifi.c` | `cl_wifi_wait_hello()`（UDP 7999 听 `CarlifeHost`）+ `cl_wifi_connect()`（带超时的 TCP connect、`TCP_NODELAY`、`FD_CLOEXEC`、`SO_KEEPALIVE` 5/3/3 探活） |
| `carlife_tcp.c` | `cl_tcp_wrap_fd()` 把已连好的 fd 包成 `cl_io` |
| `carlife_proto.c` | `cl_inner_parser_feed()`：无线用的 8 字节内层头解析 |
| `carlife_sink.c` | `--wifi`：发现 → 连 7 条 → Cmd 跑会话；Video/Media 各一个读线程；`CH_TOUCH` 走 Touch 连接；发送时跳过外层 8 字节 |
| `carlife-ap.sh` | 两种形态：`start` = 热点（hostapd + dnsmasq + NAT）；`sta <SSID> [PSK]` = 连热点（wpa_supplicant + dhcpcd）。起停自动切换 `k230-carlife-wifi` / `k230-carlife`，并与 Miracast 互斥 |
| `k230-carlife-wifi.service` | `ExecStart=/root/carlife/carlife_sink --wifi --verbose` |

## 4. 两种形态（都是「手机打开 App 就自动连」）

| 形态 | 命令 | 谁当热点 | 车机地址 | 手机地址 |
|---|---|---|---|---|
| **A 车机当热点** | `sh carlife-ap.sh start` | K230（`K230-CARLIFE` / `carlife1234`） | `192.168.50.1` | DHCP `192.168.50.10-60` |
| **B 车机连热点** | `sh carlife-ap.sh sta <SSID> [PSK]` | 手机 / 路由器 | DHCP | 热点网关 |
| C 同一路由（无脚本） | — | 路由器 | 有线 DHCP | 路由器 DHCP |

```sh
# A: 车机当热点 (会停 Miracast — 只有一颗 2.4G 网卡), 并自动起无线服务
sh /root/carlife/carlife-ap.sh start
sh /root/carlife/carlife-ap.sh status

# B: 车机连手机热点 (wpa_supplicant + dhcpcd, 20 s 内必须拿到 IP 否则报错退出)
sh /root/carlife/carlife-ap.sh sta "nubia Z17 miniS" d5cdfd84e4c1

# 收工 (A/B 通用): 停热点/断开, 自动恢复 USB 服务; Miracast 需另起
sh /root/carlife/carlife-ap.sh stop
```

手机侧动作只有一件：**连上同一个网 + 打开 CarLife**。App 每 2 s 广播 `CarlifeHost`，车机听到就自己连，不需要点任何按钮。

**C 形态**：手机与 K230 接同一个路由（同网段）时广播照样能到。实测手机在办公 Wi-Fi（`10.0.102.0/24`）、K230 走网线，同样自动连上并出画。

## 5. 真机验证（2026-10-30 09:51~09:58 / 10:30~10:50）

```
[wifi] 收到 192.168.50.34 的发现广播 (11 B)
[wifi] Cmd 通道 7240 已连 ... Touch 9340 已连 ... Data 9440 已连
[tx] ch=1 msg=0x18001 20 B ok        <- HU_VERSION (只有 8 B 内层头)
[sess] 版本匹配 -> 已发 HU_INFO + VIDEO_INIT (1280x720@30)
[sess] VIDEO_INIT_DONE: 832x480@30 -> 发 VIDEO_START
```

| 场景 | 结果 |
|---|---|
| **A 车机热点**：手机 `192.168.50.34`（RSSI -35~-42 dBm） | 7 通道全连 → 出画 **19.4 fps**，`内容变化 60/60 · flip 60/60`；手机经 NAT 外网 `ping 223.5.5.5` 32 ms、`www.baidu.com` 解析并 50 ms 通 |
| **B 车机连手机热点**：K230 `192.168.43.166` → 手机 `192.168.43.1` | 打开 App 后 1.5 s 建链 → 出画 **17.6 fps**（手机做 AP 同信道，比 A 略低） |
| App 重开（`am force-stop` + `am start`） | 旧会话按 keepalive 结束 → 新广播到达后自动重连，全程无人干预 |
| 手机关 Wi-Fi | 10:35:00 断，10:35:08 `Cmd 通道读失败` → 会话结束回听；再开 Wi-Fi 10:35:44 自动重连（**8 s 检出 / 无人工**） |
| 触摸 | `[touch] action=1 屏(312,31) -> 视频(32,164)`，`[tx] ch=6 msg=0x68001 23 B ok`（沿用 09:59 真机点按） |

## 6. 踩过的坑（按撞墙顺序）

1. **AP/STA 切换残留**：上次 `sta` 留下的 `wpa_supplicant` 会把网卡拽回 `managed`，`hostapd` 仍打印 `AP-ENABLED`，但手机根本搜不到 SSID → `start` 里补 `pkill -x wpa_supplicant` + `dhcpcd -k $IFACE`，并在 hostapd 起来后断言 `iw dev wlan1 info` 是 `type AP`，不然直接报错。
2. **wpa_supplicant 跑不起来**：`version 'OPENSSL_3.2.0' not found` —— 板上这个 `wpa_supplicant 2.11` 是对 OpenSSL 3.2 编的，系统 `libcrypto` 只有 3.0.13 ⇒ 必须 `LD_LIBRARY_PATH=/root/wpalib`（Miracast 那轮装好的那套）。
3. **手机换网后连接永挂**：手机离开该网时对端不会发 FIN，连接一直 `ESTAB`，车机就永远等不到新广播（旧代码会卡死到重启） ⇒ `cl_wifi_connect()` 开 `SO_KEEPALIVE`（`KEEPIDLE=5 / KEEPINTVL=3 / KEEPCNT=3`），约 8~15 s 结束会话并回到听广播。
4. **TCP 粘包把 payload 覆盖**：解析器「取出一个包 → `memmove` 挪掉已消费字节」时，`payload` 指针还指着同一块 buf，后续字节一多就被覆盖 ⇒ 手机回 `08 01` 被读成 `f1=0`，日志 `手机拒绝版本`。修法：`cl_parser` 加独立 `out[64K]`，解析时先 `memcpy` 再 `memmove`。USB 路径因为一次一个 bulk 包，一直没暴露。
5. **CarLife 的自带虚拟屏**：App 运行时创建 `CarlifePresentation 1920x1080` 与 `CarlifeFakePresentation 832x480` 两个虚拟屏；此时用 adb 点系统设置会落在虚拟屏坐标系（x>1080），物理屏点不动 ⇒ **要先 `am force-stop com.baidu.carlife` 再操作手机 UI**。
7. **2 分钟后弹「车机版本未经CarLife授权」—— 已修 (2026-10-30)**: 手机 App 连上后起 120 s 一次性计时器（`util/an.java` `d=120000`），到点就弹窗并断链（文案 `dialog_illegal_version`，见 `util/av.java`）。**修法 = 车机回一条 `msg=0x18027` + `CarlifeStatisticsInfo` protobuf**（手机收到后第一句就 `util/an.a()` 停表），见 `carlife_sink.c` 的 `send_hu_statistics()`: `VERSION_MATCH` 后立刻发一次 + 会话中每 50 s 补发。实测 logcat 出现 `Timer Stop` 且 120 s 后无 `Timeout`。**⚠ 只停表不够**: 渠道号若填了保留值 `20000000`/`20000001` (=`core/f$a.ai/aj`)，`util/av.a(ctx, channel, e)` 会**直接弹**同一个窗（等于自报水货车机）—— 必须报真实整车厂号，本实现默认 `20101100` (HARMAN)，`--channel` / `CARLIFE_HU_CHANNEL` 可换。改后无线连续挂机 22 min 弹窗 0 次。协议细节与字段表见 `docs/PROTOCOL.md §6`。
6. **手机不会主动回 K230 热点**：手机里若有更强/已保存的 Wi-Fi（如办公网），Android 会优先连它，即使 `K230-CARLIFE` 已保存 ⇒ 车上没有别的 Wi-Fi 时无此问题；办公环境要么手动点一下该 SSID，要么改用形态 B。

## 7. 注意（硬约束）

1. **只有一颗 2.4G 网卡**：热点/STA 必须停 Miracast/Cast；AirPlay 走以太网不受影响。
2. **屏幕只有一块**：USB 会话与 WiFi 会话不能同时跑（各起一个 `v4l2_dec` 抢播放器）。`carlife-ap.sh` 已做互斥。
3. 手机端 App 必须在前台运行（它才会广播）；App 被系统杀掉就不再广播。
4. 形态 A 手机连上后有外网（dnsmasq + NAT），CarLife 云端请求不会 `UnknownHostException`。
5. 解码器是 `fork+exec` 起的，socket 设了 `FD_CLOEXEC`；否则子进程攥住连接，手机不会重新广播。
6. 形态 B 下车机不转发流量（不做 NAT），手机自己的热点照常有网。
