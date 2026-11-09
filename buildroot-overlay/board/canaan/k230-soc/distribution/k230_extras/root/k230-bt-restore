#!/bin/sh
# k230-bt-golden.sh — 蓝牙反控「基线版」的冻结 / 比对 / 一键还原
#
# 为什么要它 (2026-11-05 主人要求):
#   蓝牙这套是"多文件咬合"的 —— 守护进程 (python) + 两个 unit + .path 触发
#   + bluetoothd 的 --noplugin=input + main.conf + modprobe 去 USB 自动挂起
#   + 主界面脚本里"注册成功才放开可发现"的时序。改任何一个都可能落到
#   "手机连不上 / 连上了但指针不动", 而现场排查一次要半小时。
#   所以把这 13 个文件连 md5 一起冻成基线, 一条命令回到"确认能用"的那一版。
#
# 用法 (板上, root):
#   sh k230-bt-golden.sh snapshot [DIR]   # 用当前现场造基线 (默认 /root/k230-bt-golden-<时间>)
#   sh k230-bt-golden.sh check   [DIR]    # 只比对, 不动现场 (退出码 0 = 与基线一致)
#   sh k230-bt-golden.sh restore [DIR]    # 还原 (默认最新基线); 板上 /root/k230-bt-restore 就是它
#   sh k230-bt-golden.sh list             # 列出所有基线
#
# 还原做什么 (顺序有讲究):
#   ① 先校验基线自身 MANIFEST (基线坏了就不用它覆盖现场, 直接报错退出)
#   ② 把现场当前这 13 件备份到 /root/k230-bt-rollback-<时间> (还原本身也能回退)
#   ③ 停 k230-bt-hid.path + k230-bt-hid (停 .path 才不会被自动拉起)
#   ④ 覆盖文件 → daemon-reload; main.conf 有变才重启 bluetoothd
#   ⑤ 断开 BlueZ 里标着 Connected 的僵尸链路 (带 5 s 超时, 绝不无限等)
#   ⑥ 当前模式在 BT_ON_MODES 里才启动, 并等 `注册=是` (≤60 s)
#   ⑦ 逐件比对 md5 + 打印服务/适配器/反控状态
set -u

GLOOT=/root
PREFIX=k230-bt-golden-
FIFO=/run/k230-bt-hid.fifo
STATUS=/run/k230-bt-hid.status
MODESTATE=/run/k230-ui-mode.state

# 基线范围: 板端路径 : 基线内相对路径
# ⚠ 不含 /var/lib/bluetooth (配对库) —— 那是"哪台手机配过"的现场状态, 不属于代码基线;
#   还原不碰它, 所以已配对的手机不用重新配对。
FILES='
/usr/bin/k230-bt-hid:bin/k230-bt-hid
/usr/bin/k230-bt-hid-logic-test:bin/k230-bt-hid-logic-test
/usr/bin/k230-bt-hid-abs-test.py:bin/k230-bt-hid-abs-test.py
/usr/bin/k230-bt-hid-int-test.py:bin/k230-bt-hid-int-test.py
/usr/bin/k230-bt-hid-tp-test.py:bin/k230-bt-hid-tp-test.py
/usr/bin/k230-bt-agent:bin/k230-bt-agent
/usr/bin/k230-bt-watch:bin/k230-bt-watch
/usr/bin/k230-ui-mode.sh:bin/k230-ui-mode.sh
/etc/systemd/system/k230-bt-hid.service:unit/k230-bt-hid.service
/etc/systemd/system/k230-bt-hid.path:unit/k230-bt-hid.path
/etc/systemd/system/k230-bt-agent.service:unit/k230-bt-agent.service
/etc/systemd/system/bluetooth.service.d/override.conf:unit/bluetooth.service.d/override.conf
/etc/bluetooth/main.conf:conf/bluetooth/main.conf
/etc/modprobe.d/90-k230-btusb.conf:conf/modprobe.d/90-k230-btusb.conf
'

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
    say "✓ 基线就绪: $(wc -l < "$D/MANIFEST.md") 件"
    say "  还原: sh $0 restore $D"
}

# ---------- check ----------
do_check() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 比对现场 vs $D =="
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线自身 MANIFEST 校验失败 (基线文件被改过/丢了)"
    bad=0
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
        b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
        if [ -z "$a" ]; then
            echo "  ✗ MISSING $src"
        elif [ "$a" = "$b" ]; then
            echo "  ✓ $rel"
        else
            echo "  ✗ DRIFT   $rel  (现场 ${a%????????????????????}… ≠ 基线 ${b%????????????????????}…)"
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

# ---------- restore ----------
bt_on_modes() {
    m=$(sed -n 's/^BT_ON_MODES=\${BT_ON_MODES:-"\(.*\)"}$/\1/p' /usr/bin/k230-ui-mode.sh 2>/dev/null | head -1)
    [ -n "$m" ] || m="airplay carplay"
    echo "$m"
}

do_restore() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 还原到基线 $D =="
    say "-- ① 校验基线自身"
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线 MANIFEST 校验失败 —— 基线已损坏, 拒绝用它覆盖现场"
    say "   ✓ $(wc -l < "$D/MANIFEST.md") 件全 OK"

    say "-- ② 备份现场 (可回退)"
    RB="$GLOOT/k230-bt-rollback-$(date +%Y%m%d-%H%M%S)"
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

    say "-- ③ 停服务"
    systemctl stop k230-bt-hid.path 2>/dev/null || true
    systemctl stop k230-bt-hid 2>/dev/null || true
    sleep 1

    say "-- ④ 覆盖文件"
    old_main=$(md5sum /etc/bluetooth/main.conf 2>/dev/null | awk '{print $1}')
    echo "$FILES" | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        [ -e "$D/$rel" ] || { echo "   ⚠ 基线里没这件: $rel"; continue; }
        mkdir -p "$(dirname "$src")"
        cp -a "$D/$rel" "$src" || echo "   ✗ 写入失败: $src"
    done
    systemctl daemon-reload 2>/dev/null || true
    new_main=$(md5sum /etc/bluetooth/main.conf 2>/dev/null | awk '{print $1}')
    if [ "$old_main" != "$new_main" ]; then
        say "   main.conf 变了 → 重启 bluetoothd"
        timeout 30 systemctl restart bluetooth 2>/dev/null || say "   ⚠ bluetoothd 重启超时"
        sleep 3
    fi

    say "-- ⑤ 断开僵尸链路 (每台 5 s 上限)"
    bluetoothctl devices 2>/dev/null | sed -n 's/^Device \([0-9A-F:]*\).*/\1/p' | while read -r mac; do
        con=$(timeout 5 bluetoothctl info "$mac" 2>/dev/null | sed -n 's/.*Connected: \(yes\).*/\1/p')
        if [ "$con" = "yes" ]; then
            say "   断开 $mac"
            timeout 5 bluetoothctl disconnect "$mac" >/dev/null 2>&1 || true
        fi
    done

    say "-- ⑥ 启动 (按当前模式)"
    mode=$(cat "$MODESTATE" 2>/dev/null || echo ui)
    if echo " $(bt_on_modes) " | grep -q " $mode "; then
        systemctl start k230-bt-hid.path 2>/dev/null || true
        systemctl start k230-bt-hid 2>/dev/null || true
        i=0
        while [ "$i" -lt 60 ]; do
            grep -q '注册=是' "$STATUS" 2>/dev/null && break
            sleep 1; i=$((i + 1))
        done
        say "   等 HID 注册: ${i}s"
    else
        say "   当前 mode=$mode 不开蓝牙 (设计如此); 切 airplay 后自动生效"
    fi

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
    echo "   服务: hid=$(systemctl is-active k230-bt-hid) path=$(systemctl is-active k230-bt-hid.path) agent=$(systemctl is-active k230-bt-agent) bluetooth=$(systemctl is-active bluetooth)"
    echo "   适配器: $(bluetoothctl show 2>/dev/null | sed -n 's/^\s*Alias: //p' | head -1) / $(bluetoothctl show 2>/dev/null | grep -cE 'Powered: yes') powered / discoverable=$(bluetoothctl show 2>/dev/null | sed -n 's/^\s*Discoverable: //p' | head -1)"
    echo "   反控: $(cat "$STATUS" 2>/dev/null || echo '(无状态文件)')"

    if grep -q '注册=是' "$STATUS" 2>/dev/null || ! echo " $(bt_on_modes) " | grep -q " $mode "; then
        say "✓ 还原完成。现场回退: sh $0 restore $RB"
        exit 0
    fi
    say "✗ 还原后 HID 未注册 —— 检查 journalctl -u k230-bt-hid -n 40"
    exit 1
}

# ---------- main ----------
case "${1:-}" in
    snapshot) shift; do_snapshot "${1:-}" ;;
    check)    shift; do_check "${1:-}" ;;
    restore)  shift; do_restore "${1:-}" ;;
    list)
        say "== 基线 ($GLOOT/$PREFIX*) =="
        for d in $(ls -d "$GLOOT/$PREFIX"* 2>/dev/null | sort); do
            say "  $d  ($(wc -l < "$d/MANIFEST.md" 2>/dev/null || echo ?) 件, $(stat -c %y "$d" 2>/dev/null | cut -d. -f1))"
        done
        ;;
    ""|--restore)
        # 板上 /root/k230-bt-restore 的默认行为就是还原
        do_restore "" ;;
    -h|--help)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        die "未知动作: $1 (snapshot|check|restore|list)" ;;
esac
