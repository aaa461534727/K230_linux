#!/bin/sh
# 找手机的触摸屏: 哪个 /dev/input/eventN 同时有 ABS_MT_POSITION_X 和 ABS_MT_TRACKING_ID
cd "$(dirname "$0")" || exit 1
for d in 0 1 2 3 4 5 6 7 8 9 10; do
    out=$(./adb_cmd "exec:getevent -p /dev/input/event$d" 2500 2>/dev/null)
    name=$(printf '%s' "$out" | sed -n 's/.*name: *"\(.*\)".*/\1/p' | head -1)
    if printf '%s' "$out" | grep -q ABS_MT_POSITION_X; then
        echo "event$d  name=[$name]  <== 触摸屏"
        printf '%s' "$out" | grep -E "ABS_MT_POSITION_[XY]|ABS_MT_TRACKING_ID|ABS_MT_TOUCH_MAJOR|ABS_MT_PRESSURE|BTN_TOUCH" | sed 's/^/        /'
    elif [ -n "$name" ]; then
        echo "event$d  name=[$name]"
    fi
done
