# backend —— 屏幕让位 + 投屏服务启停

> 总说明先看 [`../README.md`](../README.md)。这里讲协议、模式脚本、部署与排错。

## 1. 三个部件

| 部件 | 语言 | 职责 |
|---|---|---|
| `k230-ui-backend` | C | 守护进程: 监听 Unix socket, 收 UI 命令, 起停模式脚本, 探会话, 自动收尾 |
| `k230-ui-mode.sh` | sh | **唯一**做"先停对方再起自己"的地方 (DRM 互斥) |
| `k230ui-ctl` | C | 命令行调试 (板上没 UI 时用它验证后端) |

## 2. 协议 (Unix socket 文本行, 一行一条)

Socket: `/run/k230-ui/backend.sock` (权限 0666, 目录由后端自己建)

| 请求 | 响应 (示例) | 说明 |
|---|---|---|
| `STATUS` | `OK state=waiting service=miracast page=miracast mode=wait:miracast msg=miracast waiting for device` | 查当前状态 |
| `SELECT miracast` | `OK state=starting service=miracast page=miracast msg=starting miracast` | 起某一路 (miracast/airplay/carlife/carplay), **只起服务不让屏** |
| `STOP` | `OK state=stopping service=miracast page=none msg=stopping` | 停会话, 回主界面 |
| `AUTO` | `OK state=stopping ...` | 停会话, 交还开机仲裁器 (`k230-source-arbiter`) |
| `PING` | `OK state=... msg=pong` | 探活 |
| (未知) | `ERR msg=unknown command xxx` | — |

`state` 取值: `unknown|idle|starting|waiting|running|stopping|error`

| state | 谁上屏 | 含义 |
|---|---|---|
| `idle` | k230-ui | 主界面 (无投屏服务) |
| `starting` | (切模式脚本中) | 正在跑 `k230-ui-mode.sh` |
| `waiting` | **k230-ui (LVGL 二级画布)** | 服务已起、后台等设备; 不刷黑, 不等宽限期, 不自动弹回 |
| `running` | k230-screen (drmplay) | 真实投屏中, 视频流上屏 (k230ui 被停) |
| `stopping` | (切模式脚本中) | 正在回主界面 |

`page` = 后端认为当前该显示的二级画布 (`waiting`/`running` 时 = service; `idle` 时 = `none`),
`mode` = `ui` / `wait:<svc>` / `video:<svc>` / `auto`。

约定 (2026-11-07 起):
- **点主界面按钮 = 自动起服务**: 前端 `pages_show()` + `SELECT` 一起发, 后端进 `waiting`。
- **同一路在 `waiting` 再 SELECT = 幂等重跑 `wait` 模式** (「重新连接」按钮用它补服务/fifo 读者/UI)。
- 忙碌时收到的请求会**排队** (最后一条胜出), 返回 `msg=queued ...`。
- **视频会话的建立与断开由后端自己发现** (见 §4), 自动在 `waiting <-> running` 之间切屏;
  只有 `STOP` 才回主界面。旧版「点运行中同一路 = 停」和「宽限期到点自动回主界面」已作废。

调试:

```bash
k230ui-ctl STATUS
k230ui-ctl SELECT miracast
k230ui-ctl STOP
k230ui-ctl --socket /tmp/k230-ui.sock STATUS     # 指到本机联调的 socket
```

## 3. 模式脚本 (`k230-ui-mode.sh`)

| 命令 | 谁上屏 | 投屏单元 | 仲裁器 | 何时用 |
|---|---|---|---|---|
| `ui` / `stop` | **k230-ui** (主界面) | 全停 | 停 | STOP / 后端启动 |
| `wait <svc>` | **k230-ui** (LVGL 二级画布) | 只起该路 | 停 | SELECT 后待投屏; 视频流断开后回二级页 |
| `video <svc>` | **k230-screen** (drmplay) | 该路保持 | 停 | 探到真实会话, 才刷视频流 |
| `select <svc>` | k230-screen (drmplay) | 只起该路 | 停 | 兼容旧手工调用, 直接占屏 |
| `auto` | k230-screen | 全停 (等仲裁器拉) | **起** | AUTO |
| `probe <svc>` | — | — (exit 0 = 会话还在) | — | 调试 |

`wait` 模式的三个关键件 (都在 `/run/k230-ui/`, 重启即失):
- `active-page`: 记当前按钮的 service。`ui` / `auto` 模式**先删它再起 k230-ui**;
  `k230ui` 进程被 `video -> wait` 重启时 `pages_restore()` 读它自动回二级页。
- `hold-screen`: 投屏服务脚本 (`wfd-p2p.sh` 的 `start_player`/`start_blank`,
  `k230-airplay` 的 `ensure_player`) 见此标记就**不碰 DRM / 不拉起 k230-screen**,
  否则会和 k230-ui 抢 master (黑屏/紫屏)。`ui` / `auto` / `select` 清掉它。
- `fifo-keeper.pid`: 待投屏时 `k230-screen` 是停的, 解码器 `open(/run/wfd-player.fifo, O_WRONLY)`
  会因没有读者卡 10 s 超时退出、把 wfd-sink 一起带死 (手机上正在建的 RTSP 会话被掐断)。
  keeper 是一个纯 shell 的读管道丢数据的进程, 顶替 drmplay 当读者, **完全不占 DRM**。
  切 `video` 时先起 k230-screen (drmplay 也成为读者)、再杀 keeper, 全程至少一个读者;
  `k230-screen.sh` 也因此改成**复用已有 fifo inode, 不 unlink 重建**。

关键实现细节 (都是踩过坑换来的, 改之前先读):

1. **先停 UI, 再起 k230-screen** (反了就是黑屏)。中间 `sleep 0.3` 给 drmplay 释放 DRM master。
2. `fast_off()` 用 `systemctl --no-block stop` + **按 cgroup 精确杀进程**:
   - `--no-block`: 停 Miracast 实测要 18 s (wfd-p2p.sh 收尾慢), 但"让位"只要求它立刻停止写 fifo;
   - 按 cgroup: 各路命令行长得一样 (`v4l2_dec /dev/stdin /run/wfd-player.fifo`),
     按命令行 pkill 会误伤正在投的另一路;
   - 连杀 6 轮: Miracast 是 `while :; do sink | dec; done` 自重启外壳, 杀一次 0.5 s 后复活;
   - **一次停多个** (`fast_off_many`): 所有 unit 先一起 `--no-block stop`, 再**一轮**扫描统一杀。
     逐个串行停实测要 19 s 黑屏, 合并后 ~3 s (2026-11-03 板上实测)。
   - ⚠ cgroup 匹配必须用**叶子名全等**, 不能 `grep "k230-ui"` —— 子串会命中 `k230-ui-backend`
     把后端自己杀掉 (2026-11-03 板上踩过: 每次切模式后端都重启)。
3. 手动模式下**必须停仲裁器** —— 否则它按自己的优先级把别的路又拉起来。
4. `select` 会先停掉其它 3 路 —— 手动模式是**严格互斥**, 不靠仲裁器。

## 4. 会话探针 (怎么切屏 / 怎么发现断开)

后端每 2 s 探一次, **连续 2 拍探不到**才判定结束 (端口刚建/刚拆会抖)。
2026-11-07 起**没有宽限期**: `waiting` 态只等服务、一直停在 LVGL 二级画布,
不会到点把用户弹回主界面 (旧版 60/180/1800 s 宽限 `K230UI_GRACE_*` 已删)。

`waiting` 探到 **`video_session_alive(svc)` = 真** → 切 `video <svc>` (屏给 drmplay);
`running` 探到连续 2 拍为假 → 切回 `wait <svc>` (屏还给 LVGL, 自动回二级页)。
判据只看**视频会话**信号, 不看蓝牙反控 (蓝牙是 AirPlay 用户先连的那一步,
它连上不等于出帧; 视频断了蓝牙可能还连着, 算进来就永远回不了二级页)。

| service | 判定 |
|---|---|
| `miracast` | `/proc/net/tcp{,6}` 里有 **7236** 端口的 ESTABLISHED |
| `airplay` | 端口 **7100/7101** 或 `/run/airplay-cast-active` |
| `carplay` | `/run/airplay-active` 存在, 或端口 **7000** |
| `carlife` | 端口 **7240/8240/9240/9241/9242/9340/9440** 或 `/run/carlife-active` |

比 `k230-source-arbiter.sh` 少了一条 `iw dev wlan0 link` 的兜底 (那是 Miracast P2P 的
额外判据); 如果实测发现"手机还连着但被误判结束", 优先加回这一条。

## 5. 部署

```bash
# 交叉编译 (工具链路径见 frontend/README.md §4)
cd K230/src/eezUI/backend
# ⚠ 必须带 BUILD=build-board, 否则 build/ 里是 x86 的, 传到板上 Execute format error
make CC=$PWD/../../../sdk/k230_linux_sdk/output/k230_canmv_01studio_defconfig/host/bin/riscv64-unknown-linux-gnu-gcc \
     BUILD=build-board

# 一键装 (scp 三个产物 + 两个 unit + 配置模板; 停仲裁器; 起 backend)
#   板子地址是机器私有事实: python3 bin/machine local K230
#   只开了密码登录时用 SSH/SCP 把 sshpass 包进去:
SSH="sshpass -p root ssh -o StrictHostKeyChecking=no" \
SCP="sshpass -p root scp -o StrictHostKeyChecking=no" \
BOARD_HOST=root@10.0.102.172 bash tools/board-install.sh
```

装完后的板上布局:

```
/usr/bin/k230ui              ← 主界面 (frontend/build-board/k230ui)
/usr/bin/k230-ui-backend     ← 守护进程
/usr/bin/k230ui-ctl          ← 调试 CLI
/usr/bin/k230-ui-mode.sh     ← 模式脚本
/etc/k230-ui.conf            ← 按钮 -> unit 映射 (可从 k230-ui.conf.example 复制)
/etc/systemd/system/k230-ui.service
/etc/systemd/system/k230-ui-backend.service
```

**只 enable `k230-ui-backend.service`**, 它启动后第一件事是 `k230-ui-mode.sh ui` 把主界面拉起来。
`k230-ui.service` 不要 enable (会和 backend 抢启动顺序)。

## 6. 排错

| 现象 | 先看什么 |
|---|---|
| 屏黑 / 紫屏 | `systemctl status k230-ui k230-screen` —— 是不是两个同时在跑 (DRM 冲突) |
| UI 显示 `Backend offline` | `systemctl status k230-ui-backend`; `k230ui-ctl STATUS`; `ls -l /run/k230-ui/backend.sock` |
| 点了没反应 | `journalctl -u k230-ui-backend -f` (带 `--verbose` 能看到每条请求); 再看 `/var/log/k230-ui.log` |
| 起投屏失败 | `cat /var/log/k230-ui.log` 里 `⚠ 起 xxx 失败`; 再 `systemctl status <unit>` 看单元本身为什么起不来 |
| 投完不回主界面 | 探针判定 (见 §4) —— 会话端口可能还挂着 ESTABLISHED (手机没真正断); 60 s 宽限期内本来就不回 |
| 点按钮后 4 s 就弹回主界面 | 宽限期被改小了 (见 §4) |
| 屏全黑但进程在跑 | `cat /sys/kernel/debug/dri/0/clients` 看 `master=y` 的是谁 —— 有残留 `drmplay`/`k230ui` 就 `k230-ui-mode.sh ui` 强制重整 |
| 想手动强制回 UI | `k230ui-ctl STOP`; 或直接 `k230-ui-mode.sh ui` |

日志位置: `/var/log/k230-ui.log` (模式脚本) + `journalctl -u k230-ui-backend` (后端)。
