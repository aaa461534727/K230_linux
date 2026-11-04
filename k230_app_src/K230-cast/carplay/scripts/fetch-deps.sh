#!/bin/sh
# fetch-deps.sh — 取上游协议栈源码 (不 vendor 进仓: 规则「源码可拉, 编译产物不入仓」)
#
#   carplayd (lvalen91) — Unlicense / 公有领域
#     用它的两个 crate:
#       crates/iap2-core  iAP2 链路层(0xFF5A/0xFF55) + 识别 + MFi 认证 + 报文目录
#       crates/wireless   BlueZ 无代理 bring-up · SDP record · SSP Just-Works · RFCOMM · WiFi 交接编解码
#     main 分支是"有线"实现 (nusb/FunctionFS), 但这两个 crate 与传输无关, 直接能编到 riscv64。
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
PIN=4eb254499aa00f456aea91d00755cf150c7d18a4
DEST="$HERE/third_party/carplayd"

if [ -d "$DEST/.git" ]; then
    echo "已存在: $DEST ($(git -C "$DEST" rev-parse --short HEAD))"
    exit 0
fi

mkdir -p "$HERE/third_party"
git clone https://github.com/lvalen91/carplayd.git "$DEST"
git -C "$DEST" checkout "$PIN"

# 本仓对上游的改动全部以 patch 形式保存 (不 vendor 代码):
#   0002 = iap2-core 的 MFi 后端增加"本地离线 EC P-256 身份", 原 oracle 路径保留作回落
for p in "$HERE"/patches/000[2-9]-*.patch; do
    [ -f "$p" ] || continue
    echo "应用 $(basename "$p")"
    git -C "$DEST" apply "$p"
done
echo "carplayd @ $PIN 就位: $DEST"
