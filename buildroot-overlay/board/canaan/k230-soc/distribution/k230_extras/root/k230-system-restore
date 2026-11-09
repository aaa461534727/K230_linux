#!/bin/sh
# k230-system-golden.sh — 「整机基本盘」的冻结 / 比对 / 一键还原
#
# 为什么需要它 (2026-11-06 主人定):
#   主人对"基本盘"的定义是**整机**, 不是某一路功能:
#       开机 → 主界面 (UI 上四个按钮) → 点按钮进各自功能 → 退出回主界面
#   所以基线必须覆盖"这个画面能出来、四个按钮都能点进能退出"所牵涉的**全部**件:
#     · UI 层: k230ui (LVGL 主界面) + backend + 模式总控 + /etc/k230-ui.conf
#     · 屏幕层: k230-screen (常驻 drmplay) / 音频 / 解码 / 几何发布
#     · 四路后端: Miracast / AirPlay / CarLife / CarPlay (+ USB 镜像)
#     · 仲裁与共享热点: k230-source-arbiter / k230-shared-ap
#     · 蓝牙反控: k230-bt-hid 全家 (AirPlay 下手机反控靠它)
#   任何一件被改动都可能落成"开机黑屏 / 点按钮没反应 / 退不回来", 而现场排查一次很久。
#   冻成基线后, 一条命令回到主人点头的那一版。
#
# 与 k230-bt-golden.sh 的关系:
#   · 那份 (14 件) 只管**蓝牙反控**, 还原更快、影响面更小 ⇒ 只坏蓝牙时用它。
#   · 本份 (62 件) 是它的**超集**: 整机回退用本份即可, 不必再跑 bt-restore。
#   · 两份都含 k230-ui-mode.sh / 蓝牙那 13 件, 内容必须一致 (同一次 snapshot 拍的就不会漂)。
#   · 本份**故意不含** /var/lib/bluetooth (配对库) 与 /root/wfd-p2p-persistent.conf
#     (P2P 上次信道缓存) —— 那是现场状态, 不是代码; 还原不碰它们, 所以不用重配对。
#
# 用法 (板上, root):
#   sh k230-system-golden.sh snapshot [DIR]  # 用当前现场造基线 (默认 /root/k230-system-golden-<时间>)
#   sh k230-system-golden.sh check    [DIR]  # 只比对文件, 不动现场 (exit 0 = 一致)
#   sh k230-system-golden.sh restore  [DIR]  # 还原 (默认最新) + 回主界面
#   sh k230-system-golden.sh status          # 只看运行态: 模式 / 各 unit / 四个按钮
#   sh k230-system-golden.sh list            # 列出所有基线
#
# 还原做什么 (顺序有讲究):
#   ① 先校验基线自身 MANIFEST (基线坏了就不用它覆盖现场, 直接报错退出)
#   ② 把现场这 62 件备份到 /root/k230-system-rollback-<时间> (还原本身也能回退)
#   ③ 停掉所有"抢屏"的 unit (先停投屏端再停 UI; DRM 只有一个 master)
#   ④ 覆盖文件 → daemon-reload; 只有真变了才重启 backend / 共享热点
#   ⑤ k230-ui-mode.sh ui → 回主界面 (这一步才是"开机画面"的判据)
#   ⑥ 等 k230-ui active + backend.sock 出现 (≤30 s)
#   ⑦ 逐件比对 md5 + 打印运行态
set -u

GLOOT=/root
PREFIX=k230-system-golden-
MODESTATE=/run/k230-ui-mode.state
UISOCK=/run/k230-ui/backend.sock
MODE_BIN=/usr/bin/k230-ui-mode.sh

# 基线范围: 板端路径 : 基线内相对路径
FILES='
/usr/bin/k230ui:bin/k230ui
/usr/bin/k230-ui-backend:bin/k230-ui-backend
/usr/bin/k230ui-ctl:bin/k230ui-ctl
/usr/bin/k230-ui-mode.sh:bin/k230-ui-mode.sh
/etc/k230-ui.conf:etc/k230-ui.conf
/etc/systemd/system/k230-ui.service:unit/k230-ui.service
/etc/systemd/system/k230-ui-backend.service:unit/k230-ui-backend.service
/root/k230-screen.sh:root/k230-screen.sh
/root/k230-audio.sh:root/k230-audio.sh
/usr/bin/drmplay:bin/drmplay
/usr/bin/wfd-sink:bin/wfd-sink
/usr/bin/wfd-autotune:bin/wfd-autotune
/usr/bin/v4l2_dec:bin/v4l2_dec
/etc/systemd/system/k230-screen.service:unit/k230-screen.service
/etc/systemd/system/k230-screen.service.d/pipe.conf:unit/k230-screen.service.d/pipe.conf
/etc/systemd/system/k230-screen.service.d/shm.conf:unit/k230-screen.service.d/shm.conf
/etc/systemd/system/k230-audio.service:unit/k230-audio.service
/root/wfd-p2p.sh:root/wfd-p2p.sh
/etc/systemd/system/k230-miracast.service:unit/k230-miracast.service
/etc/systemd/system/k230-miracast.service.d/declat.conf:unit/k230-miracast.service.d/declat.conf
/etc/systemd/system/k230-miracast.service.d/forcech.conf:unit/k230-miracast.service.d/forcech.conf
/etc/systemd/system/k230-miracast.service.d/persistent.conf:unit/k230-miracast.service.d/persistent.conf
/etc/systemd/system/k230-miracast.service.d/shm.conf:unit/k230-miracast.service.d/shm.conf
/etc/systemd/system/k230-miracast-go.service:unit/k230-miracast-go.service
/root/k230-source-arbiter.sh:root/k230-source-arbiter.sh
/etc/systemd/system/k230-source-arbiter.service:unit/k230-source-arbiter.service
/etc/k230-arbiter.conf:etc/k230-arbiter.conf
/root/k230-airplay:root/k230-airplay
/usr/local/bin/uxplay:bin/uxplay
/etc/systemd/system/k230-airplay.service:unit/k230-airplay.service
/etc/systemd/system/k230-airplay.service.d/carplay-mode.conf:unit/k230-airplay.service.d/carplay-mode.conf
/etc/systemd/system/k230-airplay.service.d/zz-debug.conf:unit/k230-airplay.service.d/zz-debug.conf
/root/carlife/carlife_sink:root/carlife/carlife_sink
/root/carlife_sink:root/carlife_sink
/root/carlife/carlife-ap.sh:root/carlife/carlife-ap.sh
/etc/systemd/system/k230-carlife.service:unit/k230-carlife.service
/etc/systemd/system/k230-carlife-wifi.service:unit/k230-carlife-wifi.service
/etc/systemd/system/k230-shared-ap.service:unit/k230-shared-ap.service
/usr/local/bin/carplay-arbiter:bin/carplay-arbiter
/usr/local/bin/carplay-airplayd:bin/carplay-airplayd
/usr/local/bin/carplay-wireless:bin/carplay-wireless
/usr/local/bin/carplay-hal:bin/carplay-hal
/usr/local/bin/carplay-media:bin/carplay-media
/etc/systemd/system/carplay-arbiter.service:unit/carplay-arbiter.service
/etc/systemd/system/carplay-airplayd.service:unit/carplay-airplayd.service
/etc/systemd/system/carplay-wireless.service:unit/carplay-wireless.service
/root/usbmirror/usb_mirror:root/usbmirror/usb_mirror
/root/usbmirror/run.sh:root/usbmirror/run.sh
/etc/systemd/system/k230-usbmirror.service:unit/k230-usbmirror.service
/usr/bin/k230-bt-hid:bt/bin/k230-bt-hid
/usr/bin/k230-bt-hid-logic-test:bt/bin/k230-bt-hid-logic-test
/usr/bin/k230-bt-hid-abs-test.py:bt/bin/k230-bt-hid-abs-test.py
/usr/bin/k230-bt-hid-int-test.py:bt/bin/k230-bt-hid-int-test.py
/usr/bin/k230-bt-hid-tp-test.py:bt/bin/k230-bt-hid-tp-test.py
/usr/bin/k230-bt-agent:bt/bin/k230-bt-agent
/usr/bin/k230-bt-watch:bt/bin/k230-bt-watch
/etc/systemd/system/k230-bt-hid.service:bt/unit/k230-bt-hid.service
/etc/systemd/system/k230-bt-hid.path:bt/unit/k230-bt-hid.path
/etc/systemd/system/k230-bt-agent.service:bt/unit/k230-bt-agent.service
/etc/systemd/system/bluetooth.service.d/override.conf:bt/unit/bluetooth.service.d/override.conf
/etc/bluetooth/main.conf:bt/conf/bluetooth/main.conf
/etc/modprobe.d/90-k230-btusb.conf:bt/conf/modprobe.d/90-k230-btusb.conf
'
# ⚠ 最后 13 件与 k230-bt-golden.sh 的 14 件重叠 13 件 (它多一件 bin 路径不同的同名件);
#   两处内容必须一致 —— 整机基线是蓝牙基线的**超集**, 只管蓝牙时用 bt-restore 更轻。

# 还原时要停的"抢屏/抢资源" unit (先投屏端后 UI, 最后蓝牙)
STOP_UNITS="k230-miracast k230-miracast-go k230-airplay k230-carlife k230-carlife-wifi \
k230-usbmirror k230-source-arbiter carplay-arbiter carplay-airplayd carplay-wireless \
k230-screen k230-ui k230-bt-hid.path k230-bt-hid k230-bt-agent"

say() { echo "$@"; }
die() { echo "✗ $*" >&2; exit 1; }

latest() { ls -d "$GLOOT/$PREFIX"* 2>/dev/null | sort | tail -1; }

pick_dir() {  # $1 = 显式指定或用最新
    if [ -n "${1:-}" ]; then D="$1"; else D=$(latest); fi
    [ -n "$D" ] && [ -d "$D" ] || die "没有基线目录 (先 snapshot)"
    echo "$D"
}

# ---------- snapshot ----------
do_snapshot() {
    D="${1:-$GLOOT/$PREFIX$(date +%Y%m%d-%H%M)}"
    say "== 造基线 $D =="
    mkdir -p "$D" || die "建目录失败"
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        if [ ! -e "$src" ]; then
            echo "  ⚠ 缺件 (跳过): $src"
            continue
        fi
        mkdir -p "$D/$(dirname "$rel")"
        cp -a "$src" "$D/$rel" || echo "  ✗ 拷贝失败: $src"
        printf '  %s\n' "$rel"
    done
    ( cd "$D" && find . -type f ! -name MANIFEST.md | sed 's|^\./||' | sort \
        | xargs md5sum > MANIFEST.md ) || die "算 MANIFEST 失败"
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) || die "基线自检失败"
    say "✓ 基线就绪: $(wc -l < "$D/MANIFEST.md") 件, 总体积 $(du -sh "$D" | awk '{print $1}')"
    say "  还原: sh $0 restore $D"
}

# ---------- check ----------
do_check() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 比对现场 vs $D =="
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线自身 MANIFEST 校验失败 (基线文件被改过/丢了)"
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
        b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
        if [ -z "$a" ]; then
            echo "  ✗ MISSING $src"
        elif [ "$a" = "$b" ]; then
            echo "  ✓ $rel"
        else
            echo "  ✗ DRIFT   $rel"
        fi
    done
    ok=$(echo "$FILES" | sed '/^$/d' | while IFS=: read -r src rel; do
            a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
            b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
            [ -n "$a" ] && [ "$a" = "$b" ] && echo x
         done | wc -l)
    n=$(echo "$FILES" | sed '/^$/d' | wc -l)
    if [ "$ok" -eq "$n" ]; then
        say "✓ 与基线完全一致 ($ok/$n)"
        exit 0
    fi
    say "✗ 有漂移 ($ok/$n 一致) —— 要回到基线: sh $0 restore"
    exit 2
}

# ---------- status ----------
do_status() {
    echo "== 运行态 =="
    echo "  模式: $(cat "$MODESTATE" 2>/dev/null || echo '(无 state 文件)')"
    echo "  主界面: k230-ui=$(systemctl is-active k230-ui 2>/dev/null) backend=$(systemctl is-active k230-ui-backend 2>/dev/null) sock=$([ -S "$UISOCK" ] && echo 有 || echo 无)"
    echo "  屏幕: screen=$(systemctl is-active k230-screen 2>/dev/null) audio=$(systemctl is-active k230-audio 2>/dev/null)"
    echo "  热点: shared-ap=$(systemctl is-active k230-shared-ap 2>/dev/null)"
    for u in k230-miracast k230-airplay k230-carlife-wifi carplay-arbiter carplay-airplayd carplay-wireless; do
        printf '  %-20s %s\n' "$u" "$(systemctl is-active "$u" 2>/dev/null)"
    done
    echo "== 四个按钮 (/etc/k230-ui.conf) =="
    for s in miracast airplay carlife carplay; do
        v=$(sed -n "s/^UNITS_$s=\"\(.*\)\"\$/\1/p" /etc/k230-ui.conf 2>/dev/null | head -1)
        printf '  %-10s -> %s\n' "$s" "${v:-（未定义）}"
    done
}

# ---------- restore ----------
do_restore() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 还原到基线 $D =="
    say "-- ① 校验基线自身"
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线 MANIFEST 校验失败 —— 基线已损坏, 拒绝用它覆盖现场"
    say "   ✓ $(wc -l < "$D/MANIFEST.md") 件全 OK"

    say "-- ② 备份现场 (可回退)"
    RB="$GLOOT/k230-system-rollback-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RB"
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        [ -e "$src" ] || continue
        mkdir -p "$RB/$(dirname "$rel")"
        cp -a "$src" "$RB/$rel"
    done
    ( cd "$RB" && find . -type f ! -name MANIFEST.md | sed 's|^\./||' | sort \
        | xargs md5sum > MANIFEST.md ) 2>/dev/null
    say "   → $RB"

    say "-- ③ 停抢屏 unit"
    for u in $STOP_UNITS; do
        systemctl stop "$u" 2>/dev/null || true
    done
    sleep 2

    say "-- ④ 覆盖文件"
    CH="$RB/.changed"
    : > "$CH"
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        [ -e "$D/$rel" ] || { echo "   ⚠ 基线里没这件: $rel"; continue; }
        old=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
        new=$(md5sum "$D/$rel" 2>/dev/null | awk '{print $1}')
        [ "$old" = "$new" ] && continue
        mkdir -p "$(dirname "$src")"
        cp -a "$D/$rel" "$src" || echo "   ✗ 写入失败: $src"
        echo "   ~ $rel" >> "$CH"
    done
    if [ -s "$CH" ]; then
        say "   改动 $(wc -l < "$CH") 件:"
        sed 's/^/   /' "$CH"
    else
        say "   无改动 (现场已与基线一致)"
    fi
    systemctl daemon-reload 2>/dev/null || true

    # 只有真变了才动常驻服务 (重启 backend 会短暂断 UI socket, 重启热点会掉 AP)
    if grep -qE 'bin/k230-ui-backend$|unit/k230-ui-backend.service$' "$CH"; then
        say "   backend 变了 → 重启 k230-ui-backend"
        systemctl restart k230-ui-backend 2>/dev/null || say "   ⚠ backend 重启失败"
    fi
    if grep -qE 'carlife-ap.sh$|unit/k230-shared-ap.service$' "$CH"; then
        say "   热点逻辑变了 → 重启 k230-shared-ap"
        systemctl restart k230-shared-ap 2>/dev/null || say "   ⚠ shared-ap 重启失败"
    fi
    # 蓝牙配置变了才重启 bluetoothd (重启会断已连的手机)
    if grep -qE 'bt/conf/bluetooth/main.conf$|bt/unit/bluetooth.service.d/override.conf$|bt/conf/modprobe.d/' "$CH"; then
        say "   蓝牙配置变了 → 重启 bluetooth"
        timeout 30 systemctl restart bluetooth 2>/dev/null || say "   ⚠ bluetooth 重启超时"
        sleep 2
    fi
    rm -f "$CH"

    say "-- ⑤ 回主界面"
    if [ -x "$MODE_BIN" ]; then
        "$MODE_BIN" ui 2>/dev/null || say "   ⚠ k230-ui-mode.sh ui 返回非 0"
    else
        systemctl start k230-ui 2>/dev/null || true
    fi

    say "-- ⑥ 等主界面就绪 (≤30 s)"
    i=0
    while [ "$i" -lt 30 ]; do
        systemctl is-active --quiet k230-ui 2>/dev/null && break
        sleep 1
        i=$((i + 1))
    done
    j=0
    while [ "$j" -lt 10 ]; do
        [ -S "$UISOCK" ] && break
        sleep 1
        j=$((j + 1))
    done
    say "   k230-ui 起来用了 ${i}s, backend socket ${j}s"

    say "-- ⑦ 自检"
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
        b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
        if [ "$a" = "$b" ]; then
            echo "   ✓ $rel"
        else
            echo "   ✗ DRIFT $rel"
        fi
    done
    do_status | sed 's/^/   /'

    if systemctl is-active --quiet k230-ui 2>/dev/null; then
        say "✓ 还原完成, 已在主界面。现场回退: sh $0 restore $RB"
        exit 0
    fi
    say "✗ 还原后主界面没起来 —— 查 journalctl -u k230-ui -n 40 / /var/log/k230-ui.log"
    exit 1
}

# ---------- main ----------
case "${1:-}" in
    snapshot) shift; do_snapshot "${1:-}" ;;
    check)    shift; do_check "${1:-}" ;;
    restore)  shift; do_restore "${1:-}" ;;
    status)   do_status ;;
    list)
        say "== 基线 ($GLOOT/$PREFIX*) =="
        for d in $(ls -d "$GLOOT/$PREFIX"* 2>/dev/null | sort); do
            say "  $d  ($(wc -l < "$d/MANIFEST.md" 2>/dev/null || echo ?) 件, $(du -sh "$d" 2>/dev/null | awk '{print $1}'), $(stat -c %y "$d" 2>/dev/null | cut -d. -f1))"
        done
        ;;
    ""|--restore)
        do_restore "" ;;
    -h|--help)
        sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        die "未知动作: $1 (snapshot|check|restore|status|list)" ;;
esac
