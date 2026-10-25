# K230 Miracast — rtl8189fs 驱动改造 (随 SDK 走)

`0100-k230-miracast.patch` 是 **4 处改动的合并补丁**, 由 buildroot package infra 在解包后
自动 `patch -p1` 应用 (和上游 buildroot 自带的补丁一起, 本文件排在最后)。

| # | 文件 | 改动 |
|---|---|---|
| 1 | `os_dep/linux/ioctl_cfg80211.c` | `cfg80211_rtw_start_ap()` / `change_beacon()`: 让 GO 的 beacon / probe-resp 带上 supplicant 传来的 **P2P IE + WFD IE**, 并在 `change_beacon` 里吃 `beacon_ies` / `proberesp_ies` 的**动态** WPS IE (Selected Registrar=1 / DevPwId=PBC)。少了这个, 手机"发现得到但连不上" |
| 2 | `core/rtw_mlme_ext.c` | `OnProbeReq()`: `DRIVER_CFG80211` 下即使不是 AP/GO 也把 probe req 上报 cfg80211 (老的 `!P2P_STATE_NONE` 门在 cfg80211 模式下恒为假, 导致 device 角色不能被搜到) |
| 3 | `core/rtw_mlme_ext.c` | `issue_probersp()`: P2P IE 追加条件放开到 `P2P_ROLE_DEVICE` |
| 4 | `os_dep/linux/ioctl_cfg80211.c` | `cfg80211_rtw_scan()`: `survey_times_for_one_ch` **6 → 5** (单信道扫描 ≈0.79 s, 让首帧 AUTH 不要抢在手机 GO 起组之前) |

## 逐条背景 / 证据

见主仓 `K230/src/miracast/driver/README.md` (补丁 1~4 各一节, 含抓包证据与回滚方式)。

## 应用与验证

```bash
cd K230/sdk/k230_linux_sdk
make rtl8189fs-dirclean CONF=k230_canmv_01studio_defconfig
make rtl8189fs CONF=k230_canmv_01studio_defconfig
# 应用成功的标志: build/rtl8189fs-<hash>/ 里出现 .stamp_patched
# 产物: output/k230_canmv_01studio_defconfig/build/rtl8189fs-<hash>/8189fs.ko
#       期望 md5 = 44917f9637745e7030c92fdfba2ccb03 (survey=5 版)
```

## 回滚

```bash
rm buildroot-overlay/package/rtl8189fs/0100-k230-miracast.patch
make rtl8189fs-dirclean rtl8189fs CONF=k230_canmv_01studio_defconfig
```

> **2026-10-25 起 ko 不入仓**: `8189fs.ko` 由本补丁在 buildroot 里编译产出, overlay 不再放预编译 ko。
> 另一台机器 clone 后 `make rtl8189fs` 即可得到同一份驱动; 产物 md5 见上 (若因编译器版本略有差异, 以 "行为一致" 为准: survey=5 且 GO 态 probe-resp 走 issue_probersp_p2p 之外的老路)。
