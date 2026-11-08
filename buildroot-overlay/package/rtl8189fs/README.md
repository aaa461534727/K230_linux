# K230 Miracast — rtl8189fs 驱动改造 (随 SDK 走)

`0100-k230-miracast.patch` 是 **4 处改动的合并补丁**, 由 buildroot package infra 在解包后
自动 `patch -p1` 应用 (和上游 buildroot 自带的补丁一起, 本文件排在最后)。

`0101-k230-quiet-probe-log.patch` 删掉 `OnProbeReq()` 里那行 probe 上报日志。
**为什么之前 pr_debug 静音无效**: 本驱动的 `hal/phydm/phydm_debug.h` 在 `ODM_CE` 分支里
`#undef pr_debug` + `#define pr_debug printk`, 把这个编译单元里的 `pr_debug` 全部还原成裸
printk(走 default_message_loglevel=4) ⇒ 照样上串口。本板 `console=ttyS0,115200` 且单核,
这条日志实测 6.9 行/秒, 每行在串口上阻塞 ~5 ms 并持 console_lock ⇒ SSH 登录/交互被拖慢。
所以这里**直接删掉打印语句**, 而不是降级。

| # | 文件 | 改动 |
|---|---|---|
| 1 | `os_dep/linux/ioctl_cfg80211.c` | `cfg80211_rtw_start_ap()` / `change_beacon()`: 让 GO 的 beacon / probe-resp 带上 supplicant 传来的 **P2P IE + WFD IE**, 并在 `change_beacon` 里吃 `beacon_ies` / `proberesp_ies` 的**动态** WPS IE (Selected Registrar=1 / DevPwId=PBC)。少了这个, 手机"发现得到但连不上" |
| 2 | `core/rtw_mlme_ext.c` | `OnProbeReq()`: `DRIVER_CFG80211` 下即使不是 AP/GO 也把 probe req 上报 cfg80211 (老的 `!P2P_STATE_NONE` 门在 cfg80211 模式下恒为假, 导致 device 角色不能被搜到) |
| 3 | `core/rtw_mlme_ext.c` | `issue_probersp()`: P2P IE 追加条件放开到 `P2P_ROLE_DEVICE` |
| 4 | `os_dep/linux/ioctl_cfg80211.c` | `cfg80211_rtw_scan()`: `survey_times_for_one_ch` **6 → 5** (单信道扫描 ≈0.79 s, 让首帧 AUTH 不要抢在手机 GO 起组之前) |

`0102-k230-p2p-ps-off.patch` **关掉 P2P PS (Notice of Absence 省电调度)**, 对齐海奇的
`rtl8188fu` 做法。海奇那份驱动是他们自维护的仓
(`gitlab.hichiptech.com:62443/sw/hclinux_sdk/rtl8188fu.git`, 2023.02 → 2026.05 连续维护),
里面就 3 处手改, 与本文**逐字同款**:

| # | 文件 | 海奇 / 本补丁 | 说明 |
|---|---|---|---|
| 1 | `include/autoconf.h` | `//#define CONFIG_P2P_PS` | 关掉整个 P2P PS 功能 |
| 2 | `include/rtw_mlme.h` | `#if 1//def CONFIG_P2P_PS` | 结构体字段 (ctwindow/noa_*) 必须留着, 否则编不过 |
| 3 | `include/wifi.h` | `#if 1//def CONFIG_P2P_PS` | `P2P_PS_STATE` / `P2P_PS_MODE` 枚举同理 |

**为什么关**: `CONFIG_P2P_PS` 一开, 驱动会把对端 P2P IE 里的 NoA 解析出来
(`core/rtw_p2p.c:4440 process_p2p_ps_ie()`), 写进 `REG_NOA_DESC_*` 并
`rtw_write8(REG_DUAL_TSF_RST, BIT(4))` 复位 p2p circuit TSF, 最后
`FillH2CCmd8188F(H2C_8188F_P2P_PS_OFFLOAD)` 把**射频/信道调度交给固件**
(`hal/rtl8188f/rtl8188f_cmd.c:444`)。本板只有一颗 radio、两接口分时,
固件按 NoA 主动"缺席"时 AP 侧 beacon 没有射频可用 —— 表现为 AP 停发、手机扫不到。

**为什么不能只改第 1 处**: `rtl8188f_set_p2p_ps_offload_cmd()` 挂在 `#ifdef CONFIG_P2P`
下 (不是 `CONFIG_P2P_PS`), 函数体却引用 `pwdinfo->ctwindow` / `noa_num` /
`noa_duration[]` 等 P2P_PS 专属字段。所以 2、3 两处用 `#if 1` 把结构体/枚举强行留住,
才得到"关功能但仍能编"的组合。三处是一套, 缺一编译就炸。

逐行取证 + 与 K230 三处对照 + 改后符号核验见主仓
`K230/captures/20261108-hichip-p2p-ps-off-evidence.txt` 与 `K230/docs/WIFI_COEX_HICHIP.md` §2.8。

> ⚠ **本补丁只关 P2P 省电, 不动 P2P/WFD 主体**: 改后 `process_p2p_devdisc_*` /
> `process_p2p_group_negotation_*` 全部保留, 只有 `p2p_ps_wk_cmd` / `p2p_ps_wk_hdl` /
> `process_p2p_ps_ie` 三个符号消失。也**不改驱动版本** (仍是 5.7.9 / 94cc959d),
> 与 2026-11-08 被否决的 5.15.6 移植是两件事。

## 逐条背景 / 证据

见主仓 `K230/src/miracast/driver/README.md` (补丁 1~4 各一节, 含抓包证据与回滚方式)。

## 应用与验证

```bash
cd K230/sdk/k230_linux_sdk
make sync          # 把 buildroot-overlay/ 同步进 output/buildroot-*/
make rtl8189fs-dirclean CONF=k230_canmv_01studio_defconfig
make rtl8189fs CONF=k230_canmv_01studio_defconfig
# 应用成功的标志: build/rtl8189fs-<hash>/ 里出现 .stamp_patched
# 产物: output/k230_canmv_01studio_defconfig/build/rtl8189fs-<hash>/8189fs.ko
#       2026-11-08 实测 md5 = 0751957f95d6b651f8c0ef8594e888b9 (含 0100/0101/0102)
#       (更早只有 0100 的 survey=5 版是 44917f9637745e7030c92fdfba2ccb03)
# 关 P2P PS 的判据:
nm 8189fs.ko | grep p2p_ps     # 期望: 无输出
nm 8189fs.ko | grep process_p2p_devdisc   # 期望: 仍在
```

## 回滚

```bash
# 只回滚 P2P PS 这一条
rm buildroot-overlay/package/rtl8189fs/0102-k230-p2p-ps-off.patch
make sync && make rtl8189fs-dirclean rtl8189fs CONF=k230_canmv_01studio_defconfig

# 回滚 0100 (Miracast 四补丁, 会导致手机"发现得到但连不上")
rm buildroot-overlay/package/rtl8189fs/0100-k230-miracast.patch
make sync && make rtl8189fs-dirclean rtl8189fs CONF=k230_canmv_01studio_defconfig
```

> **2026-10-25 起 ko 不入仓**: `8189fs.ko` 由本补丁在 buildroot 里编译产出, overlay 不再放预编译 ko。
> 另一台机器 clone 后 `make rtl8189fs` 即可得到同一份驱动; 产物 md5 见上 (若因编译器版本略有差异, 以 "行为一致" 为准: survey=5 且 GO 态 probe-resp 走 issue_probersp_p2p 之外的老路)。
