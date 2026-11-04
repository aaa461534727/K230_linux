# CarLife 有线 (USB AOA) 协议要点 —— 实现依据

> 来源: 百度车机库的 wire 格式 (社区逆向) + `SadykovIV/CarLife-HeadUnit-Emulator` 的独立实现。
> 本仓 `carlife_proto.c` 与之一一对应；抓包可对照 `docs/../docs/CARLIFE_RESEARCH.md §4`。

## 1. AOA (Android Open Accessory) 握手

| 步骤 | 控制传输 | 说明 |
|---|---|---|
| ① | `0xC0, bRequest=51` | 读协议版本 (返回 1 或 2) |
| ② | `0x40, bRequest=52, wIndex=0..5` | 6 条识别串 (UTF-16LE + NUL)：`Baidu` / `CarLife` / `Baidu CarLife` / `1.0.0` / `http://carlife.baidu.com/` / `0720SerialNo.` |
| ③ | `0x40, bRequest=53` | 让手机切 accessory 模式 |
| ④ | — | 手机重新枚举为 **18d1:2d00 / 18d1:2d01**，interface 0 的 bulk IN/OUT 即数据通道 |

## 2. 帧格式 (大端)

```
外层: u32 channel | u32 inner_len
内层: CMD(1)/TOUCH(6):  u16 payload_len | u16 reserved(0) | u32 msg | payload
      其它通道:          u32 payload_len | u32 timestamp    | u32 msg | payload
```

- `channel = msg >> 16`，不符即丢帧；`reserved` 非 0 视为错帧；`payload_len` 必须等于 `inner_len - 头长`。
- 上限 4 MB（视频大帧保护）。

## 3. 通道与消息

| 通道 | 值 | 消息 |
|---|---|---|
| CMD | 1 | `HU_VERSION 0x18001` / `VERSION_MATCH 0x10002` / `HU_INFO 0x18003` / `MD_INFO 0x10004` / `VIDEO_INIT 0x18007` / `VIDEO_INIT_DONE 0x10008` / `VIDEO_START 0x18009` / `VIDEO_PAUSE 0x1800A` / `VIDEO_RESET 0x1800B` |
| VIDEO | 2 | `VIDEO_DATA 0x20001` (payload = H.264 Annex-B 访问单元) / `VIDEO_HEARTBEAT 0x20002` |
| MEDIA | 3 | `MEDIA_INIT 0x30001`(f1 采样率/f2 声道/f3 位深) / `MEDIA_STOP 0x30002` / `MEDIA_DATA 0x30006` (裸 PCM) |
| TTS | 4 | `TTS_INIT 0x40001` / `TTS_STOP 0x40002` / `TTS_DATA 0x40003` |
| TOUCH | 6 | `TOUCH_ACTION 0x68001`：protobuf `f1=action(0 down/1 up/2 move), f2=x, f3=y`；`ACTION3 0x6800E` 为多点变体 |

## 4. 建链时序 (无鉴权 profile)

```
HU → HU_VERSION(f1=1, f2=0)
MD → VERSION_MATCH(f1=1)
HU → HU_INFO(型号/名称) + VIDEO_INIT(f1=宽, f2=高, f3=帧率)
MD → MD_INFO            (可忽略)
MD → VIDEO_INIT_DONE(f1..f3 = 实际编码尺寸)
HU → VIDEO_START
MD → VIDEO_DATA …       (可随时 HEARTBEAT)
```

## 5. ⚠ OEM 鉴权

部分车机/手机组合在建链后要求 **0x18048 / 0x10049 / 0x1804A / 0x1004B / 0x1806B / 0x18083** 这一族鉴权报文（需要百度车机 SDK 的密钥）。本实现**不伪造鉴权**：遇到时只在日志写明 `⚠ 手机要求 OEM 鉴权`，会话大概率被手机拒绝。

这也是社区模拟器只敢跑 `SYNTHETIC_NO_AUTH` profile 的原因 —— 能否和市售 CarLife App 直连，取决于手机端是否强制鉴权（需真机验证）。
