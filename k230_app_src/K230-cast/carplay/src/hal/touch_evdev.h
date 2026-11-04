/* touch_evdev.h — FT5x06 (event0) → CarPlay 触摸 HID
 *
 * 板端实测: /proc/bus/input/devices → Name="generic ft5x06 (79)", Handlers=mouse0 event0
 * 触摸是多点 (MT-B 协议): ABS_MT_SLOT / ABS_MT_TRACKING_ID / ABS_MT_POSITION_X|Y。
 * 输出 = 12 字节 HID (见 hid.h), 与 PlayPort 现网报文逐字节同构。
 *
 * 坐标系: evdev 的绝对范围由 EVIOCGABS 读出来 (别写死!), 再线性映射到 CarPlay 的
 * displays.widthPixels/heightPixels (本板 480x800)。K230 屏是竖屏, 不做旋转 —— 转不转
 * 由 iOS 侧按 /info 的宽高自行布局。
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_TOUCH_EVDEV_H
#define CARPLAY_TOUCH_EVDEV_H

#include <stdint.h>
#include "hid.h"

#define CP_TOUCH_DEFAULT_DEV "/dev/input/event0"

typedef void (*cp_touch_cb)(const uint8_t report[CP_HID_REPORT_LEN], void *user);

struct cp_touch {
    int fd;
    int grab;
    int out_w, out_h;
    int raw_x_max, raw_y_max;
    /* 每个 slot 的原始状态 */
    int slot;
    int32_t slot_x[CP_HID_MAX_FINGERS];
    int32_t slot_y[CP_HID_MAX_FINGERS];
    int32_t slot_id[CP_HID_MAX_FINGERS];
    int touching[CP_HID_MAX_FINGERS];
    cp_touch_cb cb;
    void *user;
    uint64_t events;
};

int cp_touch_open(struct cp_touch *t, const char *dev, int out_w, int out_h,
                  int grab, cp_touch_cb cb, void *user);
/* 阻塞读一批事件, 产生 0..N 个 HID 报告 (每次状态变化一个)。返回报告数, -1 = 设备没了 */
int cp_touch_poll(struct cp_touch *t, int timeout_ms);
void cp_touch_close(struct cp_touch *t);

#endif
