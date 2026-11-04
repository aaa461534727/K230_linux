# K230 统一信源仲裁 (k230-source-arbiter)

2026-11-02 定: **所有投屏能力开机全部待命, 同一时刻只允许一个会话**。

## 管什么

| 功能 | 名字 | 投屏端 (systemd) | 会话探针 |
|---|---|---|---|
| CarPlay (无线) | `carplay` | `carplay-arbiter` + `carplay-airplayd` (7000) + `carplay-wireless` (BT) | `/run/airplay-active` 或 7000 有 ESTAB |
| AirPlay 镜像 (iOS/Mac) | `airplay` | `k230-airplay` (uxplay, 7100/7101) | 7100/7101 有 ESTAB |
| Miracast | `miracast` | `k230-miracast` (wlan0 P2P, 7236) | 7236 有 ESTAB 或 wlan0 已关联 |
| CarLife 无线 | `carlife_wifi` | `k230-carlife-wifi` (UDP 7999) | 与手机 7240/8240/924x/9340/9440 有 ESTAB |
| CarLife 有线 | `carlife_usb` | `k230-carlife` (USB AOA) | USB 出现 18d1:2d0x |

排队规则: **先连上先赢** (优先级 = 配置里 ALL 的顺序), 赢家连上后其它投屏端全部 `stop`,
会话结束 3 s 后全部 `start` 回待命。

## 为什么不打架

* **显示层**: 只有 `k230-screen` 拥有 drmplay, 所有投屏端只往 `/run/wfd-player.fifo` 写;
  仲裁器保证同一时刻只有一个端在写 ⇒ 不会抢 CRTC、不会花屏。仲裁器**从不** stop
  `k230-screen` / 杀 drmplay / pcmplay。
* **端口**: CarPlay 控制口固定 7000; AirPlay 镜像改到 7100 (= `AIRPLAY_PORT`), 两者可并存。
* **无线**: CarPlay 与 CarLife 无线**共用同一个热点** K230-CARPLAY (wlan1 / 192.168.50.1),
  热点是基础设施, 不算会话; Miracast 走 wlan0 的 P2P, 不占 wlan1。
* **标记文件**: CarPlay 用 `/run/airplay-active`; AirPlay 镜像用 `/run/airplay-cast-active`,
  互不误判。
* **运行期绝不碰网络**: 见下节。

## 运行期绝不碰网络 (2026-11-02 血的教训)

本板 WiFi 是 **8189fs (SDIO)**。在运行期对 wlan1 做 `ip link down/up` + 重起 hostapd,
会**把整机挂死** (两次现场: 日志停在 `[ap] 停 Miracast` 那一行, SSH 全断, 只能断电恢复)。
另外 `pkill -x wpa_supplicant` (全局) 会连带杀掉另一块 vif 上的连接, 同样失联。

所以现在的分工是硬边界:

| 谁 | 能不能动网卡 |
|---|---|
| `k230-shared-ap.service` (oneshot) | ✅ **只在开机时**起一次共享热点, `Restart=no` |
| `k230-source-arbiter` | ❌ 只读体检 (`ap_ok`), 不在就**打日志告警**, 绝不重启 |
| `carlife-ap.sh` / `wfd-p2p.sh` | 清理 supplicant 时**只按 `-i <本网卡>` 精确匹配**, 不再 `pkill -x` 全局 |

手动恢复热点 (接受挂机风险, 尽量在没人投屏时做): `systemctl restart k230-shared-ap`。

## 部署 (顺序不能反)

```sh
cp k230-source-arbiter.sh /root/ && chmod +x /root/k230-source-arbiter.sh
cp k230-shared-ap.service k230-source-arbiter.service /etc/systemd/system/
cp k230-arbiter.conf /etc/ && chmod 644 /etc/k230-arbiter.conf
systemctl disable --now k230-cast.service        # 旧的两路仲裁器, 已被本服务取代
systemctl daemon-reload
systemctl enable k230-shared-ap.service           # 开机先起热点 (oneshot)
systemctl enable k230-source-arbiter.service      # 再起仲裁
systemctl start k230-shared-ap.service            # 先手动验一次热点
systemctl start k230-source-arbiter.service
tail -f /var/log/k230-arbiter.log
```

## 加新功能

1. `ALL` / `BOOT` 里加名字 (位置 = 优先级);
2. 脚本里写 `probe_<名>` (有会话返回 0) / `start_<名>` / `stop_<名>` 三件套;
3. `sh -n` 过一遍, 重启服务。

状态: `cat /run/k230-arbiter.state` (当前赢家), `/var/log/k230-arbiter.log` 有全过程。
