# eezUI —— K230 上电主界面 (LVGL + EEZ Studio)

> 一句话: 板子上电看到的主界面 —— 4 个按钮 (Miracast / AirPlay / CarLife / CarPlay),
> 点哪个就起哪一路投屏; 投屏时把屏幕让给投屏, 手机断开后自动回到主界面。
>
> 位置: `K230/src/eezUI/` (放在 `src/` 下是为了不破坏「项目顶层只许 9 项」的白名单,
> `eezUI` 本身是一个独立目录, 前端 `frontend/` + 后端 `backend/` 分开)。

---

## 1. 边界与现状 (2026-11-03)

| 项 | 状态 | 证据 |
|---|---|---|
| 主界面 (480x800, 4 按钮 + 状态行) | ✅ 已可跑 | `K230/captures/20261103-0022-k230-eezui-main-idle-sim.png` |
| 点按钮 -> 后端 -> 状态回显 (白边高亮 + 绿字) | ✅ 本机整链打通 (mock 后端) | `K230/captures/20261103-0022-k230-eezui-main-running-sim.png` |
| 板端交叉编译 `k230ui` (RISC-V ELF64, 98K) | ✅ 已编过 | `frontend/build-board/k230ui` |
| **板端实机出画面** (LVGL 走 SDK 的 `liblvgl_linux.so` + DRM + evdev) | ✅ **已上真板** | `K230/captures/20261103-1642-k230-eezui-board-main-ui.png` (板上 `drmdump` 抓 scanout 转 PNG) |
| **四个按钮真起服务 + 屏幕让位** | ✅ **板上实测 9 个场景全绿** | `K230/logs/20261103-1642-k230-eezui-board-e2e.log` |
| **上电自动进主界面** (reboot 实测) | ✅ 重启后 `k230-ui-backend` 自己把界面拉起来 | 同上日志 (uptime 0 min + `mode=ui` + `k230ui master=y`) |
| 中文文案 | ⏳ 未做 (当前英文, 用 LVGL 内置 Montserrat; 中文需另做 TTF 子集字体) | — |
| 手指实测 (真触摸点击) | ⏳ 未做 (硬件要人按); 按钮→socket 链路用 `k230ui-ctl` 等价验证 + 模拟器真 CLICKED 事件验证 | — |

---

## 2. 目录

```
K230/src/eezUI/
├── README.md                 ← 本文件 (总入口)
├── frontend/                 ← 界面 (EEZ Studio 工程 + LVGL 代码 + 模拟器)
│   ├── eez-project/          ← EEZ Studio 工程 (k230-ui.eez-project)
│   ├── ui/                   ← EEZ **生成**的 C 代码 (不要手改, 会被覆盖)
│   ├── src/                  ← 手写代码 (main_sdl/main_board/ui_actions/backend_client)
│   ├── tools/make-eez-project.py   ← 生成 .eez-project 的脚本 (改布局/文案从这里改)
│   ├── tools/board-build.sh        ← 交叉编译板端 k230ui
│   ├── scripts/              ← fetch-lvgl / build-sim / run-sim
│   ├── lv_conf.h             ← 本机模拟器配置 (SDL2)
│   └── lv_conf_board.h       ← 板端配置副本 (必须和 liblvgl.so 一致!)
└── backend/                  ← 后端 (屏幕让位 + 服务启停)
    ├── k230-ui-backend.c     ← 守护进程 (Unix socket, 文本行协议)
    ├── k230-ui-mode.sh       ← 模式切换 (ui / select <svc> / stop / auto / probe)
    ├── systemd/              ← k230-ui.service + k230-ui-backend.service
    ├── k230-ui.conf.example  ← 按钮 -> unit 映射
    └── tools/                ← k230ui-ctl (调试 CLI) + mock 脚本 + 安装脚本
```

---

## 3. 为什么要后端 (核心矛盾: DRM 只能有一个主人)

板子上有两个都想画屏的进程:

| 进程 | 干什么 | 谁起 |
|---|---|---|
| `k230-ui` (**k230ui**) | LVGL 主界面, DRM 全屏 | `k230-ui-backend` |
| `k230-screen` (**drmplay**) | 常驻播放器, 读 `/run/wfd-player.fifo` 显示投屏画面 | `k230-ui-backend` (投屏时) |

**两者绝对不能同时开** —— DRM master 只能有一个, 后起的那个拿不到就黑屏/紫屏。
所以必须有"唯一决策者"决定**此刻谁该上屏**, 这就是后端:

```
   用户点按钮
       │
       ▼
   k230ui  ──SELECT miracast──►  k230-ui-backend  ──fork──►  k230-ui-mode.sh select miracast
                                      ▲                            │
                                      │                            ├─ 停 k230-ui        (让出 DRM)
                                      │                            ├─ 停仲裁器 (手动模式)
       ┌──STATUS──────────────────────┘                            ├─ 停其它投屏路
       │                                                           ├─ 起 k230-screen    (接管 DRM)
   k230ui 显示 "Miracast running"                                   └─ 起 k230-miracast
```

手机断开后: 后端自己探端口 (`/proc/net/tcp` 的 ESTABLISHED), 连续 2 次探不到
=> 自动 `stop` => 回主界面。**用户不需要手动关**。

---

## 4. 快速上手

### 4.1 本机看界面 (不用板子)

```bash
cd K230/src/eezUI/frontend
bash scripts/fetch-lvgl.sh      # 拉 LVGL 9.6.0 (一次性, 约 60 MB, 已被 .gitignore 排除)
bash scripts/build-sim.sh       # 编出 build-sim/k230ui-sim
bash scripts/run-sim.sh         # 开窗口, 鼠标 = 触摸
```

没有图形界面 (纯 ssh) 想截图:

```bash
Xvfb :99 -screen 0 600x900x24 &
DISPLAY=:99 K230UI_SIM_EXIT_MS=3000 ./build-sim/k230ui-sim &
sleep 2; DISPLAY=:99 import -window root /tmp/ui.png
```

此时底部会显示红字 `Backend offline` —— 正常, 本机没起后端。

### 4.2 本机联调"点按钮 -> 真的切模式" (假后端)

```bash
cd K230/src/eezUI/backend
make                                                    # build/k230-ui-backend + build/k230ui-ctl
./build/k230-ui-backend --socket /tmp/k230-ui.sock \
    --mode-cmd ./tools/mock-k230-ui-mode.sh --verbose &
./build/k230ui-ctl --socket /tmp/k230-ui.sock SELECT miracast
./build/k230ui-ctl --socket /tmp/k230-ui.sock STATUS
# 想让界面也连这个假后端:
cd ../frontend && K230UI_SOCK=/tmp/k230-ui.sock bash scripts/run-sim.sh
```

### 4.3 上板 (2026-11-03 已在 K230 真板跑通)

板子地址是**机器私有事实**, 用 `python3 bin/machine local K230` 查本机可达的那个
(2026-11-03 实测可用的是 `root@10.0.102.172`, 密码 `root`)。

```bash
# 1) 交叉编译界面 (用 SDK 自带 riscv64 工具链 + staging sysroot, 不重编 LVGL)
cd K230/src/eezUI/frontend && bash tools/board-build.sh

# 2) 交叉编译后端 (⚠ 一定要指定 CC, 否则编出来是 x86, 传到板上 Execute format error)
cd ../backend && make CC=$PWD/../../sdk/k230_linux_sdk/output/k230_canmv_01studio_defconfig/host/bin/riscv64-unknown-linux-gnu-gcc BUILD=build-board

# 3) 一键装到板子 (scp + systemd + 停仲裁器 + 起 backend)
#    板子只开了密码登录时, 用 SSH / SCP 变量把 sshpass 包进去:
cd .. && SSH="sshpass -p root ssh -o StrictHostKeyChecking=no" \
           SCP="sshpass -p root scp -o StrictHostKeyChecking=no" \
           BOARD_HOST=root@10.0.102.172 bash backend/tools/board-install.sh

# 4) 板上自检
ssh root@10.0.102.172 'k230ui-ctl STATUS; systemctl --no-pager status k230-ui'
ssh root@10.0.102.172 'k230ui-ctl SELECT miracast'   # 等价于点 Miracast 按钮
ssh root@10.0.102.172 'k230ui-ctl STOP'              # 等价于点"回主界面"
```

装完后板上的形态:

| 单元 | 开机 | 谁管 |
|---|---|---|
| `k230-ui-backend` | **enable** (自启) | 启动第一件事就是 `k230-ui-mode.sh ui` ⇒ 屏上出现主界面 |
| `k230-ui` | disable | 由 backend 按需起停 (enable 会抢启动顺序) |
| `k230-screen` | disable | 投屏时才起 (它就是 `drmplay`) |
| `k230-source-arbiter` | disable | 只有按 `AUTO` 才起 (交还开机仲裁器) |

### 4.4 板上抓证据 (没有显示器也能看界面)

```bash
ssh root@10.0.102.172 '/usr/bin/drmdump'            # dump 当前 scanout -> /tmp/frame.bgra (480x800 BGRA)
sshpass -p root scp root@10.0.102.172:/tmp/frame.bgra /tmp/frame.bgra
convert -size 480x800 -depth 8 bgra:/tmp/frame.bgra /tmp/ui.png   # 转 PNG 看
```

`drmdump` 抓的是**屏上真正扫出去的像素**, 所以它同时证明"程序没崩"和"画面真的上屏了"
(UI 拿不到 DRM master 时抓到的是全黑, md5 `f8860b77…`)。

### 4.5 板上踩过的坑 (2026-11-03 现场, 改这里之前先看)

| # | 现象 | 根因 | 现在的做法 |
|---|---|---|---|
| 1 | `k230ui` 起来了但屏全黑, 日志 `drmModeAtomicCommit failed: Permission denied (13)` | 还有一个 `drmplay` 活着且是 **DRM master** (`/sys/kernel/debug/dri/0/clients` 里 `master=y`)。手工 `systemctl stop` 后 unit 被 `Restart=always`/自启外壳又拉起来 | 只走 `k230-ui-mode.sh`: 它 `--no-block stop` + 按 `/proc/<pid>/cgroup` 连杀 6 轮; 起界面前还会 `pkill -x k230ui` 清掉**不在任何 unit 里**的游离进程 |
| 2 | **每次切模式, 后端自己跟着重启**, 起来后又把模式抢回 `ui` | `fast_off` 用 `grep "k230-ui"` **子串**匹配 cgroup, 把 `k230-ui-backend.service` 也杀了 | cgroup 叶子名 (`k230-ui-backend`) 与 unit 名 (`k230-ui`) **全等**比较 |
| 3 | 点了 Miracast, 4 秒后自己弹回主界面 (看起来"按钮没反应") | 会话探针没有宽限期, 手机还没连上就判"会话结束" | 进入某一路后有 **60 s 宽限** (`GRACE_DEFAULT_S`), 期间显示 `<svc> waiting for device` |
| 4 | 从 `AUTO` 按 STOP, 屏上还是仲裁器的画面 | `AUTO` 跑完 `g_st.state` 也是 `IDLE`, STOP 被当成 "already idle" 丢弃 | 后端单独记**真实模式** `g_mode`; 只有 `mode=ui` 才回 already idle |
| 5 | "回主界面"要等 **19 s** 黑屏 | 7 个 unit **逐个**串行 stop + 每轮扫 `/proc` 还 fork `grep` (本机 500 进程 × 8 unit = 4000 次 fork) | ① 所有 unit 一次 `--no-block stop` ② 一轮扫描统一杀 ③ 扫 `/proc` 用 shell 内建 `read`/`case` 不 fork ⇒ **19 s → 3 s** |
| 6 | 切到某一路后 `k230-screen` 有一两秒是 inactive | 别的路的 `ExecStop` (`wfd-p2p.sh stop`) 会顺带 `systemctl restart k230-screen`, 这波浪落在我们刚起的 screen 之后 | 起完目标服务后**复查**一次 `unit_on k230-screen` + `pgrep -x drmplay`, 不在就补起 |
| 7 | 屏幕右下角一直挂着 `30 FPS \| CPU (0% \| 0%)` | SDK 的 `lv_conf.h` 开了 `LV_USE_PERF_MONITOR=1`, 开关编进 `liblvgl.so` 改不了 | 运行时调 `lv_sysmon_hide_performance(lv_display_get_default())` (weak 引用, 模拟器没这符号就跳过) |
| 8 | `scp k230ui /usr/bin/k230ui` 报 `Text file busy` | 程序正在跑, 内核不许覆盖可执行文件 | 传 `/tmp/xxx.new` 再 `mv` (board-install.sh 已改) |
| 9 | 板上有个 `k230-carlife` (有线) 一直 `activating`, restart counter 793 | 该 unit 的 `ExecStart=/root/carlife_sink` 不存在 (203/EXEC), 又 `Restart=always` | 与 UI 无关; `systemctl stop k230-carlife && systemctl reset-failed k230-carlife` 收掉即可 (有线 CarLife 本来就不用, 会切走调试网卡) |

---

## 5. 前端文件到底谁做? (回答"我要怎么配合你")

**结论: 前端 AI 可以全包, 你只在需要调视觉时进 EEZ Studio 拖两下。**

原因: EEZ Studio 的工程文件 (`.eez-project`) 就是一份 JSON, 里面描述
"有哪些页面/控件/坐标/颜色/事件绑定"。所以前端被拆成三段, 各自可替换:

| 段 | 谁产生 | 能不能重生成 | 说明 |
|---|---|---|---|
| ① 界面布局/样式 | `tools/make-eez-project.py` **或** EEZ Studio 手工拖 | ✅ 都能 | 脚本生成 = 改代码就改界面; EEZ 手工拖 = 所见即所得 |
| ② 生成 C 代码 | EEZ Studio 无头构建 (`--build-project`) | ✅ 随时 | `ui/*.c/h`, **不要手改** |
| ③ 事件响应 | `src/ui_actions.c` (手写, 只有 5 个函数) | ❌ 手写 | 这里才有业务逻辑 (调后端 + 刷状态) |

也就是说: **界面长什么样** 完全在 ①, **点下去干什么** 只在 ③ 的 5 个函数里,
两者之间由 EEZ 生成的 `ui/actions.h` 按名字连起来 (`action_on_miracast_click` 等)。

所以配合方式 (任选):
- **你自己动手调外观**: `EEZ Studio` 打开 `frontend/eez-project/k230-ui.eez-project`,
  拖完保存, 再 `cd ~/.local/opt/eez-studio/squashfs-root && ./"EEZ Studio" --no-sandbox --build-project <abs path>`
  重新生成 `ui/` 即可 (生成了也不要手改, 因为下次会覆盖)。
  ⚠ 一旦在 EEZ Studio 里改过, **别再跑 `make-eez-project.py`** (会覆盖你的手工改动)。
- **告诉我改什么** (按钮文案/颜色/布局/加图标/加页面): 我改 `tools/make-eez-project.py`
  重新生成, 你只验收。
- **要中文**: 需要把 TTF 做成 LVGL 字体 (子集, 否则字库很大), 我可以用
  `lv_font_conv` 生成; 现在先用英文 (LVGL 内置 Montserrat, 零额外体积)。

---

## 6. 本机环境 (已装好, 2026-11-03)

| 组件 | 版本/位置 | 用途 |
|---|---|---|
| EEZ Studio | 0.29.0, 解包在 `~/.local/opt/eez-studio/squashfs-root/` | 画界面 + 生成 C 代码 |
| LVGL | 9.6.0 (`scripts/fetch-lvgl.sh` 拉) | 本机模拟器; **版本刻意和板端一致** |
| SDL2 | 2.20 (系统包) | 本机开窗口 |
| gcc / cmake | 11.4 / 3.28 | 编模拟器 |
| Xvfb + ImageMagick | 已装 | 无显示器时截图 (`import -window root`) |

EEZ 工程里的 `lvglVersion` 填的是 **9.5.0**: EEZ 0.29 只支持到 9.5.0 (没有 9.6),
生成的代码是标准 LVGL 9 API, 在 9.6 上实测可编可跑 (就是本机这套)。

---

## 7. 安全规则 (改这个目录前必读)

1. **`frontend/ui/` 是生成物**, 手改会在下次生成时被覆盖; 要改就改
   `tools/make-eez-project.py` 或 `ui_actions.c`。
2. **板端 `lv_conf.h` 必须和 `liblvgl.so` 一致** —— `lv_conf_board.h` 是 SDK 编库时那份的
   副本, `board-build.sh` 优先用 SDK 现场生成的, 不要自己改配置项(开关不一样 = 结构体布局
   对不上 = 随机崩)。
3. **切屏幕必须先停对方再起自己** —— 全部逻辑集中在 `backend/k230-ui-mode.sh`, 不要在别处
   直接 `systemctl start k230-screen`。
4. **不要 enable `k230-ui.service`** —— 它由 backend 按需起停, enable 了会和 backend 抢启动顺序。
5. 有线 CarLife (`k230-carlife`) 会把 USB gadget 切到 AOA, **同时切走调试网卡 = SSH 断**,
   默认不用它 (见 `backend/k230-ui.conf.example`)。
