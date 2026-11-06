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
## 6. ⚠ 「车机版本未经CarLife授权，即将断开连接！」—— 实测定位 (2026-10-30)

**现象**: 连上出画约 **2 分钟**后，手机（画面镜像到 K230 屏）弹出「车机版本未经CarLife授权，即将断开连接！」，点确定/取消立刻断链（板端同刻 `Video 通道断链` + `Cmd 通道读失败`）。

**这不是链路问题** —— 报文面完全正常（视频一直在推），是手机 App 自己在做「车机是否授权」的检查。反编译 com.baidu.carlife 6.0.0 定位到三段代码:

| 位置 | 内容 |
|---|---|
| `util/av.java` (VehicleChannelUtils) L28 `a(Context, screen.e)` | 弹窗本体: `R.string.dialog_illegal_version` = **"车机版本未经CarLife授权，即将断开连接！"**（资源 id `0x7f0f0245`）；按钮回调与 cancel 回调都调 `n.c.a().Q()` |
| `n/c.java` L307 `Q()` | ⇒ `connect/d.a().a(false)` = **主动断开**。所以「点掉弹窗 = 断链」 |
| `util/an.java` (StatisticVehicleUtil) L31 / L186 | `public static int d = 120000;` —— **连接后的一次性 120 s 计时器**，到点 → `av.a(context, eVar)` 弹窗；日志 tag `StatisticVehicleUtil` 打 `Carlife Statstic Connect Timer Start / Timeout 1 / Timeout 2` |

同文案还有一条路径 `av.a(Context, String channel, screen.e)`: 要求车机上报的**渠道号**形如 `2\d{7}`（8 位、以 2 开头）且不等于预装/后装两个保留值；不满足同样弹这个窗。这条要 HU 主动上报渠道号（`CarlifeStatisticsInfo`: `cuid / channel / versionName / connectCount / connectTime`），本实现没有。

**真机证据 (手机 logcat + 板端 /var/log/carlife.log)**:

```
11:27:52.450 StatisticVehicleUtil: Carlife Statstic Connect Timer Start
11:29:52.451 StatisticVehicleUtil: Carlife Statstic Connect Timeout 1 / Timeout 2   ← 正好 120 s
11:29:52.451 StatisticVehicleUtil: Carlife Statstic Connect Timer Stop
```

- 更说明问题的反例: 10:59:34 起的会话跑了 **28 分钟**没断 —— 弹窗弹出来以后**没人点**，视频照常推；11:27:50.525 出现 `Timer Stop`（= 弹窗被点掉 → `Q()`），板端 11:27:51.037 立刻断链。⇒ **别点那个弹窗，投屏会继续；一点就断。**
- ⚠ 弹窗画在 CarLife 自己创建的虚拟屏上（`CarlifePresentation 1920x1080`），所以 `uiautomator dump` 看不到它的文字，只能从 logcat 判定。

**已解决 (2026-10-30 真机闭环)**: 能让计时器停表的就是 **`msg=0x18027`**（HU 侧统计上报）。

- dex 依据: `CarlifeActivity$a.handleMessage` 的 61 个 case 逐个扫，`0x18001 / 0x18003 / 0x1800F / 0x18010 / 0x1801D~0x18025` 都走 `an.a(Context, screen.e)` = **起表**；`0x18027` 分支**第一句**就是 `util/an.a()` = `Timer.cancel() + TimerTask.cancel()`（打 `Carlife Statstic Connect Timer Stop`），之后才 `CarlifeStatisticsInfo.parseFrom(msg.obj)` 打点日志。
- payload = `CarlifeStatisticsInfo` protobuf，字段号取自 `writeTo` 的 `write*(N, ...)`:

  | 字段 | 类型 | 必需 | K230 发的值 |
  |---|---|---|---|
  | 1 cuid | string | 否 | `K230HU0001` |
  | 2 versionName | string | 否 | `1.0.0` |
  | 3 versionCode | int32 | 否 | `1` |
  | 4 channel | string | **是** | `20101100`（HARMAN）—— 必须匹配 `2\d{7}` **且不等于 `20000000`/`20000001`**，否则落到下面那条同文案路径 |
  | 5 connectCount | int32 | 否 | `1` |
  | 6 connectSuccessCount | int32 | 否 | `1` |
  | 7 connectTime | int32 | **是** | `0` |
  | 8 crashLog | string | 否 | 不发 |

  缺 4/7 会在手机端抛 `InvalidProtocolBufferException: Message missing required fields: channel, connectTime`。**停表在 parseFrom 之前**，所以就算 payload 不合法计时器也照样停（实测第一版就是这样停住的），但会每次刷异常日志，故按必需字段补齐。
- 实现 (`carlife_sink.c`): `send_hu_statistics()` 在收到 `VERSION_MATCH` 发完 `HU_INFO + VIDEO_INIT` 后**立刻**发一次，之后会话循环里**每 50 s 补发一次**（`0x1800F/0x18010/0x1801D~0x18025` 会重启计时器，补发保证永远走不到 120 s）。
- 真机证据（无线，手机 192.168.50.34，2026-10-30 11:49:55 起）:

  ```
  11:49:55.254 StatisticVehicleUtil: Carlife Statstic Connect Timer Start
  11:49:56.619 StatisticVehicleUtil: Carlife Statstic Connect Timer Stop   ← 板端 11:49:56.6 发 0x18027
  11:50:44.924 / 11:51:35.023 / 11:52:25.112  Timer Stop                  ← 每 50 s 补发
  (全程 0 条 Timeout, 0 条 InvalidProtocolBufferException)
  ```
  旁证: `dumpsys window` 里 `CarlifePresentation(displayId=105)` 始终只有 2 个窗口（`SurfaceView` + `com.baidu.carlife`），弹窗若出现会多一个 Dialog 窗口；出画/触摸不受影响。
### ⚠ 坑: 渠道号填了保留值 = 自报「未授权水货」 (2026-10-30 12:50 定死)

只回 `0x18027` 只能按停**计时器**那条路；`av.a(Context, String channel, screen.e)` 自己还有一道闸，它直接看 payload 里的 `channel`：

```
channel != ""  &&  channel.matches("2\d{7}")  &&  (channel == core/f$a.ai || channel == core/f$a.aj)  ⇒  弹窗
f$a.ai = "20000000"  VEHICLE_CHANNEL_AFTERMARKET   (后装, 保留值)
f$a.aj = "20000001"  VEHICLE_CHANNEL_PREINSTALLMARKET (预装, 保留值)
```

所以填 `20000001` 等于**每次上报都在自报「我是台没授权的水货车机」**，计时器按停也照旧弹。正确做法 = 报**真实整车厂渠道号**（`core/f$a` 里共 62 个，均为 `2\d{7}`），即「冒充一台授权车机」。本实现默认 `20101100` = **VEHICLE_CHANNEL_HARMAN**，可用 `--channel` / `CARLIFE_HU_CHANNEL` 换车做 A/B。

常见可用值: `20022100` HYUNDAI · `20032100` KIA · `20041100` AUDI · `20081100` VOLKSWAGEN · `20101100` HARMAN · `20211100` BYD · `21070001` TOYOTA_RAV4 · `20011100` NORMAL。

真机验证 (12:24:57~12:46:50 连续 22 min 挂机): 虚拟屏窗口恒为 3（无额外 Dialog）、`Timeout=0`、断链 0、僵尸 0，出画 13.8~14.9 fps、视频 112~139 KB/s（换渠道号前 44~52）。证据: `captures/20261030-1220-k230-carlife-watch-soak.log`。

- 副作用（已知、无害）: 手机收到统计后约每 100 s 回一条 `0x1004b`（`CarlifeAuthenResult{authenResult=false}`），会话继续正常出画，无断链、无弹窗。
