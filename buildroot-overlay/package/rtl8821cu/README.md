# rtl8821cu — K230(8811CU) 专用补丁

buildroot 2025.02.1 自带 `package/rtl8821cu/`(上游 morrownr/8821cu-20210916, pinned
`96c65c58b544241178638e810b333dcc9aa26b91`)。本目录只放 **K230 需要的补丁**,
`make sync` 时连同官方 `.mk/.hash/Config.in` 一起拷进 `output/buildroot-2025.02.1/package/rtl8821cu/`。

## 0100-k230-p2p-wfd-ie.patch

- **问题**: `cfg80211_rtw_start_ap()` 只把 `settings->beacon.head/tail` 交给
  `rtw_add_beacon()`, 把 `beacon_ies` / `proberesp_ies`(wpa_supplicant 放的
  P2P IE + WFD IE)整块丢掉 ⇒ GO 的 beacon/probe-resp 里没有 WFD IE
  ⇒ 手机「无线投屏」列表看不到本机 (8189FS 同一坑, 见 `src/K230-cast/miracast/driver/`)。
- **实测证据**: 8811CU 起 GO 后, 用另一块射频当 P2P 探针
  `p2p_peer <GO MAC>` → 有 `device_name` / `oper_ssid`, **没有 `wfd_dev_info=`**;
  对照真手机 `nubia Z17 miniS` 有 `wfd_dev_info=0x00101c440032`。
- **修法**: 把额外 IE 交给驱动自己的 `rtw_cfg80211_set_mgnt_wpsp2pie()`
  (它分别取 WPS/P2P/WFD IE), 并把 P2P 角色直接置 GO + `p2p_enabled=TRUE`。
- **代价**: 无(只影响 AP/GO start 路径); 失败回退 = 删本补丁 + `make rtl8821cu-dirclean`。
