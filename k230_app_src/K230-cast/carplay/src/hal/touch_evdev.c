/* touch_evdev.c — 见 touch_evdev.h
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "touch_evdev.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int abs_max(int fd, int axis)
{
    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    if (ioctl(fd, EVIOCGABS(axis), &ai) < 0)
        return -1;
    return ai.maximum;
}

int cp_touch_open(struct cp_touch *t, const char *dev, int out_w, int out_h,
                  int grab, cp_touch_cb cb, void *user)
{
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->out_w = out_w;
    t->out_h = out_h;
    t->cb = cb;
    t->user = user;
    t->slot = 0;

    t->fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (t->fd < 0) {
        LOGE("打开触摸 %s: %s", dev, strerror(errno));
        return -1;
    }
    t->raw_x_max = abs_max(t->fd, ABS_MT_POSITION_X);
    t->raw_y_max = abs_max(t->fd, ABS_MT_POSITION_Y);
    if (t->raw_x_max <= 0)
        t->raw_x_max = abs_max(t->fd, ABS_X);
    if (t->raw_y_max <= 0)
        t->raw_y_max = abs_max(t->fd, ABS_Y);
    if (t->raw_x_max <= 0 || t->raw_y_max <= 0) {
        LOGE("%s 不是绝对坐标设备 (x_max=%d y_max=%d)", dev, t->raw_x_max, t->raw_y_max);
        close(t->fd);
        t->fd = -1;
        return -1;
    }
    if (grab) {
        if (ioctl(t->fd, EVIOCGRAB, 1) < 0)
            LOGI("EVIOCGRAB 失败 (%s): 触摸仍会被其它进程收到", strerror(errno));
    }
    t->grab = grab;
    LOGI("触摸已接上: %s 原始 %dx%d → CarPlay %dx%d%s", dev, t->raw_x_max, t->raw_y_max,
         out_w, out_h, grab ? " (独占)" : "");
    return 0;
}

static uint16_t map_axis(int32_t v, int raw_max, int out_max)
{
    if (raw_max <= 0)
        return 0;
    if (v < 0)
        v = 0;
    if (v > raw_max)
        v = raw_max;
    return (uint16_t)((int64_t)v * out_max / raw_max);
}

static void emit(struct cp_touch *t)
{
    struct cp_hid_finger f[CP_HID_MAX_FINGERS];
    for (int i = 0; i < CP_HID_MAX_FINGERS; i++) {
        if (!t->touching[i]) {
            /* 空闲触点必须整条清零: 现场实测 f1 抬起后仍带着上一次的 (x,y),
             * 原样发出去 iOS 会把那只"幽灵手指"当成一直按在 425,128。 */
            f[i].id = 0;
            f[i].down = 0;
            f[i].x = 0;
            f[i].y = 0;
            continue;
        }
        /*触点 id 用 slot 序号 (0/1), 不是内核的 ABS_MT_TRACKING_ID:
         * 跟踪 ID 会随每次按下递增 (实测 94/95/96…), 而 HID digitizer 里
         * 它只是"第几个触点", 超过触点数的值 iOS 侧行为未定义。 */
        f[i].id = (uint8_t)i;
        f[i].down = 1;
        f[i].x = map_axis(t->slot_x[i], t->raw_x_max, t->out_w);
        f[i].y = map_axis(t->slot_y[i], t->raw_y_max, t->out_h);
    }
    uint8_t rep[CP_HID_REPORT_LEN];
    cp_hid_pack(f, rep);
    t->events++;
    if (t->cb)
        t->cb(rep, t->user);
}

int cp_touch_poll(struct cp_touch *t, int timeout_ms)
{
    if (t->fd < 0)
        return -1;
    struct pollfd pfd = { .fd = t->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    int reports = 0;
    for (;;) {
        struct input_event ev[16];
        ssize_t n = read(t->fd, ev, sizeof(ev));
        if (n <= 0)
            break;
        int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < cnt; i++) {
            switch (ev[i].type) {
            case EV_ABS:
                switch (ev[i].code) {
                case ABS_MT_SLOT:
                    t->slot = ev[i].value;
                    if (t->slot < 0 || t->slot >= CP_HID_MAX_FINGERS)
                        t->slot = 0;
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev[i].value < 0) {
                        t->touching[t->slot] = 0;
                        t->slot_id[t->slot] = 0;
                    } else {
                        t->touching[t->slot] = 1;
                        t->slot_id[t->slot] = ev[i].value;
                    }
                    break;
                case ABS_MT_POSITION_X:
                    t->slot_x[t->slot] = ev[i].value;
                    break;
                case ABS_MT_POSITION_Y:
                    t->slot_y[t->slot] = ev[i].value;
                    break;
                default:
                    break;
                }
                break;
            case EV_SYN:
                if (ev[i].code == SYN_REPORT) {
                    emit(t);
                    reports++;
                }
                break;
            default:
                break;
            }
        }
    }
    return reports;
}

void cp_touch_close(struct cp_touch *t)
{
    if (t->fd >= 0) {
        if (t->grab)
            ioctl(t->fd, EVIOCGRAB, 0);
        close(t->fd);
        t->fd = -1;
    }
}
