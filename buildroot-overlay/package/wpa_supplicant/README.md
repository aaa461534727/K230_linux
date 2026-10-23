# K230 Miracast — wpa_supplicant 2.11 改造 (随 SDK 走)

`0100-k230-miracast.patch` 是 **6 处改动的合并补丁**, 由 buildroot package infra 在解包后
自动 `patch -p1` 应用 (排在上游 buildroot 自带的 `0001-Revert-*` / `0002-macsec_linux-*` 之后)。

| # | 文件 | 改动 | 主仓文档 |
|---|---|---|---|
| 1 | `src/p2p/p2p_go_neg.c` | K230P2P: 自动接受手机发来的 GO Negotiation Request (PBC); Intended P2P Interface Address 不用全零 | patches/README §(go-neg-accept) |
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
md5sum $B/wpa_supplicant/wpa_supplicant   # 期望 5f6f1497e51f10d1a1655f3d75d5f4fc
strings $B/wpa_supplicant/wpa_supplicant | grep -c K230   # 25
```

## 上板

```bash
sh /root/wfd-p2p.sh stop                     # 必须先停, 否则 Text file busy
scp -O $B/wpa_supplicant/wpa_supplicant root@<板>:/tmp/wp.new
ssh root@<板> 'cp -a /usr/sbin/wpa_supplicant /root/wpa_supplicant.bak-$(date +%Y%m%d)   && cp -f /tmp/wp.new /usr/sbin/wpa_supplicant && chmod 755 /usr/sbin/wpa_supplicant   && md5sum /usr/sbin/wpa_supplicant'
sh /root/wfd-p2p.sh client
```

## 回滚

```bash
rm buildroot-overlay/package/wpa_supplicant/0100-k230-miracast.patch
make wpa_supplicant-dirclean wpa_supplicant CONF=k230_canmv_01studio_defconfig
```
