# carplay/patches/ 说明

## ⚠ 0001-rtl8189fs-quiet-k230p2p-probe-log.patch 已失效, 勿再使用

该补丁把 `OnProbeReq()` 里的 `pr_info` 降成 `pr_debug`, **实际没有效果**:

rtl8189fs 的 `hal/phydm/phydm_debug.h` 在 `DM_ODM_SUPPORT_TYPE == ODM_CE` 分支里做了
`#undef pr_debug` + `#define pr_debug printk`, 把该编译单元内的 `pr_debug` 全部还原成
裸 printk (走 default_message_loglevel = 4), 照样上串口。2026-11-01 那次「换模块后 8 s 内
0 行」的结论不成立 —— 换的模块仍是同一份源码。

**现行方案** (2026-11-04): 直接删掉打印语句 + 控制台降级双保险

- SDK: `K230/sdk/k230_linux_sdk/buildroot-overlay/package/rtl8189fs/0101-k230-quiet-probe-log.patch`
  (删掉 `pr_debug("K230P2P: probe req -> cfg80211 ...")`; 已实测重编后 ko 里该字符串 0 次)
- SDK: `.../k230_extras/etc/sysctl.d/99-k230-quiet.conf` (`kernel.printk = 1 1 1 7`)
- SDK: `.../k230_extras/etc/systemd/system/ssh.service.d/niceness.conf` (`Nice=-10`, 单核抢不到 CPU)

成因与实测数据见 `K230/docs/PROGRESS.md` 2026-11-04「SSH 卡顿」条。
