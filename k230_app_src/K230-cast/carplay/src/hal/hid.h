/* hid.h — CarPlay 触摸 HID 报告 (12 字节 / 2 触点)
 *
 * 规格来源: hichip/docs/CARPLAY_NATIVE_STACK.md §2.4 (从 PlayPort 现网 trace 逐字节解出)
 *   hidDescriptor = 05 0D | 09 04 | A1 01 | [fingerCollection(xMax,yMax)] x2 | C0
 *   报告格式      = 每触点 6 字节: [id:1][down:1][x:u16 LE][y:u16 LE], 共 2 个触点
 *   发送方式      = POST /command  body = bplist {type:"hidSendReport", uuid:<ts uuid>, hidReport:<12B>}
 *
 * 本文件只负责"触摸事件 → 12 字节", 不碰 JSON/plist/HTTP。
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_HID_H
#define CARPLAY_HID_H

#include <stdint.h>
#include <string.h>

#define CP_HID_REPORT_LEN 12
#define CP_HID_MAX_FINGERS 2

struct cp_hid_finger {
    uint8_t id;
    uint8_t down;
    uint16_t x;
    uint16_t y;
};

static inline void cp_hid_pack(const struct cp_hid_finger f[CP_HID_MAX_FINGERS],
                               uint8_t out[CP_HID_REPORT_LEN])
{
    memset(out, 0, CP_HID_REPORT_LEN);
    for (int i = 0; i < CP_HID_MAX_FINGERS; i++) {
        uint8_t *p = out + i * 6;
        p[0] = f[i].id;
        p[1] = f[i].down ? 1 : 0;
        p[2] = (uint8_t)(f[i].x & 0xFF);
        p[3] = (uint8_t)(f[i].x >> 8);
        p[4] = (uint8_t)(f[i].y & 0xFF);
        p[5] = (uint8_t)(f[i].y >> 8);
    }
}

#endif
