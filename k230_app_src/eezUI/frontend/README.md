# frontend —— K230 主界面 (EEZ Studio 工程 + LVGL 代码)

> 总说明先看 [`../README.md`](../README.md)。这里只讲前端怎么改、怎么编。

## 1. 三段式: 哪些文件能重生成, 哪些是手写的

```
eez-project/k230-ui.eez-project   ← ① 工程 (布局/控件/事件绑定都在里面)
        │  EEZ Studio --build-project
        ▼
ui/*.c  ui/*.h                    ← ② 生成物 (禁止手改!)
        │
src/ui_actions.c                  ← ③ 手写: 5 个 action 的业务逻辑
src/backend_client.c              ← ③ 手写: 跟后端说话的 socket 客户端
```

- 改**长什么样**: 改 `tools/make-eez-project.py` (推荐, 一行改文案) 或用 EEZ Studio 拖。
- 改**点下去干什么**: 只改 `src/ui_actions.c`。
- 改**跟后端说什么**: 只改 `src/backend_client.c` + `../backend/`。

## 2. 改界面 (两种方式, 二选一)

### 2.1 改脚本再生成 (推荐给 AI / 批量改动)

```bash
cd K230/src/eezUI/frontend
# 改 tools/make-eez-project.py 顶部的 LAYOUT 区 (文案/颜色/坐标/字体)
python3 tools/make-eez-project.py
cd ~/.local/opt/eez-studio/squashfs-root
./"EEZ Studio" --no-sandbox --build-project \
    /home/dengbaowen/freebuf/hclinux/K230/src/eezUI/frontend/eez-project/k230-ui.eez-project
```

输出 14 个文件到 `frontend/ui/`, 正常打印 `No error and no warning detected`。

> ⚠ `./AppRun` 启动会报 `/EEZ Studio: No such file or directory` (APPDIR 没设),
> 必须直接在 `squashfs-root/` 里跑 `./"EEZ Studio"`。

### 2.2 用 EEZ Studio 拖 (推荐给人调视觉)

```bash
~/.local/opt/eez-studio/squashfs-root/"EEZ Studio" \
    K230/src/eezUI/frontend/eez-project/k230-ui.eez-project
```

拖完 `Ctrl+S`, 再用 2.1 的第二条命令生成代码。
**注意: 一旦手工拖过, 就别再跑 `make-eez-project.py`** —— 它会按脚本里的布局覆盖你的改动。
(想让脚本也不丢, 就把你调好的数值抄回脚本的 LAYOUT 区。)

## 3. 本机模拟器 (SDL2)

```bash
bash scripts/fetch-lvgl.sh    # 一次性: 拉 LVGL v9.6.0 到 third_party/lvgl
bash scripts/build-sim.sh     # -> build-sim/k230ui-sim
bash scripts/run-sim.sh       # 480x800 窗口, 鼠标当触摸
bash scripts/build-sim.sh --clean   # 编译缓存乱了就 --clean
```

环境变量:

| 变量 | 作用 |
|---|---|
| `K230UI_SOCK` | 后端 socket 路径 (默认 `/run/k230-ui/backend.sock`) |
| `K230UI_SIM_CLICK` | 启动 1.5 s 后自动点某个按钮 (`miracast/airplay/carlife/carplay`), 用于自动截图 |
| `K230UI_SIM_EXIT_MS` | 跑够多少毫秒自动退出 (CI/截图用) |

## 4. 板端编译

```bash
bash tools/board-build.sh          # -> build-board/k230ui  (RISC-V ELF64)
bash tools/board-build.sh --copy   # 顺手 scp 到 $BOARD_HOST:/usr/bin/k230ui
```

- 交叉工具链: `K230/sdk/k230_linux_sdk/output/<cfg>/host/bin/riscv64-unknown-linux-gnu-gcc`
- sysroot: `.../output/<cfg>/staging` (`usr/include/lvgl` + `usr/lib/liblvgl*.so`)
- **不重编 LVGL**: 板上已有 SDK buildroot 编好的 9.6.0 (`liblvgl.so` + `liblvgl_linux.so`)

板端入口是 `src/main_board.c` (不是 `main_sdl.c`):

```c
k230_driver_init(NULL, 0);   // 内部: lv_init + 标准 DRM 后端 + evdev 触摸
ui_init();                   // EEZ 建屏
lv_sysmon_hide_performance(lv_display_get_default());  // 关掉 SDK 自带的 "30 FPS | CPU" 角标
app_init(getenv("K230UI_SOCK"));
while (1) { lv_timer_handler(); usleep(...); }
```

两条板上实测过的坑:

1. **DRM master 只能有一个** —— `k230-screen` (drmplay) 在跑时 `k230ui` 会报
   `drmModeAtomicCommit failed: Permission denied (13)` 且屏全黑; 一定先 `k230-ui-mode.sh ui` 让出屏。
2. **性能角标** —— 板端 SDK 的 `lv_conf.h` 里 `LV_USE_PERF_MONITOR=1`, 开关编进了 `liblvgl.so`,
   改 `lv_conf_board.h` 没用; 只能在运行时 `lv_sysmon_hide_performance()` (weak 引用, 模拟器无此符号时自动跳过)。

## 5. 文件清单

| 文件 | 作用 |
|---|---|
| `lv_conf.h` | **本机模拟器**配置 (SDL2, 32bpp, Montserrat 16/20/24/28) |
| `lv_conf_board.h` | **板端**配置副本 (必须与 `liblvgl.so` 一致, 见文件头说明) |
| `CMakeLists.txt` | 模拟器构建 (LVGL 用官方 cmake, 传我们的 `lv_conf.h`) |
| `src/main_sdl.c` | 本机入口 (SDL 窗口 + 鼠标) |
| `src/main_board.c` | 板端入口 (DRM + evdev) |
| `src/ui_actions.c` | 5 个 EEZ action 的实现 + 500ms 状态轮询 |
| `src/backend_client.c/.h` | Unix socket 客户端 (后台线程, 不阻塞 LVGL) |
| `tools/make-eez-project.py` | 生成 `.eez-project` (只改 LAYOUT 区即可) |
| `tools/board-build.sh` | 交叉编译 |
| `scripts/fetch-lvgl.sh` / `build-sim.sh` / `run-sim.sh` | 模拟器三件套 |
