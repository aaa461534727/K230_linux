# K230 Miracast — wpa_supplicant 2.11 改造 (随 SDK 走)

`0100-k230-miracast.patch` 是 **7 处改动的合并补丁**, 由 buildroot package infra 在解包后
自动 `patch -p1` 应用 (排在上游 buildroot 自带的 `0001-Revert-*` / `0002-macsec_linux-*` 之后)。

| # | 文件 | 改动 | 主仓文档 |
|---|---|---|---|
| 1 | `src/p2p/p2p_go_neg.c` | K230P2P: 自动接受手机发来的 GO Negotiation Request (PBC); Intended P2P Interface Address 不用全零; **GO-Neg-Resp 在 `cfg_op_channel` 时不再省略 Operating Channel, 并把 Channel List 收成单信道** | patches/README §(go-neg-accept) / §(go-neg-resp-operating-channel) |
| 2 | `src/p2p/p2p_pd.c` | PD Response 补 P2P IE (**Group ID + Operating Channel**); PD 用对端 listen 频点回 | §1 |
| 3 | `src/p2p/p2p_group.c` + `p2p_i.h` | 新增 `p2p_group_get_ssid()` 访问器 | §1 |
| 4 | `wpa_supplicant/p2p_supplicant.c` | 关联前 `p2p_stop_find()` + cancel remain-on-channel (省 5.31 s 空转) | §1.1 |
| 5 | `src/eapol_supp/eapol_supp_sm.c` | WPS enrollee 立即发 EAPOL-Start (去掉固定 2.00 s) | §1.2 |
| 6 | `wpa_supplicant/wpa_supplicant.c` | WPS 场景关联退避封顶 1.5 s | §1.3 |

## 应用与验证

```bash
cd K230/sdk/k230_linux_sdk
make wpa_supplicant-dirclean CONF=k230_canmv_01studio_defconfig
make wpa_supplicant CONF=k230_canmv_01studio_defconfig
B=output/k230_canmv_01studio_defconfig/build/wpa_supplicant-2.11
ls $B/.stamp_patched                     # 应用成功
md5sum $B/wpa_supplicant/wpa_supplicant   # 期望 6a0cd71eac228d1c1feb74b5d08092a4 (2026-10-23 起)
strings $B/wpa_supplicant/wpa_supplicant | grep -c K230   # 5 (K230P2P 两条新串: auto-accept / advertise single channel)
```

## 上板

```bash
sh /root/wfd-p2p.sh stop                     # 必须先停, 否则 Text file busy
scp -O $B/wpa_supplicant/wpa_supplicant root@<板>:/tmp/wp.new
ssh root@<板> 'cp -a /usr/sbin/wpa_supplicant /root/wpa_supplicant.bak-$(date +%Y%m%d)   && cp -f /tmp/wp.new /usr/sbin/wpa_supplicant && chmod 755 /usr/sbin/wpa_supplicant   && md5sum /usr/sbin/wpa_supplicant'
sh /root/wfd-p2p.sh client
```

## 2026-10-23 追加: GO-Neg-Resp 无条件带 Operating Channel (最高优先修复)

**问题**: 手机当 GO 时, 上游在 `peer->go_state == REMOTE_GO && !p2p->num_pref_freq` 分支里
**省略 Operating Channel 属性**。8189fs 走 cfg80211 拿不到驱动 PCL (`get_pref_freq_list` 失败),
`num_pref_freq` 恒 0 ⇒ 属性一直省略 ⇒ 手机自己挑信道 (实测挑到 **ch8/2447**) ⇒ 对我们
**179 帧合法 AUTH 一帧不 ACK**, 16 s 后 `FORMATION_FAILED`。

**改法** (两处, 都在 `p2p_build_go_neg_resp()`):

1. Omit 分支加条件 `&& !(p2p->cfg->cfg_op_channel && p2p->op_reg_class && p2p->op_channel)`
   —— 只要配置里硬写了操作信道 (`p2p_oper_reg_class` + `p2p_oper_channel`), 就照样发属性;
2. `peer->go_state == REMOTE_GO` 的 Channel List 在同样条件下换成**只含该信道**的
   `struct p2p_channels` (原来是 13 个信道全给, 手机有挑错的余地)。

配套的板上配置在主仓 `K230/src/miracast/sink/wfd-p2p.sh`:

```
p2p_oper_reg_class=81
p2p_oper_channel=$LISTEN_CHANNEL
```

**绝对不要写 `p2p_pref_chan`**: `p2p_check_pref_chan()` 见到 `cfg->num_pref_chan` 非零就
直接 return, `num_pref_freq` 永远是 0 —— 补丁之前就是它让属性一直被省略。

**验收判据**: 手机日志/板端日志出现 `P2P-GO-NEG-SUCCESS role=client freq=2462`, 且
`/tmp/wfd-p2p.log` 里能看到 `K230P2P: advertise single channel 81/11`, 不再出现
`Omit Operating Channel attribute`。

## 回滚

```bash
rm buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch
make wpa_supplicant-dirclean wpa_supplicant CONF=k230_canmv_01studio_defconfig
```
