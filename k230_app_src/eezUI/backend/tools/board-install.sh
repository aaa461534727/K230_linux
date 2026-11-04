#!/usr/bin/env bash
# board-install.sh —— 把 UI + 后端装到 K230 板子上 (可重复执行, 幂等)
#
# 干什么:
#   1) scp 三个产物: k230ui (frontend/build-board) / k230-ui-backend / k230ui-ctl
#   2) scp 两个脚本: k230-ui-mode.sh / 配置文件模板
#   3) scp 两个 systemd unit, 只 enable k230-ui-backend (k230-ui 由 backend 按需起)
#   4) 停掉旧的开机仲裁器 (手动模式 = 由 UI 决定谁上屏), 起 backend
# 怎么用:
#   bash backend/tools/board-install.sh                    # 默认 root@10.0.102.101
#   BOARD_HOST=root@10.0.102.172 bash backend/tools/board-install.sh
#   KEEP_ARBITER=1 bash backend/tools/board-install.sh     # 保留仲裁器 (不推荐)
# 依赖  : 本机能 ssh 到板子 (免密或已 ssh-copy-id); 已经跑过
#         frontend/tools/board-build.sh 和 backend/ 的交叉编译
# 在哪跑: 本机 (Ubuntu)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND="$(dirname "$HERE")"
EEZUI="$(dirname "$BACKEND")"
FRONTEND="$EEZUI/frontend"

BOARD="${BOARD_HOST:-root@10.0.102.101}"

# 板子只有 ssh 密码登录时这样用: SSH="sshpass -p root ssh" SCP="sshpass -p root scp" ...
SSH_CMD="${SSH:-ssh}"
SCP_CMD="${SCP:-scp}"

# 产物优先用交叉编译目录 (build-board), 没有才回退本机目录 (模拟器联调用)
# 判据: 板子是 riscv64, 传 x86 的 build/ 过去只会 Execute format error
pick() {  # $1 = build-board 路径, $2 = build 路径
    [ -x "$1" ] && { echo "$1"; return; }
    echo "$2"
}
UI_BIN="$(pick "$FRONTEND/build-board/k230ui"         "$FRONTEND/build-sim/k230ui-sim")"
BACKEND_BIN="$(pick "$BACKEND/build-board/k230-ui-backend" "$BACKEND/build/k230-ui-backend")"
CTL_BIN="$(pick "$BACKEND/build-board/k230ui-ctl"      "$BACKEND/build/k230ui-ctl")"

for f in "$UI_BIN" "$BACKEND_BIN" "$CTL_BIN"; do
    [ -x "$f" ] || { echo "[install] 缺 $f —— 先交叉编译 (见 README)" >&2; exit 1; }
done

echo "[install] 目标板: $BOARD"
echo "[install] 上传: $(basename "$UI_BIN") (UI) / $(basename "$BACKEND_BIN") (backend) / $(basename "$CTL_BIN") (ctl)"
$SSH_CMD "$BOARD" 'mkdir -p /root/k230-ui'

# ⚠ 先传 /tmp 再 mv: 程序正在跑时直接 scp 覆盖会 "Text file busy"
echo "[install] 上传可执行文件"
$SCP_CMD "$UI_BIN"      "$BOARD:/tmp/k230ui.new"
$SCP_CMD "$BACKEND_BIN" "$BOARD:/tmp/k230-ui-backend.new"
$SCP_CMD "$CTL_BIN"     "$BOARD:/tmp/k230ui-ctl.new"
$SCP_CMD "$BACKEND/k230-ui-mode.sh" "$BOARD:/tmp/k230-ui-mode.sh.new"
$SSH_CMD "$BOARD" 'mv /tmp/k230ui.new /usr/bin/k230ui
                   mv /tmp/k230-ui-backend.new /usr/bin/k230-ui-backend
                   mv /tmp/k230ui-ctl.new /usr/bin/k230ui-ctl
                   mv /tmp/k230-ui-mode.sh.new /usr/bin/k230-ui-mode.sh'

echo "[install] 上传 unit + 配置模板 + 文档"
$SCP_CMD "$BACKEND/systemd/k230-ui.service"         "$BOARD:/etc/systemd/system/"
$SCP_CMD "$BACKEND/systemd/k230-ui-backend.service" "$BOARD:/etc/systemd/system/"
[ -f "$BACKEND/k230-ui.conf.example" ] && \
    $SCP_CMD "$BACKEND/k230-ui.conf.example" "$BOARD:/etc/k230-ui.conf" || true
[ -f "$EEZUI/README.md" ] && $SCP_CMD "$EEZUI/README.md" "$BOARD:/root/k230-ui/README.md" || true

echo "[install] 设置权限 + 重载 systemd"
$SSH_CMD "$BOARD" 'chmod 0755 /usr/bin/k230ui /usr/bin/k230-ui-backend /usr/bin/k230ui-ctl /usr/bin/k230-ui-mode.sh
                   systemctl daemon-reload'

if [ "${KEEP_ARBITER:-0}" != "1" ]; then
    echo "[install] 停用开机仲裁器 (改由 UI 决定哪个服务上屏)"
    $SSH_CMD "$BOARD" 'systemctl disable --now k230-source-arbiter.service 2>/dev/null || true
                       systemctl disable --now k230-screen.service 2>/dev/null || true'
fi

echo "[install] 起 backend (它会自己把主界面拉起来)"
$SSH_CMD "$BOARD" 'systemctl enable k230-ui-backend.service
              systemctl restart k230-ui-backend.service
              sleep 3
              systemctl --no-pager --lines=15 status k230-ui-backend.service || true
              echo "--- 当前模式:"; cat /run/k230-ui-mode.state 2>/dev/null || echo unknown
              echo "--- 后端自检:"; /usr/bin/k230ui-ctl STATUS || true'

echo "[install] OK. 想看投屏单元状态: ssh $BOARD k230ui-ctl SELECT miracast"
