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
| `STATUS` | `OK state=running service=miracast msg=miracast running` | 查当前状态 |
| `SELECT miracast` | `OK state=starting service=miracast msg=starting miracast` | 起某一路 (miracast/airplay/carlife/carplay) |
| `STOP` | `OK state=stopping service=miracast msg=stopping` | 停会话, 回主界面 |
| `AUTO` | `OK state=stopping ...` | 停会话, 交还开机仲裁器 (`k230-source-arbiter`) |
| `PING` | `OK state=... msg=pong` | 探活 |
| (未知) | `ERR msg=unknown command xxx` | — |

`state` 取值: `unknown|idle|starting|running|stopping|error`

约定:
- **点同一路 (运行中) = 停** —— 后端把 `SELECT` 自动转成停, UI 不用额外按钮。
- 忙碌时收到的请求会**排队** (最后一条胜出), 返回 `msg=queued ...`。
- 手机断开由后端自己发现 (见 §4), UI 不负责。

调试:

```bash
k230ui-ctl STATUS
k230ui-ctl SELECT miracast
k230ui-ctl STOP
k230ui-ctl --socket /tmp/k230-ui.sock STATUS     # 指到本机联调的 socket
```

## 3. 模式脚本 (`k230-ui-mode.sh`)

| 命令 | 谁上屏 | 投屏单元 | 仲裁器 |
|---|---|---|---|
| `ui` / `stop` | **k230-ui** (主界面) | 全停 | 停 |
| `select <svc>` | **k230-screen** (drmplay) | 只起该路 | 停 |
| `auto` | k230-screen | 全停 (等仲裁器拉) | **起** |
| `probe <svc>` | — | — (exit 0 = 会话还在) | — |

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

## 4. 会话探针 (手机断开怎么发现)

后端每 2 s 探一次, **连续 2 拍探不到**才判定结束 (防抖)。
刚切进某一路还有 **60 s 宽限期** (`GRACE_DEFAULT_S`): 手机上找设备/连热点本来就要十几秒到一分钟,
宽限期内探不到**不算断开**, 只把状态文案改成 `<svc> waiting for device`。

| service | 判定 |
|---|---|
| `miracast` | `/proc/net/tcp{,6}` 里有 **7236** 端口的 ESTABLISHED |
| `airplay` | 端口 **7100/7101** |
| `carplay` | `/run/airplay-active` 存在, 或端口 **7000** |
| `carlife` | 端口 **7240/8240/9240/9241/9242/9340/9440** |

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
