/* touch.h —— 反控 (把 K230 屏上的触摸注入到手机)
 *
 * 两条路, 按手机实际情况自动选:
 *   A. 原始 evdev 注入 (首选): 手机 shell 用户在 input 组里, 直接往
 *      /dev/input/eventN 写 input_event 结构 ⇒ 真·实时多点, 延迟就是 USB 往返。
 *      Android 5~14 通用, 不依赖 input 命令的版本。
 *   B. input 命令兜底 (手机 shell 不在 input 组时): exec:input tap / swipe / keyevent,
 *      只能在手指抬起后补一次完整手势, 没有实时拖动。
 */
#ifndef TOUCH_H
#define TOUCH_H

#include "adb.h"

struct touch_cfg {
    adb_dev *dev;
    uint32_t chan;          /* 本端通道 id */
    const char *evdev;      /* 手机上的 /dev/input/eventN, NULL = 用兜底方案 */
    int struct_size;        /* input_event 结构大小: 64 位=24, 32 位=16 */
    int phys_w, phys_h;     /* 手机物理分辨率 (注入坐标用这个) */
    int panel_w, panel_h;   /* 本板屏 */
    const char *board_evdev;
};

int  touch_start(const struct touch_cfg *cfg, uint32_t remote);

/* 自测: 直接注入一段手势 (屏坐标), 走的是和真手指完全相同的打包/发送路径。
 * 用来在没有手指的情况下验证「手机是否真的收到并响应」——
 * 配合手机端 screencap 前后对比 md5 就是硬证据。 */
int  touch_gesture(int sx0, int sy0, int sx1, int sy1, int ms);
void touch_stop(void);
void touch_note_closed(void);   /* 主循环收到该通道 CLSE 时调用 */

#endif
