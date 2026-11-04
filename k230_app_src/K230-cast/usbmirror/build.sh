#!/bin/sh
# 板上编译 (板载 gcc). 交叉编译同理: CC=riscv64-xxx-gcc ./build.sh
set -e
cd "$(dirname "$0")"
CC=${CC:-gcc}
$CC -O2 -Wall -o usb_mirror usb_mirror.c adb.c rsa2048.c touch.c -I. -lusb-1.0 -lpthread
echo "✓ usb_mirror 已生成: $(pwd)/usb_mirror"
