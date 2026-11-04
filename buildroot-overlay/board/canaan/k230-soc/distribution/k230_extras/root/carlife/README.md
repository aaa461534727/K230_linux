# K230 CarLife（有线优先）

> 本目录 = `K230/src/carlife/` (CarLife 车机端源码与文档)。调研背景见 `K230/docs/CARLIFE_RESEARCH.md`。

## 现状

| 阶段 | 状态 |
|---|---|
| AOA 握手 + accessory 重枚举 | ✅ 已实现（`carlife_usb.c`） |
| 私有帧编解码（外层/内层/protobuf） | ✅ 已实现（`carlife_proto.c`） |
| 会话状态机（HU_VERSION → VIDEO_START） | ✅ 已实现（`carlife_sink.c`） |
| H.264 → VPU 硬解 → drmplay | ✅ 复用 Miracast 管线（`/run/carlife.h264`） |
| PCM 音频 → pcmplay | ✅ 写 `/run/wfd-audio.pcm` |
| 单点触摸上行 | ✅ `/dev/input/event0` → `TOUCH_ACTION` |
| OEM 鉴权 | ❌ 未实现（无密钥）；手机若强制鉴权会拒 |
| 多点触摸 / 键盘 / 麦克风 | ⏳ 未做 |
| 无线（WiFi） | ⏳ 二期 |

## 编译（板上本机）

```sh
apt-get install -y libusb-1.0-0-dev      # 一次性
sh build.sh                              # 产出 ./carlife_sink
```

## 运行

```sh
# 前台跑, 日志同时写 /var/log/carlife.log
./carlife_sink --width 1280 --height 720 --fps 30 --verbose
```
- 插上安卓手机, 打开 **CarLife App**（手机端要装），程序自动 AOA 切换 → 等重枚举 → 建链；
- 画面走 `v4l2_dec → /run/wfd-player.fifo → drmplay`（与 Miracast/AirPlay 共用上屏进程）；
- 触摸读 `/dev/input/event0` 并按 480x800 → 视频坐标线性映射上行。

## 开机自启

```sh
cp carlife_sink /root/ && cp k230-carlife.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now k230-carlife
```

## 判据（真机验证时看）

```sh
tail -f /var/log/carlife.log      # [usb] AOA53 已下发 → accessory 就绪 → 版本匹配 → VIDEO_INIT_DONE
grep -c VIDEO_DATA /var/log/carlife.log
drmdump /dev/dri/card0 /tmp/c.jpg  # 有画面时非黑 >0%
```

## 已知限制
1. **OEM 鉴权**：不伪造 ACK；遇到会明确打日志。
2. 触摸映射是**线性比例**（屏 480x800 → 视频 WxH），drmplay 若做信箱化会有边距误差，后续按 `/run/wfd-uibc-geom` 同款做法改成发布真实绘制矩形。
3. 只做 **有线**；无线（AP + 发现 + TCP）列二期。
4. 与 Miracast/AirPlay 共用上屏管道：CarLife 起来时若另一路在投，会抢 `/run/wfd-player.fifo`，需要接仲裁（二期）。
