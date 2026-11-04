# 第三方源码 (vendored)

- `uxplay-master.tar.gz` — UxPlay 1.74 上游源码 (FDH2/UxPlay, GPLv3), 2026-10-26 拉取, md5 `a8f001407f41542098f88b96261cf291`

  **不联网构建**: `build-uxplay.sh` 默认先找本地 `vendor/uxplay-master.tar.gz`, 没有才去 GitHub 下载。
  所有板端改动都在 `../patches/` 里 (4 个补丁), 不用改上游代码, 升级时重打即可。
