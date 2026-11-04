#!/bin/bash
# =============================================================================
# k230_extras_build.sh — 构建时从源码编译投屏栈, 装入 k230_extras/usr/bin
#
# 目的 (2026-11-04, 主人要求): SDK 里不再存"预编译好的二进制", 而是由构建
#   (make ubuntu) 现场从源码编译, 保证"以后编出来的固件天然含当前功能".
#   源码真相: K230/src (本项目的源树, 与 SDK 同级). 本脚本只引用, 不拷贝.
#
# 调用: distribution.sh 的 k230_apply_extras() 里, 覆盖 rootfs 之前.
#   env: K230_SDK_ROOT / BRW_BUILD_DIR (distribution.sh 已提供)
# =============================================================================
set -e

K230_SDK_ROOT="${K230_SDK_ROOT:?need K230_SDK_ROOT}"
BRW_BUILD_DIR="${BRW_BUILD_DIR:?need BRW_BUILD_DIR}"
# 源码真相 = SDK 自带 k230_app_src (用户维护的干净源码); K230/src 只是 AI 工作快照
SRC="$K230_SDK_ROOT/k230_app_src"
EXTRAS="$K230_SDK_ROOT/buildroot-overlay/board/canaan/k230-soc/distribution/k230_extras"
OUT="$BRW_BUILD_DIR"
STAGING="$OUT/staging"
CROSS="$OUT/host/bin/riscv64-unknown-linux-gnu-"
CC="${CROSS}gcc"

echo "=== [k230_extras_build] 源码=$SRC ==="
[ -d "$SRC/K230-cast/miracast/sink" ] || { echo "✗ 找不到 $SRC/K230-cast"; exit 1; }
[ -x "$CC" ] || { echo "✗ 找不到交叉编译器 $CC"; exit 1; }
export PATH="$OUT/host/bin:$PATH"

# 1) 投屏核心 (wfd-sink / drmplay / pcmplay / ai2dplay)
echo "=== [k230_extras_build] miracast/sink ==="
make -B -C "$SRC/K230-cast/miracast/sink" CROSS=riscv64-unknown-linux-gnu- ALSA_SYSROOT="$STAGING" \
    wfd-sink drmplay pcmplay ai2dplay

# 2) VPU 硬解 (v4l2_dec)
echo "=== [k230_extras_build] v4l2_dec ==="
"$CC" -O2 -D_GNU_SOURCE -static -o "$SRC/K230-cast/miracast/tools/v4l2_dec" \
    "$SRC/K230-cast/miracast/tools/v4l2_dec.c" -lpthread

# drmdump (DRM 诊断工具, 也从源码编)
echo "=== [k230_extras_build] drmdump ==="
"$CC" -O2 -D_GNU_SOURCE -static -o "$SRC/K230-cast/miracast/tools/drmdump" \
    "$SRC/K230-cast/miracast/tools/drmdump.c" 2>/dev/null || \
    "$CC" -O2 -D_GNU_SOURCE -o "$SRC/K230-cast/miracast/tools/drmdump" "$SRC/K230-cast/miracast/tools/drmdump.c"

# 3) eezUI: 主界面 (LVGL, 链 staging 的 liblvgl) + 后端
echo "=== [k230_extras_build] eezUI ==="
K230_SDK="$K230_SDK_ROOT" K230_SDK_CFG="$(basename "$OUT")" \
    bash "$SRC/eezUI/frontend/tools/board-build.sh"
make -B -C "$SRC/eezUI/backend" CROSS_COMPILE=riscv64-unknown-linux-gnu- CC="$CC" BUILD=build-board

# 4) CarPlay C 部分 (hal/arbiter/media); Rust 守护 carplay-airplayd 另计
echo "=== [k230_extras_build] carplay (C) ==="
make -B -C "$SRC/K230-cast/carplay" CROSS=riscv64-unknown-linux-gnu- all

# 5) CarLife (需 staging libusb)
echo "=== [k230_extras_build] carlife ==="
"$CC" -O2 -o "$SRC/K230-cast/carlife/carlife_sink" "$SRC"/K230-cast/carlife/*.c \
    -I"$STAGING/usr/include/libusb-1.0" -L"$STAGING/usr/lib" -lusb-1.0 -lpthread

# 6) UsbMirror (需 staging libusb)
echo "=== [k230_extras_build] usbmirror ==="
"$CC" -O2 -o "$SRC/K230-cast/usbmirror/usb_mirror" \
    "$SRC"/K230-cast/usbmirror/usb_mirror.c "$SRC"/K230-cast/usbmirror/adb.c \
    "$SRC"/K230-cast/usbmirror/rsa2048.c "$SRC"/K230-cast/usbmirror/touch.c \
    -I"$SRC/K230-cast/usbmirror" -I"$STAGING/usr/include/libusb-1.0" -L"$STAGING/usr/lib" -lusb-1.0 -lpthread

# 6.4) wpa_supplicant (K230 补丁版, 从 SDK 的 buildroot 源码编)
#   为什么在这里重编一道 (2026-11-04 板上实测的坑):
#     buildroot 默认开了 BR2_PACKAGE_WPA_SUPPLICANT_WPA3 ⇒ CONFIG_DPP/DPP2/DPP3=y,
#     其中 DPP3 的 HPKE 实现直接调 OpenSSL 3.2+ 的 OSSL_HPKE_* ⇒ 二进制会带
#     "OPENSSL_3.2.0" 版本节点。而 Ubuntu rootfs 的 libssl3t64 只有 3.0.13,
#     开机就死在 `version 'OPENSSL_3.2.0' not found` ⇒ supplicant 起不来 ⇒
#     没有 P2P listen ⇒ **手机根本发现不了 K230** (本次"点 Miracast 没反应"的真因)。
#   做法: 关掉 DPP 系列 (Miracast/WPS 用不到), 让二进制只依赖 OPENSSL_3.0.0,
#     直接吃 rootfs 自带的 openssl, 不需要再往 rootfs 里塞私有 libssl/libcrypto。
echo "=== [k230_extras_build] wpa_supplicant (DPP off) ==="
WPA_DIR="$OUT/build/wpa_supplicant-2.11/wpa_supplicant"
if [ -f "$WPA_DIR/.config" ]; then
    sed -i -e 's/^CONFIG_DPP=y/#CONFIG_DPP=y/' \
           -e 's/^CONFIG_DPP2=y/#CONFIG_DPP2=y/' \
           -e 's/^CONFIG_DPP3=y/#CONFIG_DPP3=y/' "$WPA_DIR/.config"
    grep -q '^#CONFIG_DPP3=y' "$WPA_DIR/.config" || { echo "✗ wpa .config 里没有 CONFIG_DPP3 可关"; exit 1; }
    make -C "$WPA_DIR" clean >/dev/null 2>&1 || true
    ( cd "$WPA_DIR" && \
      env CFLAGS="--sysroot=$STAGING -O2 -I$STAGING/usr/include/libnl3" \
          LDFLAGS="--sysroot=$STAGING -L$STAGING/usr/lib" \
          LIBS="-lnl-3 -lm -lpthread" LIBS_c="-lnl-3 -lm -lpthread" LIBS_p="-lnl-3 -lm -lpthread" \
          make -j"$(nproc)" CC="${CROSS}gcc" BINDIR=/usr/sbin >/dev/null )
    # 自检: 只要还引用 3.2 符号, 板上就一定起不来
    if readelf --dyn-syms -W "$WPA_DIR/wpa_supplicant" 2>/dev/null | grep -q 'OPENSSL_3\.2'; then
        echo "✗ wpa_supplicant 仍依赖 OPENSSL_3.2 (rootfs 只有 3.0.13)"; exit 1
    fi
    echo "  ✓ wpa_supplicant 重编完成 (仅依赖 OPENSSL_3.0, DPP 已关)"
else
    echo "  ! 找不到 $WPA_DIR/.config (先 make buildroot), 跳过 — distribution.sh 会用 buildroot 原产物"
fi

# 6.5) uxplay (AirPlay 服务端) 交叉编译 (依赖 staging 的 gstreamer/sdp/video/app/plist/dbus/glib/openssl)
echo "=== [k230_extras_build] uxplay (cross) ==="
CXX="$CROSS""g++"
UXTAR="$SRC/K230-cast/airplay/vendor/uxplay-master.tar.gz"
UXPAT="$SRC/K230-cast/airplay/patches"
UXB="/tmp/uxplay-cross"
if [ -f "$UXTAR" ]; then
    rm -rf "$UXB" 2>/dev/null || sudo rm -rf "$UXB" 2>/dev/null; mkdir -p "$UXB"
    tar xzf "$UXTAR" -C "$UXB" --strip-components=1
    for p in "$UXPAT"/*.patch; do [ -f "$p" ] && ( cd "$UXB" && patch -p1 <"$p" ); done
    cat > "$UXB/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_CXX_COMPILER $CXX)
set(CMAKE_SYSROOT $STAGING)
set(CMAKE_FIND_ROOT_PATH $STAGING)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    mkdir -p "$UXB/build"
    ( cd "$UXB/build" && \
      PKG_CONFIG_SYSROOT_DIR="$STAGING" \
      PKG_CONFIG_LIBDIR="$STAGING/usr/lib/pkgconfig:$STAGING/usr/share/pkgconfig" \
      cmake .. -DCMAKE_TOOLCHAIN_FILE="$UXB/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release -DNO_X11_DEPS=ON -DNO_MARCH_NATIVE=ON && \
      make -j"$(nproc)" )
    mkdir -p "$EXTRAS/usr/local/bin"
    install -m 0755 "$UXB/build/uxplay" "$EXTRAS/usr/local/bin/uxplay"
fi

# 6.6) CarPlay Rust 守护: carplay-airplayd + carplay-wireless
echo "=== [k230_extras_build] carplay rust ==="
if [ -n "${SUDO_USER:-}" ] && [ -x "/home/${SUDO_USER}/.cargo/bin/cargo" ]; then
    RUNAS="sudo -u ${SUDO_USER} env"
    CARGO_BIN="/home/${SUDO_USER}/.cargo/bin/cargo"
    RUSTUP_HOME="/home/${SUDO_USER}/.rustup" CARGO_HOME="/home/${SUDO_USER}/.cargo"
elif command -v cargo >/dev/null 2>&1; then
    RUNAS="env"
    CARGO_BIN="$(command -v cargo)"
else
    RUNAS=""; CARGO_BIN=""
fi
if [ -n "${CARGO_BIN}" ]; then
    ( cd "$SRC/K230-cast/carplay/rust/airplayd" && \
      $RUNAS CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER="$CC" "${CARGO_BIN}" build --release --target riscv64gc-unknown-linux-gnu )
    ( cd "$SRC/K230-cast/carplay/third_party/carplayd/rust/carplayd" && \
      $RUNAS CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER="$CC" "${CARGO_BIN}" build --release --target riscv64gc-unknown-linux-gnu -p carplay-wireless )
else
    echo "⚠ 无 cargo, 跳过 carplay rust"
fi

# 7) 装入 extras (distribution.sh 随后 cp -a extras -> rootfs)
echo "=== [k230_extras_build] install -> $EXTRAS/usr/bin ==="
mkdir -p "$EXTRAS/usr/bin"
install -m 0755 "$SRC/K230-cast/miracast/sink/wfd-sink"        "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/K230-cast/miracast/sink/drmplay"         "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/K230-cast/miracast/sink/pcmplay"         "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/K230-cast/miracast/sink/ai2dplay"        "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/K230-cast/miracast/tools/v4l2_dec"       "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/K230-cast/miracast/tools/drmdump"       "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/eezUI/frontend/build-board/k230ui"       "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/eezUI/backend/build-board/k230-ui-backend" "$EXTRAS/usr/bin/"
install -m 0755 "$SRC/eezUI/backend/build-board/k230ui-ctl"    "$EXTRAS/usr/bin/"
mkdir -p "$EXTRAS/usr/local/bin" "$EXTRAS/root/carlife" "$EXTRAS/root/usbmirror"
install -m 0755 "$SRC/K230-cast/carplay/build/carplay-hal"     "$EXTRAS/usr/local/bin/"
install -m 0755 "$SRC/K230-cast/carplay/build/carplay-arbiter" "$EXTRAS/usr/local/bin/"
install -m 0755 "$SRC/K230-cast/carplay/build/carplay-media"   "$EXTRAS/usr/local/bin/"
install -m 0755 "$SRC/K230-cast/carlife/carlife_sink"          "$EXTRAS/root/carlife/"
install -m 0755 "$SRC/K230-cast/carlife/carlife_sink"          "$EXTRAS/root/carlife_sink"
install -m 0755 "$SRC/K230-cast/usbmirror/usb_mirror"          "$EXTRAS/root/usbmirror/"
install -m 0755 "$SRC/K230-cast/carplay/rust/airplayd/target/riscv64gc-unknown-linux-gnu/release/carplay-airplayd" "$EXTRAS/usr/local/bin/"
install -m 0755 "$SRC/K230-cast/carplay/third_party/carplayd/rust/carplayd/target/riscv64gc-unknown-linux-gnu/release/carplay-wireless" "$EXTRAS/usr/local/bin/"

# root 编译产生物归还给源树属主, 免得弄脏用户仓库
OWNER="$(stat -c '%U:%G' "$SRC/README.md" 2>/dev/null || echo '')"
if [ -n "$OWNER" ]; then
    chown "$OWNER" "$SRC/K230-cast/miracast/sink/wfd-sink" "$SRC/K230-cast/miracast/sink/drmplay" \
        "$SRC/K230-cast/miracast/sink/pcmplay" "$SRC/K230-cast/miracast/sink/ai2dplay" \
        "$SRC/K230-cast/miracast/tools/v4l2_dec" "$SRC/eezUI/frontend/build-board/k230ui" \
        "$SRC/eezUI/backend/build-board/k230-ui-backend" "$SRC/eezUI/backend/build-board/k230ui-ctl" 2>/dev/null || true
fi
echo "=== [k230_extras_build] done ==="
