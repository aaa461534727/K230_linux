# wpa_supplicant 补丁 (K230 Miracast)

## 1. `wpa_supplicant-2.11-k230-pd-resp-group-id.patch`

**解决的问题**: 安卓手机 (小米/红米 "无线投屏") 点选 `DIRECT-xxK230` 后一直转圈, 板端 supplicant 日志里能看到手机发来的
`Provision Discovery Request (PBC)` 且我们**已回 `Provision Discovery Response` 并被 ACK**, 但手机之后**一帧 Auth/Assoc 都没有**,
30 s 后重试 (整轮超时)。

**根因**: `src/p2p/p2p_pd.c::p2p_build_prov_disc_resp()` 的 legacy (非 P2PS) 分支在 `status == P2P_SC_SUCCESS` 时
**完全不发 P2P IE** (只发 WPS IE + WFD IE)。一个已经在跑组的 GO 如果不告诉对端
**P2P Group ID (BSSID + SSID)** 和 **Operating Channel**, 对端就无从得知"该加入哪个组 / 哪个信道",
join 流程起不来 —— 这正是手机转圈的原因。

**改了什么**:

| 文件 | 改动 |
|---|---|
| `src/p2p/p2p_group.c` | 新增 `p2p_group_get_ssid()` 访问器 (取运行中组的 SSID) |
| `src/p2p/p2p_i.h` | 声明该访问器 |
| `src/p2p/p2p_pd.c` | `p2p_build_prov_disc_resp()`: 新增 `else if (p2p->num_groups > 0)` 分支, 补 P2P IE = `Group ID` + `Operating Channel` |

只影响"我们自己是 GO 且已有组"这一种情况, 其他路径 (非 GO / 失败状态 / P2PS) 一行未动。

**参考实现对照**: 海奇板 (手机能连) 的形态是 `DIRECT-` 开放设备 + 独立 WPA2 AP `Hccast_9d08e7`;
手机靠 PD 响应里的 Group ID 才知道要去加入 `Hccast_9d08e7`。抓包证据见 `docs/MIRACAST_PORT.md` §8.12。

## 1.1 `wpa_supplicant-2.11-k230-cancel-roc-before-assoc.patch` (2026-10-23)

**解决的问题**: GO Negotiation 成功到第一帧 Authentication 之间**空转 5.31 s**。

**根因**: `wpas_start_wps_enrollee()` 里拿到 GO-NEG 结果后立刻往下走 WPS/关联, 但此时 supplicant
自己的 P2P **Listen 定时器仍在跑** (`wpas_p2p_listen` → `p2p_listen` → 一个 5000 ms 的
remain-on-channel, 在 `p2p_listen_channel` = ch6/2437)。射频被 ROC 占住, 排在后面的定向
scan(组信道 ch8/2447) 只能等 ROC 结束才轮得上。RT5572 空口实测: `P2P-GO-NEG-SUCCESS` @89.58 s
→ 首帧 AUTH @94.89 s。

**改了什么**: `wpa_supplicant/p2p_supplicant.c::wpas_start_wps_enrollee()`, 在
`wpas_copy_go_neg_results()` 之后加 `p2p_stop_find()` + `wpa_drv_cancel_remain_on_channel()`
并清 `off_channel_freq` / `roc_waiting_drv_freq` —— 与 join-running-group 路径的处理一致。
组信道此时已由 GO Negotiation 确定, 不需要再在 listen 信道上守着。

**效果**: 延迟 **5.31 s → 0.91 s** (板端日志 `1792740661.194` → `1792740662.104`, 并有新打印
`K230: Stop P2P find/listen before association`)。**但不解决 ACK 问题** (见
`docs/MIRACAST_AUTH_WINDOW.md`)。

**注意**: 编译时 `-t` (`wfd-p2p.sh` 已加) 才会出现秒级时间戳, 用于复算上述延迟。

## 1.2 `wpa_supplicant-2.11-k230-eapol-start-immediate.patch` (2026-10-23)

**解决的问题**: 关联成功 (`State: ASSOCIATING -> ASSOCIATED`) 之后**固定空等 2.00 s** 才发第一帧 `EAPOL-Start`,
而手机 GO 从 GO Negotiation 起只给约 **15 s**, 这 2 s 直接把 WPS 挤出窗口。

**根因**: `src/eapol_supp/eapol_supp_sm.c::SM_STATE(SUPP_PAE, CONNECTING)`. 上游为避免"AP 关联后本就会先发
EAP-Request/Identity, 我们抢发 EAPOL-Start 反而拖慢", 默认 `startWhen = 2` s; 只有 WPS 1.0 AP 才缩到 1 s.
REDMI K80 Ultra 当 GO 时**不会先发 EAP-Request**, 只等 enrollee 的 EAPOL-Start, 于是这 2 s 纯亏。

**改了什么**: 当 `sm->conf.wps & EAPOL_LOCAL_WPS_IN_USE` (我们确实是 WPS enrollee) 时, 直接把
`startWhen = startPeriod` + `send_start = 1`, 走已有的立即 `eapol_sm_txStart(sm)` 分支。
非 enrollee 场景 (普通 WPA2 连接 / WPS registrar) 一行未动。

**实测依据** (2026-10-23 16:51 ch8 失败样本): 手机 `GO_ENDING_SESSION` @480.405 s,
而我们的 EAPOL-Start 被排在 480.51 s —— **只差 0.10 s**。成功样本 (16:42) 里这段间隔同样是 2.00 s
(`1792744938.867` assoc -> `1792744940.869` `EAPOL: startWhen --> 0`), 只是那次余量够。

**效果**: 待真机验证 (预期把 `assoc -> EAPOL-Start` 从 2.00 s 压到 ~0.00 s, 等于白赚 2 s 预算)。

**回滚**: `patch -R -p1 <` 本补丁后重编; 板上二进制留底 `/root/wpa_supplicant.bak-pre-eapol-20261023`
(md5 `8f3fe3154c97aae3ca673aac59aa6244`, 含 ROC 补丁、不含本补丁)。

---
## 1.3 `wpa_supplicant-2.11-k230-wps-backoff-cap.patch` (2026-10-23)

**解决的问题**: 关联失败后的**指数退避**在 WPS 组网里是纯浪费 —— 手机 GO 从 GO Negotiation 起只给约 **16 s**,
而上游 `wpas_connection_failed()` 第 4 次给 5 s、第 5 次给 10 s, 一次就把剩余窗口吃光。

**根因**: `wpa_supplicant/wpa_supplicant.c::wpas_connection_failed()` 的 `switch (consecutive_conn_failures)`
1/2/3/4/≥5 → **100 / 500 / 1000 / 5000 / 10000 ms**。这套阶梯是为"AP 一直在线、慢慢重试"设计的;
P2P 组网里手机 GO 会在 16 s 后 `FORMATION_FAILED`/`GO_ENDING_SESSION`, 等不了。

**改了什么**: 在 switch 之后追加一行封顶 —— `if (wpa_s->key_mgmt == WPA_KEY_MGMT_WPS && timeout > 1500) timeout = 1500;`
只影响 WPS 场景, 普通 WPA2/WPA3 连接一行未动。

**实测依据 (2026-10-23 17:49, ch8)**: 封顶前第 4 次失败后要等 5 s (16:51 那次直接等到手机拆组);
封顶后日志出现 `Consecutive connection failures: 4 --> request scan in 1500 ms`, 5 次尝试在 14 s 内铺满窗口。
**但没有改变结果** —— 手机依旧 0 ACK (179 帧 AUTH / 0 ACK), 说明"退避浪费窗口"只是次要项,
真正卡点是手机在 16 s 窗口内根本不应答关联 (见 `docs/MIRACAST_AUTH_WINDOW.md` §9/§10)。

**回滚**: `patch -R -p1 <` 本补丁后重编; 板上留底 `/root/wpa_supplicant.bak-pre-backoff-20261023`
(md5 `97d53b11ba4b18c50021c0b41a7928f4`, 含 EAPOL 补丁、不含本补丁)。
带本补丁的二进制 md5 = `5f6f1497e51f10d1a1655f3d75d5f4fc`。

---

## 1.4 `wpa_supplicant-2.11-k230-go-neg-resp-operating-channel.patch` (2026-10-23) 【最高优先】

**解决的问题**: 手机当 GO 时, 我们的 GO Negotiation Response **不带 Operating Channel 属性**,
手机于是自己挑信道 (实测 ch8 / ch149 偏好), 然后对我们 **179 帧合法 AUTH 一帧都不 ACK**,
16 s 后 `FORMATION_FAILED`。

**根因链**:
1. `src/p2p/p2p_go_neg.c::p2p_check_pref_chan()` 开头 `if (!cfg->get_pref_freq_list || cfg->num_pref_chan || ...) return;`
   —— 8189fs 通过 cfg80211 拿不到驱动 PCL, 加上 conf 里还写着 `p2p_pref_chan`, 直接 return,
   ⇒ `p2p->num_pref_freq` 恒 **0**;
2. `p2p_build_go_neg_resp()` 里 `peer->go_state == REMOTE_GO && !p2p->num_pref_freq` 正是
   **`Omit Operating Channel attribute`** 的条件 ⇒ 属性被省略, 手机不知道我们只用 2.4G;
3. 同一函数里 Resp 的 Channel List 过滤 (`p2p_pref_channel_filter`) 也只在 `num_pref_freq` 非零时才走
   ⇒ 实测仍然广告 **13 个信道** (`58 58 04 51 0c 01..0d`)。

**改了什么** (两处, 都在 `p2p_build_go_neg_resp()`):
- Omit 分支加条件 `&& !(p2p->cfg->cfg_op_channel && p2p->op_reg_class && p2p->op_channel)`
  ⇒ conf 里硬写了操作信道时**照样发**属性;
- `peer->go_state == REMOTE_GO` 的 Channel List 在同样条件下换成**只含该信道**的
  `struct p2p_channels`, 日志打 `K230P2P: advertise single channel %u/%u`。

**配套配置** (`src/miracast/sink/wfd-p2p.sh`):
```
p2p_oper_reg_class=81
p2p_oper_channel=$LISTEN_CHANNEL
```
**绝对不要写 `p2p_pref_chan`** —— 它会让第 1 步直接 return, 见根因链。

**验证**: `make wpa_supplicant-dirclean && make wpa_supplicant` → 日志
`Applying 0100-k230-miracast.patch` 后列出 7 个文件; 产物 md5 **`6a0cd71eac228d1c1feb74b5d08092a4`**;
`strings | grep K230P2P` 有 `auto-accept ... with PBC` 与 `advertise single channel`。
**真机验收 (待做)**: `P2P-GO-NEG-SUCCESS role=client freq=2462` + 日志不再出现 `Omit Operating Channel attribute`。

**归口**: 正式补丁在 SDK 仓 `K230_linux:buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch`
(已合并进那一份大补丁, commit `d0ed278`); 本文件是**单文件文档副本**, 便于单独阅读/回滚。

---

## 2. 重新编译 (Buildroot SDK)

```bash
cd <repo>/K230/sdk/k230_linux_sdk
# 1) 让 buildroot 先把包解包/配置好 (会重新生成 .config, 覆盖 CONFIG_TLS)
make wpa_supplicant-rebuild CONF=k230_canmv_01studio_defconfig
# 2) 打补丁 (幂等: 已打过会失败, 可跳过)
cd output/k230_canmv_01studio_defconfig/build/wpa_supplicant-2.11
patch -p1 < <repo>/K230/src/miracast/patches/wpa_supplicant-2.11-k230-pd-resp-group-id.patch
```

**⚠ 交叉编译 pkg-config 坑 (2026-10-23 实测)**: 直接 `make CC=<host>/bin/riscv64-unknown-linux-gnu-gcc` 时,
pkg-config 会从**宿主** `/usr/lib/pkgconfig` 读 libnl-3.0, 于是 CFLAGS 里出现 `-I/usr/include/libnl3`,
编译器直接报 `ERROR: unsafe header/library path used in cross-compilation`。必须先让 pkg-config 只读
buildroot sysroot:

```bash
H=<SDK>/output/k230_canmv_01studio_defconfig/buildroot/host
SR=<SDK>/output/k230_canmv_01studio_defconfig/buildroot/host/riscv64-buildroot-linux-gnu/sysroot
export PKG_CONFIG_LIBDIR=$SR/usr/lib/pkgconfig:$SR/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=$SR
```

**⚠ 不要手工传整串 CFLAGS**: 会漏掉 `.config` 里定义的功能宏, 报
`'struct wpa_supplicant' has no member named 'ap_iface'` / `p2p_lo_started`。让 `make` 自己读
`.config` 生成 CFLAGS, 只覆盖 `CC`。

**⚠ 覆盖运行中的二进制**: `scp` 会报 `Text file busy`, 先 `kill $(cat /run/wfd-p2p.pid)`。

**⚠ openssl 版本坑 (2026-10-21 实测)**: buildroot 生成的 `.config` 是 `CONFIG_TLS=openssl`, 编出来的二进制动态依赖
**buildroot 的 libcrypto 3.4**, 而 Ubuntu 24.04 rootfs 自带 libcrypto 3.0.x ⇒ 启动直接
`version 'OPENSSL_3.2.0' not found`。两种走法:

- **A (现在板上用的)**: 把 buildroot 的 `output/<conf>/target/usr/lib/{libcrypto.so.3,libssl.so.3}` 放到
  `/root/wpalib/`, `wfd-p2p.sh` 检测到该目录就 `export LD_LIBRARY_PATH=/root/wpalib` (脚本已内置)
- **B (推荐做进镜像)**: 用内部 TLS 重编, 摆脱 openssl:
  ```bash
  cd output/<conf>/build/wpa_supplicant-2.11/wpa_supplicant
  sed -i 's/^CONFIG_TLS=openssl/CONFIG_TLS=internal/; s/^CONFIG_MESH=y/#CONFIG_MESH is not set/' .config
  make clean
  make -j4 CC=<host>/bin/riscv64-unknown-linux-gnu-gcc \
       CFLAGS="-O2 -I../src -I../src/utils -I../src/common -I../src/crypto -I../src/tls \
               -I../src/eap_common -I../src/eap_peer -I../src/eap_server -I../src/ap \
               -I../src/wps -I../src/p2p -I../src/drivers -I../src/rsn_supp \
               -I../src/l2_packet -I../src/radius -I../src/eapol_supp -I../src/eapol_auth \
               -I<host>/staging/usr/include -I<host>/staging/usr/include/libnl3"
  ```
  (内部 TLS 需同时关掉 `CONFIG_SAE_PK / CONFIG_OWE / CONFIG_SUITEB*`, 否则 `sae_pk.c` 重定义报错)

## 3. 验证

```bash
# 板上
sh /root/wfd-p2p.sh start          # 起 GO
grep "K230P2P: PD resp add Group ID" /tmp/wfd-p2p.log   # 手机点选后应出现
# 抓包侧: PD Response 里应出现 P2P IE (dd xx 50 6f 9a 09 ... 含 attr 0x0f Group ID)
```

## 4. 回滚

```bash
# 板上 (二进制)
cp /root/wpa_supplicant.orig-20261021 /usr/sbin/wpa_supplicant && sh /root/wfd-p2p.sh stop && sh /root/wfd-p2p.sh start
# 源码
patch -R -p1 < <repo>/K230/src/miracast/patches/wpa_supplicant-2.11-k230-pd-resp-group-id.patch
```
