# 无线 CarLife（WiFi）联调说明

> 2026-10-30 更新：有线已真机闭环，本章进入无线联调。**端口与发现方式仍未确定**，本文件记录现状与抓包方案。

## 1. 现状

| 项 | 状态 |
|---|---|
| 传输层抽象 `carlife_io.h`（USB/TCP 共用上层帧层） | ✅ |
| TCP 服务端 `carlife_sink --tcp <port>`（默认 9868，可配） | ✅ 本机 nc 灌帧已验证接入→会话→重监听 |
| 热点脚本 `carlife-ap.sh`（hostapd + `/root/dhcpd`，192.168.50.1/24） | ✅ 板端已起：SSID `K230-CARLIFE` / 密码 `carlife1234` |
| 手机侧真实发现方式 / 端口 | ❓ **待抓包**（tcpdump 已就位） |
| 与 Miracast 互斥 | ⚠ 同一颗 2.4G 网卡，开 AP 前自动停 `k230-miracast` |

## 2. 板端一键起停

```sh
sh /root/carlife/carlife-ap.sh start      # 停 Miracast → 起 hostapd + dhcpd
sh /root/carlife/carlife-ap.sh status     # 看 hostapd/dhcpd/信道/已连客户端
systemd-run --unit=carlife-wifi --no-block \
    /root/carlife/carlife_sink --tcp 9868 --verbose
tcpdump -i wlan1 -s 0 -U -w /tmp/wifi-carlife.pcap &   # 抓手机在干什么

sh /root/carlife/carlife-ap.sh stop       # 收工，恢复 P2P
systemctl start k230-miracast k230-cast
```

## 3. 抓包要回答的三个问题

1. 手机连上 AP 后**有没有发起发现**：UDP 广播 / mDNS(`_carlife._tcp`) / SSDP / 直接 SYN 某个端口；
2. 如果是直连，**目标端口**是多少（决定 `--tcp` 参数）；
3. 报文是不是我们已实现的 `u32 channel | u32 inner_len` 外层帧（若带别的握手前缀，需要在前置状态机里补）。

判据（抓完直接看）：

```sh
tcpdump -nr /tmp/wifi-carlife.pcap 'udp or (tcp[tcpflags] & tcp-syn != 0)' | head -40
```

## 4. 硬件约束

- K230 只有一颗 **RTL8189FS 2.4G 单频**（`iw phy phy1` 支持 IBSS/managed/AP/P2P-client/P2P-GO）；
- 开 AP 必须停 Miracast P2P，**两者不能同时**；AirPlay 走以太网不受影响；
- 三路（Miracast / AirPlay / CarLife 无线）的仲裁尚未接入，目前是人工 stop/start。
