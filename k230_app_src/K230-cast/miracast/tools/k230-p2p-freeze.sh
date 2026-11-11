#!/bin/sh
# k230-p2p-freeze.sh — 投屏/P2P 栈「改形态前」的现状冻结 / 比对 / 一键还原
#
# 为什么要它 (2026-11-09 主人: "改配置前先冻结现状备份"):
#   下一步要把 P2P 从 client 形态改成海奇那种 GO 形态 (go_intent / 信道列表 /
#   持久组 / 驱动 group_capab 一起动), 而现场这套是 wfd-p2p.sh + wfd-pbc-watcher.sh
#   + systemd unit + drop-in + /tmp/wfd-p2p.conf + wpa_supplicant + 8189fs.ko
#   "多件咬合"的, 只改一件看不出是谁的锅。所以先把现状连 md5 冻成一份,
#   任何一轮实验后都能一条命令回到"改之前"。
#
#   注意: 现状 ≠ 旧 golden。板上 wfd-p2p.sh (11-09 14:29) / wfd-pbc-watcher.sh
#   (11-09 16:11) 都晚于 10-29 黄金版, 且 drop-in 多了 zz-cea-test.conf;
#   拿旧 golden 还原回来的是"另一套", 不能当本轮实验的基线。
#
# 用法 (板上 root):
#   sh k230-p2p-freeze.sh snapshot [DIR]   # 用当前现场造基线 (默认 /root/k230-p2p-freeze-<时间>)
#   sh k230-p2p-freeze.sh check   [DIR]    # 只比对, 不动现场 (0 = 一致)
#   sh k230-p2p-freeze.sh restore [DIR]    # 还原 (默认最新基线)
#   sh k230-p2p-freeze.sh list             # 列出所有基线
set -u

GLOOT=/root
PREFIX=k230-p2p-freeze-
SVC=k230-miracast

say() { echo "$@"; }
die() { echo "✗ $*" >&2; exit 1; }

# 冻结范围: 板端路径:基线内相对路径
# 二进制 (wpa_supplicant / 8189fs.ko / 8821cu.ko) 也一起冻 —— 形态实验要换它们,
# 没有原件就回不去; 但本机仓库只收文本件, 二进制留板上 (见 captures README)。
file_list() {
    cat <<'EOF'
/root/wfd-p2p.sh:root/wfd-p2p.sh
/root/wfd-pbc-watcher.sh:root/wfd-pbc-watcher.sh
/root/k230-cast:root/k230-cast
/root/k230-screen.sh:root/k230-screen.sh
/root/k230-audio.sh:root/k230-audio.sh
/root/8821cu.ko:root/8821cu.ko
/root/wifi8821:root/wifi8821
/etc/systemd/system/k230-miracast.service:unit/k230-miracast.service
/usr/sbin/wpa_supplicant:bin/wpa_supplicant
/lib/modules/6.6.36/updates/8189fs.ko:bin/8189fs.ko
/tmp/wfd-p2p.conf:tmp/wfd-p2p.conf
EOF
    for f in /etc/systemd/system/k230-miracast.service.d/*.conf; do
        [ -e "$f" ] || continue
        printf '%s:unit/k230-miracast.service.d/%s\n' "$f" "$(basename "$f")"
    done
}

latest() { ls -d "$GLOOT/$PREFIX"* 2>/dev/null | sort | tail -1; }

pick_dir() {
    if [ -n "${1:-}" ]; then D="$1"; else D=$(latest); fi
    [ -n "$D" ] && [ -d "$D" ] || die "没有基线目录 (先 snapshot)"
    echo "$D"
}

# ---------- snapshot ----------
do_snapshot() {
    D="${1:-$GLOOT/$PREFIX$(date +%Y%m%d-%H%M)}"
    say "== 冻结现状 → $D =="
    mkdir -p "$D" || die "建目录失败"
    file_list | sed '/^$/d' | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        if [ ! -e "$src" ]; then echo "  ⚠ 缺件 (跳过): $src"; continue; fi
        mkdir -p "$D/$(dirname "$rel")"
        cp -a "$src" "$D/$rel" || echo "  ✗ 拷贝失败: $src"
        printf '  %s\n' "$rel"
    done

    mkdir -p "$D/state"
    { echo "# $(date -u '+%F %T UTC')"; uname -a; uptime; echo;
      for u in k230-screen k230-audio k230-miracast k230-airplay k230-cast; do
          printf '%-16s %s\n' "$u" "$(systemctl is-active "$u" 2>/dev/null)"
      done; } > "$D/state/runtime.txt" 2>&1
    systemctl show "$SVC" -p FragmentPath -p Environment -p ExecStart > "$D/state/service.txt" 2>&1
    { iw dev 2>&1; echo; iw dev wlan0 info 2>&1; echo; iw phy 2>&1 | sed -n '1,80p'; } > "$D/state/iw.txt"
    ip -4 -br addr > "$D/state/net.txt" 2>&1
    { echo '--- modules'; grep -E '^(8189fs|8821cu|cfg80211)' /proc/modules 2>/dev/null;
      echo '--- 8189fs params'; ls /sys/module/8189fs/parameters 2>/dev/null | while read -r p; do
          printf '%s=%s\n' "$p" "$(cat "/sys/module/8189fs/parameters/$p" 2>/dev/null)"; done; } > "$D/state/modules.txt" 2>&1
    { echo '--- wfd-p2p.sh'; grep -nE 'p2p_go_intent|PERSIST_RECONNECT|persistent_reconnect|p2p_oper_(reg_class|channel)|P2P_OPER_CH|WFD_FORCE_OPER_CH' /root/wfd-p2p.sh 2>/dev/null;
      echo '--- /tmp/wfd-p2p.conf'; cat /tmp/wfd-p2p.conf 2>/dev/null;
      echo '--- unit + drop-in'; cat /etc/systemd/system/k230-miracast.service /etc/systemd/system/k230-miracast.service.d/*.conf 2>/dev/null; } > "$D/state/config.txt" 2>&1
    { echo '--- procs'; pgrep -a -f 'wfd-sink|wfd-p2p|v4l2_dec|drmplay|wpa_supplicant|hostapd' 2>/dev/null;
      echo '--- listen'; ss -tlnp 2>/dev/null | grep -E ':7236|:7001' || true; } > "$D/state/procs.txt" 2>&1
    { echo '--- miracast.log tail'; tail -5 /var/log/miracast.log 2>/dev/null;
      echo '--- sizes'; ls -l /var/log/miracast.log /var/log/miracast-dec.log /tmp/wfd-p2p.log 2>&1; } > "$D/state/logs.txt" 2>&1

    ( cd "$D" && find . -type f ! -name MANIFEST.md ! -path './state/*' \
        | sed 's|^\./||' | sort | xargs md5sum > MANIFEST.md ) || die "算 MANIFEST 失败"
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) || die "基线自检失败"
    say "✓ 冻结完成: $(wc -l < "$D/MANIFEST.md") 件 + $(ls "$D/state" | wc -l) 份运行时快照"
    say "  还原: sh $0 restore $D"
    say "  比对: sh $0 check $D"
}

# ---------- check ----------
do_check() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 比对现场 vs $D =="
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线自身校验失败 (基线文件被改过/丢了)"
    file_list | sed '/^$/d' | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
        b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
        if [ -z "$a" ]; then echo "  ✗ MISSING $src"
        elif [ "$a" = "$b" ]; then echo "  ✓ $rel"
        else echo "  ✗ DRIFT   $rel"; fi
    done
    n=$(file_list | sed '/^$/d' | wc -l)
    ok=$(file_list | sed '/^$/d' | while IFS=: read -r src rel; do
            [ -n "$src" ] || continue
            a=$(md5sum "$src" 2>/dev/null | awk '{print $1}')
            b=$(awk -v r="$rel" '$2==r{print $1}' "$D/MANIFEST.md")
            [ -n "$a" ] && [ "$a" = "$b" ] && echo x
         done | wc -l)
    # 反向检查: 基线里有、现场集合里没有的 (例如 drop-in 被删) 也要报
    MAP=$(mktemp); EXTRA=$(mktemp)
    file_list | sed '/^$/d' | awk -F: '{print $2" "$1}' > "$MAP"
    awk '{print $2}' "$D/MANIFEST.md" | while read -r rel; do
        src=$(awk -v r="$rel" '$1==r{print $2}' "$MAP")
        if [ -z "$src" ]; then echo "  ✗ GONE    $rel"; echo x >> "$EXTRA"
        elif [ ! -e "$src" ]; then echo "  ✗ MISSING $src"; echo x >> "$EXTRA"; fi
    done
    rm -f "$MAP"
    if [ -s "$EXTRA" ]; then ok=$((ok - $(wc -l < "$EXTRA"))); fi
    rm -f "$EXTRA"
    if [ "$ok" -eq "$n" ]; then say "✓ 与冻结基线完全一致 ($ok/$n)"; exit 0; fi
    say "✗ 有漂移 ($ok/$n 一致) —— 回到现状: sh $0 restore"
    exit 2
}

# ---------- restore ----------
do_restore() {
    D=$(pick_dir "${1:-}") || exit 1
    say "== 还原到 $D =="
    ( cd "$D" && md5sum -c MANIFEST.md --quiet ) \
        || die "基线 MANIFEST 校验失败 —— 拒绝用坏基线覆盖现场"
    say "   ✓ 基线 $(wc -l < "$D/MANIFEST.md") 件全 OK"

    RB="$GLOOT/k230-p2p-rollback-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RB"
    file_list | sed '/^$/d' | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue; [ -e "$src" ] || continue
        mkdir -p "$RB/$(dirname "$rel")"; cp -a "$src" "$RB/$rel"
    done
    ( cd "$RB" && find . -type f | sed 's|^\./||' | sort | xargs md5sum > MANIFEST.md ) 2>/dev/null
    say "   → 还原前现场已备份: $RB"

    was=$(systemctl is-active "$SVC" 2>/dev/null || true)
    say "-- 停 $SVC (was=$was)"
    systemctl stop "$SVC" 2>/dev/null || true
    sleep 1

    file_list | sed '/^$/d' | while IFS=: read -r src rel; do
        [ -n "$src" ] || continue
        [ -e "$D/$rel" ] || { echo "   ⚠ 基线里没这件: $rel"; continue; }
        mkdir -p "$(dirname "$src")"
        cp -a "$D/$rel" "$src.zz-restore" && mv -f "$src.zz-restore" "$src" \
            || echo "   ✗ 写入失败: $src"
    done
    systemctl daemon-reload 2>/dev/null || true

    if [ "$was" = "active" ]; then
        say "-- 起 $SVC"
        systemctl start "$SVC" 2>/dev/null || true
        i=0; while [ "$i" -lt 15 ]; do
            systemctl is-active --quiet "$SVC" && break
            sleep 1; i=$((i + 1))
        done
        say "   $(systemctl is-active "$SVC" 2>/dev/null)"
    else
        say "-- 原本没在跑, 不自动拉起 (要就 systemctl start $SVC)"
    fi

    say "-- 自检"
    $0 check "$D" || true
}

case "${1:-}" in
    snapshot) shift; do_snapshot "${1:-}" ;;
    check)    shift; do_check "${1:-}" ;;
    restore)  shift; do_restore "${1:-}" ;;
    list)     ls -d "$GLOOT/$PREFIX"* 2>/dev/null || echo "(无)" ;;
    *) echo "用法: sh $0 {snapshot|check|restore|list} [DIR]"; exit 1 ;;
esac
