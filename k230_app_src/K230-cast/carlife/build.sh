#!/bin/sh
# 在 K230 板上本机编译 (板载 gcc 13 + libusb-1.0-dev)
set -e
cd "$(dirname "$0")"
gcc -O2 -Wall -Wextra -o carlife_sink \
    carlife_sink.c carlife_proto.c carlife_usb.c carlife_tcp.c carlife_wifi.c \
    -I/usr/include/libusb-1.0 -lusb-1.0 -lpthread
echo "✅ build ok: $(pwd)/carlife_sink"
