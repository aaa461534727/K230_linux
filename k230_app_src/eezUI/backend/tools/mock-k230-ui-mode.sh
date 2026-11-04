#!/bin/sh
# mock-k230-ui-mode.sh —— 本机 (PC) 联调用的**假**模式脚本
#
# 干什么: 顶替板上的 /usr/bin/k230-ui-mode.sh, 不碰 systemd, 只 sleep 一下
#         表示"切模式要时间", 并把当前模式记到文件里, 供 probe 用。
# 怎么用: K230UI_MODE_CMD=... 或 backend 启动参数 --mode-cmd 指过来
#         k230-ui-backend --socket /tmp/k230-ui.sock --mode-cmd backend/tools/mock-k230-ui-mode.sh
# 依赖  : 纯 sh
# 在哪跑: 本机 (Ubuntu) 只用于开发联调; 板端用真的 k230-ui-mode.sh
set -u
STATE=${K230UI_MOCK_STATE:-/tmp/k230-ui-mock.state}

sleep "${K230UI_MOCK_DELAY:-0.6}"

case "${1:-}" in
    select)
        [ $# -ge 2 ] || { echo "ERROR: select needs a service"; exit 1; }
        case "$2" in
            miracast|airplay|carlife|carplay) ;;
            *) echo "ERROR: unknown service $2"; exit 2 ;;
        esac
        echo "$2" > "$STATE"
        echo "OK mode=$2"
        ;;
    ui|stop)
        echo ui > "$STATE"
        echo "OK mode=ui"
        ;;
    auto)
        echo auto > "$STATE"
        echo "OK mode=auto"
        ;;
    probe)
        # 模拟"手机一直连着" —— 除非把状态文件删了
        [ -f "$STATE" ] || exit 1
        [ "$(cat "$STATE" 2>/dev/null)" = "${2:-}" ]
        ;;
    status)
        [ -f "$STATE" ] && cat "$STATE" || echo unknown
        ;;
    *)
        echo "usage: $0 {ui|select <svc>|stop|auto|probe <svc>|status}"
        exit 1
        ;;
esac
