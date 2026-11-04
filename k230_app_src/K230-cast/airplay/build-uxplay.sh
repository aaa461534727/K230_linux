#!/bin/sh
# build-uxplay.sh —— 在 K230 板上原地编译 UxPlay (AirPlay 接收端)
#
# 设计取舍:
#   · 板子是 Ubuntu 24.04 riscv64 + apt (aliyun 源), 直接 native 编译最省事,
#     不用折腾 riscv 交叉工具链和 sysroot;
#   · 只编协议层, **不用它自己的 GStreamer 上屏** (K230 单核软解跑不动),
#     视频由 -vdmp 直通到命名管道 → v4l2_dec 硬解 → drmplay (与 Miracast 同一条链路)。
#
# 依赖 (apt):
#   build-essential cmake pkg-config libplist-dev libssl-dev \
#   libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev
#
# 用法: sh build-uxplay.sh [源码tarball]
set -e

VER="${UXPLAY_VER:-master}"
TARBALL="${1:-}"
SRC="${UXPLAY_SRC:-/root/build/uxplay-src}"
PATCHDIR="${PATCHDIR:-$(cd "$(dirname "$0")" && pwd)/patches}"
URL="https://codeload.github.com/FDH2/UxPlay/tar.gz/refs/heads/${VER}"

# 优先用 vendored 源码 (仓里 src/miracast/cast/vendor/), 不联网也能编。
# 只有本地没有源码包时才去 GitHub 拉 (升级第三方时用)。
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -z "$TARBALL" ] && [ -s "$HERE/vendor/uxplay-master.tar.gz" ]; then
    TARBALL="$HERE/vendor/uxplay-master.tar.gz"
fi
if [ -z "$TARBALL" ]; then
    TARBALL=/tmp/uxplay-${VER}.tar.gz
    [ -s "$TARBALL" ] || curl -sSL -o "$TARBALL" "$URL"
fi
[ -s "$TARBALL" ] || { echo "没有源码包: $TARBALL" >&2; exit 1; }

rm -rf "$SRC"
mkdir -p "$SRC"
tar xzf "$TARBALL" -C "$SRC" --strip-components=1

for p in "$PATCHDIR"/*.patch; do
    echo "== apply $(basename "$p")"
    patch -p1 -d "$SRC" <"$p"
done

cd "$SRC"
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DNO_X11_DEPS=ON -DNO_MARCH_NATIVE=ON
make -j2

install -m 0755 uxplay /usr/local/bin/uxplay
echo "✅ 已安装: $(command -v uxplay) ($(md5sum /usr/local/bin/uxplay | cut -d" " -f1))"
