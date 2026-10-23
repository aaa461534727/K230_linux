# K230 Miracast — 驱动与 wpa_supplicant 改造 (随 SDK 走, 另一台机器 clone 即得)

> **为什么在这里**: 之前的驱动 / supplicant 改动只落在 `output/<conf>/build/` 里,
> 那是 buildroot 的解包目录 —— `make <pkg>-dirclean` 或 `make clean` 就全没了,
> 换机器 clone 下来也**复现不了**。现在全部收进 `buildroot-overlay/package/`,
> 由 buildroot 的 package infra 在每次解包后自动 `patch -p1`。

## 1. 两个补丁的位置与内容

| 包 | 补丁 | 改了哪些文件 | 一览 |
|---|---|---|---|
| `rtl8189fs` (94cc959d…) | `buildroot-overlay/package/rtl8189fs/0100-k230-miracast.patch` | `core/rtw_mlme_ext.c` · `os_dep/linux/ioctl_cfg80211.c` | 4 处: P2P/WFD IE 上 beacon/probe-resp · change_beacon 吃动态 WPS IE (PBC 位) · device 角色上报 probe req · 单信道扫描 6→5 |
| `wpa_supplicant` 2.11 | `buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch` | `src/p2p/p2p_go_neg.c` · `src/p2p/p2p_pd.c` · `src/p2p/p2p_group.c` · `src/p2p/p2p_i.h` · `src/eapol_supp/eapol_supp_sm.c` · `wpa_supplicant/p2p_supplicant.c` · `wpa_supplicant/wpa_supplicant.c` | 7 处: GO-Neg 自动接受 · **GO-Neg-Resp 不再省略 Operating Channel + Channel List 收成单信道** · PD 响应带 Group ID/Operating Channel · 关联前 cancel ROC · EAPOL-Start 立即发 · WPS 退避封顶 1.5 s · p2p_group_get_ssid |

两个目录里各有一份 `README.md`, 写着每处改动的理由、证据、重编与回滚命令。

> 补丁顺序: buildroot 自带的 `0001-Revert-*` / `0002-macsec_linux-*` 先打,
> 我们的 `0100` 排在最后 (同目录按文件名字典序)。

## 2. 另一台机器怎么复原 (从零到能编译)

```bash
# 1) 拉主仓 (框架 + 源码/文档/脚本) 和 K230 SDK 仓
git clone ssh://git@ssh.github.com:443/aaa461534727/project-framework.git hclinux
cd hclinux && ./setup.sh
git clone ssh://git@ssh.github.com:443/aaa461534727/K230_linux.git K230/sdk/k230_linux_sdk

# 2) 编译 (补丁自动生效, 无需手工 patch)
cd K230/sdk/k230_linux_sdk
make wpa_supplicant rtl8189fs CONF=k230_canmv_01studio_defconfig

# 3) 确认补丁真的打进去了
B=output/k230_canmv_01studio_defconfig/build/wpa_supplicant-2.11
ls $B/.stamp_patched                                    # 存在 = 补丁已应用
strings $B/wpa_supplicant/wpa_supplicant | grep K230    # 应有 4 条 (EAPOL 立即发 / 自动接受 GO-Neg / stop find / cancel ROC)
grep -n "survey_times_for_one_ch = 5" \
  output/k230_canmv_01studio_defconfig/build/rtl8189fs-*/os_dep/linux/ioctl_cfg80211.c
```

> `output/` `dl/` `buildroot-2025.02.1/` 都被 `.gitignore` 掉, 不进仓 ——
> 源码级改动全部由 overlay 补丁承载, 所以 **clean 之后照样复现**。

## 3. 上板 (换 supplicant / 驱动都按这个顺序)

```bash
D=output/k230_canmv_01studio_defconfig
BOARD=root@10.0.102.4
SSH="sshpass -p root ssh -o StrictHostKeyChecking=no $BOARD"
SCP="sshpass -p root scp -O -o StrictHostKeyChecking=no"

$SSH 'sh /root/wfd-p2p.sh stop'                   # 不停会 "Text file busy"
$SCP $D/build/wpa_supplicant-2.11/wpa_supplicant/wpa_supplicant $BOARD:/tmp/wp.new
$SCP $D/target/lib/modules/6.6.36/updates/8189fs.ko               $BOARD:/tmp/8189fs.ko
$SSH 'cp -a /usr/sbin/wpa_supplicant /root/wpa_supplicant.bak-$(date +%Y%m%d)'
$SSH 'cp -f /tmp/wp.new /usr/sbin/wpa_supplicant && chmod 755 /usr/sbin/wpa_supplicant && md5sum /usr/sbin/wpa_supplicant'
$SSH 'cp -f /tmp/8189fs.ko /lib/modules/6.6.36/updates/ && rmmod 8189fs && modprobe 8189fs'
$SSH 'sh /root/wfd-p2p.sh client'                 # 手机当 GO, 板子当 client
$SSH 'nohup setsid sh /root/wfd-device-watcher.sh >>/tmp/wfd-device-watcher.log 2>&1 &'
```

> ⚠ `pkill -f wfd-device-watcher` 这类命令必须**单独一条 ssh 执行**:
> 和 `cp` / `mv` 写在同一个 `ssh` 里, pkill 会把自己的 `bash -c` 一起带走,
> 后面的 `mv` 静默不执行 (实测 md5 还是旧的)。

## 4. 板上运行期脚本 (源在主仓, 不在 SDK 仓)

| 板上路径 | 主仓源 |
|---|---|
| `/root/wfd-p2p.sh` | `K230/src/miracast/sink/wfd-p2p.sh` |
| `/root/wfd-device-watcher.sh` | `K230/scripts/board/wfd-device-watcher.sh` |

**注意 `/root/wfd-p2p.sh` 的 `write_conf()` 每次 start 都会整份重写 `/tmp/wfd-p2p.conf`** ——
想改 conf 必须改脚本本身, 直接改 `/tmp/wfd-p2p.conf` 会被覆盖。

## 5. 当前未解卡点 (交接)

见主仓 `K230/docs/MIRACAST_AUTH_WINDOW.md` **§10**:

- 手机 GO 在"刚建组、零客户端"的 16 s 窗口内, 对我们的合法 AUTH **0 ACK**
  (17:49 轮: 179 帧 AUTH / 0 ACK, 手机却回了 168 beacon + 36 probe-resp);
- 唯一一次真机连上是 2026-10-23 16:42 (ch11/2462, WPS 全通, 持活 25 s);
- 根因 (已定位, 已改): 板上 `p2p_pref_chan=81:11` 让 `p2p->num_pref_freq` 恒 0 ⇒ 我们继续
  `Omit Operating Channel`, 手机不知道我们只支持 2.4G ⇒ 才协商到 ch8 / ch149 ⇒ 对我们
  179 帧合法 AUTH 一帧不 ACK。
- **补丁已落 (2026-10-23)**: `p2p_build_go_neg_resp()` 里 REMOTE_GO 且 `cfg_op_channel` 时
  照样带 Operating Channel, 并把 Channel List 收成单信道; 板上配置改成
  `p2p_oper_reg_class=81` + `p2p_oper_channel=$LISTEN_CHANNEL`, **不再写 `p2p_pref_chan`**。
  判据 = `P2P-GO-NEG-SUCCESS ... freq=2462` + 日志有 `K230P2P: advertise single channel 81/11`。
  细节见 `buildroot-overlay/package/wpa_supplicant/README.md` 末尾一节。
  改完照 §2 重编, 板端只需换 supplicant (§3)。
