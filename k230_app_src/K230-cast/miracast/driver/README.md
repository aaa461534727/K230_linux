# K230 — WiFi 驱动补丁 (Miracast 发现层)

> 为什么需要: **sink 能不能出现在手机「无线投屏」列表里，取决于 WiFi 驱动的 P2P/WFD IE**，
> 跟我们的 RTSP sink (`wfd_sink.c`) 无关。原厂 8189fs 驱动会把 wpa_supplicant 传来的
> 这两块 IE 丢掉，手机扫不到（2026-10-20 抓包实证）。

## 1. K230 01Studio 实际在用的驱动

```sh
# 板上确认（不要看 lsmod 里有没有 8733bs 就以为用它）
readlink -f /sys/class/net/wlan0/device/driver   # → /sys/bus/sdio/drivers/rtl8189fs
readlink -f /sys/class/net/wlan1/device/driver   # → 同一个 rtl8189fs
```

> `wlan0` / `wlan1` 都是 **8189fs** 驱动的两个 vif（phy0 / phy1）。
> `8733bs` 模块虽然也加载了，但**不拥有任何接口** —— 改它对发现层没有任何影响。

## 2. 补丁内容

`rtl8189fs-k230-p2p-wfd-ie.patch`（目标: `os_dep/linux/ioctl_cfg80211.c`）

| 点 | 原厂行为 | 补后 |
|---|---|---|
| `cfg80211_rtw_start_ap()` | 只把 `settings->beacon.head/tail` 交给 `rtw_add_beacon()` | 额外把 `beacon_ies` / `proberesp_ies` 交给 `rtw_cfg80211_set_mgnt_wpsp2pie()` 解析 |
| GO 角色 | `rtw_add_beacon()` 里「先看到 P2P IE 才 `rtw_p2p_enable(GO)`」，而 P2P IE 在 `beacon_ies` 里 ⇒ 永不触发 | 直接 `rtw_p2p_set_role(GO)` + `p2p_enabled=TRUE`，绕过 `CONFIG_CONCURRENT_MODE` 下 `iface_id==sel_p2p_iface` 的限制（GO 跑在 wlan1 会失败） |
| 结果 | beacon/probe-resp **222 B**，最后一个 IE 是 WPS，**没有 P2P IE / WFD IE** | probe-resp **360 B**，含 `dd 2e 50 6f 9a 09`(P2P) + `dd 0d 50 6f 9a 0a`(WFD, primary sink + session available + RTSP 0x1c44) |

## 3. 应用 + 重编

```sh
SDK=K230/sdk/k230_linux_sdk
CONF=k230_canmv_01studio_defconfig
cd $SDK/output/$CONF/build/rtl8189fs-*/          # hash 目录
patch -p0 < /path/to/rtl8189fs-k230-p2p-wfd-ie.patch
cd $SDK && make rtl8189fs-rebuild CONF=$CONF      # 产出 target/lib/modules/6.6.36/updates/8189fs.ko
```

上板（**必须真的重载模块**，`insmod` 对已加载模块是空操作）：

```sh
sh /root/wfd-p2p.sh stop
ip link set wlan1 down; ip link set wlan0 down
cp -f 8189fs.ko /lib/modules/6.6.36/updates/8189fs.ko
rmmod 8189fs && modprobe 8189fs
dmesg | grep K230P2P        # 应看到 start_ap enter head=.. bies=90 pries=.. / extra ies type=1 ... p2p_ielen=20
```

回滚: 板上备份在 `/root/8189fs.ko.orig-20261020`（原厂 ko），`cp` 回去 + `rmmod/insmod` 即可。

## 4. 验证（不依赖手机也能验）

```sh
# PC 侧 USB 网卡进 monitor, 盯 ch6 (社交信道)
sudo iw dev wlxf428530c5891 set type monitor && sudo ip link set wlxf428530c5891 up
sudo iw dev wlxf428530c5891 set channel 6
sudo tcpdump -i wlxf428530c5891 -e -s 600 "wlan addr2 46:87:63:df:bd:72 and wlan type mgt subtype probe-resp" -XX
```

判据: probe-resp 里出现 `dd 2e 50 6f 9a 09`（P2P）与 `dd 0d 50 6f 9a 0a`（WFD）。
实测小米/红米 `REDMI K80 Ultra` 已能在列表里发现 `DIRECT-xxK230`，并发 `P2P-PROV-DISC-PBC-REQ`。

## 第二个补丁 (2026-10-21): P2P device 模式探针响应

**文件**: `rtl8189fs-k230-p2p-device-probe-resp.patch` (改 `core/rtw_mlme_ext.c::issue_probersp()`)

**背景**: 解析 `captures/p2p0.pcap` 发现安卓手机投屏时**手机自己是 P2P GO + RTSP 服务端**
(192.168.49.1:7236, `Server: stagefright/1.2 (Linux;Android 16)`), sink 反而当 P2P client
(192.168.49.128)。于是把 sink 改成 client 模式, 但手机列表里再也看不到 K230。

**根因 (源码实证, 两层)**:
1. `OnProbeReq()` 对 `P2P_ROLE_DEVICE` 走 `p2p_listen_state_process()`, 而它要求
   `mlmepriv.p2p_probe_resp_ie != NULL` —— 该指针只在 `rtw_cfg80211_set_probe_resp_wpsp2pie()`
   (即 `cfg80211_rtw_start_ap()`) 里赋值 ⇒ **不建组就永远不回探针响应**;
2. `issue_probersp()` 里的 P2P/WFD IE 追加条件写死 `P2P_ROLE_GO`, 且外层还要 `_FW_LINKED`。

**本补丁做了什么**: 只放开第 2 层的角色判断 (`P2P_ROLE_GO || P2P_ROLE_DEVICE`, DEVICE 时用
`build_probe_resp_p2p_ie()` 自组装, group capability 为「非 GO」)。

**实测结论**: 上板后 `dmesg` 无 `K230P2P: probe resp P2P IE (role=DEVICE)` ⇒ 被第 1 层提前
return, 补丁单靠这一处不足。**该驱动上「当 P2P client」与「能被手机发现」互斥**, 想被看到必须
是 AP/GO (`_FW_LINKED`)。要继续走 client 路线, 需要更深改 `p2p_listen_state_process()`
(自建探针响应帧), 风险较高, 暂不推进。

**回滚**: `cp /root/8189fs.ko.bak-20261021 /lib/modules/6.6.36/updates/8189fs.ko && rmmod 8189fs && modprobe 8189fs`

## 补丁 3: `rtl8189fs-k230-pbc-on-air.patch` (2026-10-22) — **WPS PBC 位上不了空口**

**改哪里**: `os_dep/linux/ioctl_cfg80211.c` 的 `cfg80211_rtw_change_beacon()`

**现象**: 板端 `wps_pbc` 返 OK, supplicant 日志明确打出
`WPS: Selected registrar information changed` + `nl80211: Set beacon (beacon_set=1)`,
`proberesp_ies(len=218)` 里也确实有 `Selected Registrar=1` / `DevPwId=PBC`,
但**空口 beacon / probe-resp 的 WPS IE 始终停在 55 字节那版**(无 `0x1041` / `0x1012`)。
后果: 任何 WPS enrollee (MIUI 手机 / 海奇板 / 标准 wpa_supplicant) 都判
"AP without active PBC Registrar" ⇒ **发完 PD 就不再有动作 = 一直转圈**。

**根因 (两层)**:
1. 驱动原来只取 `head` / `tail`, 而 supplicant 把**动态变化**的 WPS IE 放在
   `beacon_ies` / `proberesp_ies` (额外 IE) 里 ⇒ 被丢弃。
2. 补丁必须打在 `change_beacon` 上 —— kernel ≥ 4.1 起 supplicant 的
   "Set beacon" 走 `NL80211_CMD_SET_BEACON` → `ops->change_beacon`,
   **`ops->set_beacon` 是死路径** (2026-10-22 先改错了一次, dmesg 无任何输出才发现)。
   驱动里两个函数并存, 只有 `change_beacon` 会被调用。

**改法**: 在 `change_beacon` 里补调 `rtw_k230_p2p_apply_extra_ies()` 吃额外 IE,
再用 `_update_beacon(adapter, 0xFF, ...)` 强制重发一次 beacon。

**验证 (2026-10-22, captures/20261022-0227-k230-go-pbc-on-air.pcap)**:
空口 beacon 的 WPS IE 从 **55 B → 78 B**, 新增
`10 41 00 01 01` (Selected Registrar=1) ·
`10 12 00 02 00 04` (DevPwId=PBC) ·
`10 53 00 02 63 88` · `10 49 00 0e 00 37 2a 00 01 20 01 06 ff ff ff ff ff ff` (AuthorizedMACs);
**同一条 beacon 里 P2P Capability 仍是 `0b03 ...` (group_capab 0x0b, GO 位在)**
⇒ 「手机能发现」与「WPS PBC 就绪」第一次同时成立。

**重编/部署**:
```bash
make -C output/k230_canmv_01studio_defconfig rtl8189fs-rebuild
scp output/.../target/lib/modules/6.6.36/updates/8189fs.ko root@<板>:/lib/modules/6.6.36/updates/
# 板上
pkill -x wpa_supplicant; rmmod 8189fs; insmod /lib/modules/6.6.36/updates/8189fs.ko
sh /root/wfd-p2p.sh stop && sh /root/wfd-p2p.sh start
```

**回滚**: `cp /root/8189fs.ko.bak-20261022-presetbeacon /lib/modules/6.6.36/updates/8189fs.ko && rmmod 8189fs && modprobe 8189fs`

> ⚠ 这个补丁与本目录另两个补丁**必须一起存在**: 少 `wfd-ie` 则 beacon 无 WFD/P2P IE,
> 少 `pbc-on-air` 则手机发现得到但连不上。

## 补丁 4: `rtl8189fs-k230-survey-times-5.patch` (2026-10-23) — 首帧 AUTH 的时机

**改哪里**: `os_dep/linux/ioctl_cfg80211.c::cfg80211_rtw_scan()` 里的 `survey_times_for_one_ch` (单信道扫描重复次数)

**背景**: 手机 GO 建完组后的头 1 s 内部还没就绪, 此时发去的 AUTH 会被静默丢弃。
上游把单信道扫描重复 **6** 遍 (≈0.90 s); 我们曾压到 **1** 遍 (0.188 s) 求快, 结果更糟 ——
首帧 AUTH 落在 GO-NEG **+0.21 s**, 手机全程不应答。

**改法与依据**:
- `1` → **`5`** (≈0.79 s), 让首帧 AUTH 落在 **GO-NEG +0.79 s**。
- 唯一成功的样本 (2026-10-23 16:42, ch11) 是 `+0.64 s` 关联成功, 而且那次第一次扫描**扫到 0 个 BSS**,
  等 0.25 s 重扫才找到 GO —— 属于"运气好地慢了一点"。所以这里不抢快, 主动留 ~0.75 s 给手机起组。

**效果**: 方向对 (AUTH 从 +0.21 s 推到 +0.79 s, 逼近成功样本), 但仍 **0 ACK**
(2026-10-23 17:49 轮: 179 帧 AUTH / 0 ACK)。说明**"发得早"不是主因**, 主因见
`docs/MIRACAST_AUTH_WINDOW.md` §10 (手机 GO 从头到尾不应答关联)。

**重编**:
```bash
cd K230/sdk/k230_linux_sdk/output/k230_canmv_01studio_defconfig/build/rtl8189fs-<hash>
make ARCH=riscv CROSS_COMPILE=<sdk>/host/bin/riscv64-unknown-linux-gnu- \
     KSRC=<sdk>/build/linux-<hash> KVER=6.6.36 \
     CONFIG_RTL8189FS=m CONFIG_RTW_DEBUG=n \
     USER_EXTRA_CFLAGS="-UCONFIG_LITTLE_ENDIAN -DCONFIG_LITTLE_ENDIAN" modules
```

**回滚**: 板上 `cp /root/8189fs.ko.bak-20261023b /lib/modules/6.6.36/updates/8189fs.ko && rmmod 8189fs && modprobe 8189fs`
(那份是 `survey_times=1` 的旧版, md5 `5339a786a928c243ca27708094718905`); 源码 `patch -R -p1 <` 本补丁。
带本补丁的 ko md5 = `44917f9637745e7030c92fdfba2ccb03`。
