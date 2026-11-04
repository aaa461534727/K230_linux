/* touch.c —— 反控实现 (详见 touch.h)
 *
 * 关键实测 (2026-11-03, 中兴 NX589J / Android 7.1.2 / arm64-v8a):
 *   · 手机没有 input motionevent (Android 10+ 才有) ⇒ 只靠 input 命令做不出实时拖动
 *   · 但 shell 用户在 input 组 (uid=2000, groups 含 1004(input)) ⇒ 能直接写 /dev/input
 *   · arm64 ⇒ struct input_event = 24 字节 (timeval 是两个 8 字节 long; 32 位是 16)
 *   · 通道必须用 exec: (无 PTY)。shell: 有 PTY, 写二进制会被终端驱动改字节
 *     (0x0a/0x0d/0x03 会被当成换行/Ctrl-C), 坐标必然乱。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <linux/input.h>
#include "touch.h"

#define LOG(...) do { fprintf(stderr, "[touch] " __VA_ARGS__); fflush(stderr); } while (0)

static struct {
    adb_dev *dev;
    uint32_t chan, remote;
    int fd, struct_size;
    int phys_w, phys_h, panel_w, panel_h;
    double scale, off_x, off_y;
    int raw;                 /* 1 = 原始 evdev 注入; 0 = input 命令兜底 */
    int last_down, lx, ly;
    long last_move_ms;
    volatile int stop;
    int chan_alive;
    /* 兜底模式的起点 */
    int fb_x0, fb_y0, fb_x1, fb_y1;
    long fb_t0;
} g;
static pthread_t g_th;
static int g_running;

static long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32s(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int ev_pack(uint8_t *buf, int n, uint16_t type, uint16_t code, int32_t value)
{
    uint8_t *p = buf + n * g.struct_size;
    memset(p, 0, g.struct_size);
    if (g.struct_size == 24) {
        put16(p + 16, type); put16(p + 18, code); put32s(p + 20, value);
    } else {
        put16(p + 8, type);  put16(p + 10, code); put32s(p + 12, value);
    }
    return n + 1;
}

static void ev_flush(uint8_t *buf, int n)
{
    if (n <= 0 || !g.chan_alive) return;
    adb_send_packet(g.dev, g.chan, g.remote, buf, n * g.struct_size);
}

/* 兜底: 往 shell 通道写一条 input 命令 (PTY 对纯文本无害) */
static void fb_cmd(const char *fmt, ...)
{
    if (!g.chan_alive) return;
    char line[160];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    strncat(line, "\n", sizeof line - strlen(line) - 1);
    adb_send_packet(g.dev, g.chan, g.remote, line, (int)strlen(line));
}

static void map_point(int sx, int sy, int *ox, int *oy)
{
    int px = (int)((sx - g.off_x) / g.scale);
    int py = (int)((sy - g.off_y) / g.scale);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > g.phys_w - 1) px = g.phys_w - 1;
    if (py > g.phys_h - 1) py = g.phys_h - 1;
    *ox = px; *oy = py;
}

static void *reader(void *arg)
{
    (void)arg;
    struct input_event ev;
    uint8_t batch[32 * 24];
    int cur_x = 0, cur_y = 0;

    while (!g.stop) {
        ssize_t n = read(g.fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            if (errno == EINTR) continue;
            usleep(10000);
            continue;
        }
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) cur_x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) cur_y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            int px, py;
            map_point(cur_x, cur_y, &px, &py);
            if (ev.value) {
                if (g.raw) {
                    int k = 0;
                    k = ev_pack(batch, k, EV_ABS, ABS_MT_SLOT, 0);
                    k = ev_pack(batch, k, EV_ABS, ABS_MT_TRACKING_ID, 1);
                    k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_X, px);
                    k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_Y, py);
                    k = ev_pack(batch, k, EV_KEY, BTN_TOUCH, 1);
                    k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
                    ev_flush(batch, k);
                } else {
                    g.fb_x0 = g.fb_x1 = px; g.fb_y0 = g.fb_y1 = py; g.fb_t0 = now_ms();
                }
                g.last_down = 1;
            } else {
                if (g.raw) {
                    int k = 0;
                    k = ev_pack(batch, k, EV_ABS, ABS_MT_TRACKING_ID, -1);
                    k = ev_pack(batch, k, EV_KEY, BTN_TOUCH, 0);
                    k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
                    ev_flush(batch, k);
                } else {
                    long dur = now_ms() - g.fb_t0;
                    int dx = g.fb_x1 - g.fb_x0, dy = g.fb_y1 - g.fb_y0;
                    int far = dx * dx + dy * dy >= 12 * 12;
                    if (!far && dur < 400)                                   fb_cmd("input tap %d %d", g.fb_x1, g.fb_y1);
                    else if (!far)                                           fb_cmd("input swipe %d %d %d %d 900", g.fb_x0, g.fb_y0, g.fb_x0, g.fb_y0);
                    else                                                     fb_cmd("input swipe %d %d %d %d %d",
                                                                                     g.fb_x0, g.fb_y0, g.fb_x1, g.fb_y1,
                                                                                     dur < 60 ? 60 : (dur > 3000 ? 3000 : (int)dur));
                }
                g.last_down = 0;
            }
        } else if (ev.type == EV_SYN) {
            if (!g.last_down) continue;
            if (cur_x == g.lx && cur_y == g.ly) continue;
            long ms = now_ms();
            if (ms - g.last_move_ms < 16) continue;   /* 60 Hz 上限 */
            g.last_move_ms = ms;
            g.lx = cur_x; g.ly = cur_y;
            int px, py;
            map_point(cur_x, cur_y, &px, &py);
            if (g.raw) {
                int k = 0;
                k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_X, px);
                k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_Y, py);
                k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
                ev_flush(batch, k);
            } else {
                g.fb_x1 = px; g.fb_y1 = py;   /* 兜底只在抬起时补一次手势 */
            }
        }
    }
    return NULL;
}

int touch_start(const struct touch_cfg *cfg, uint32_t remote)
{
    memset(&g, 0, sizeof g);
    g.dev = cfg->dev; g.chan = cfg->chan; g.remote = remote;
    g.struct_size = cfg->struct_size;
    g.phys_w = cfg->phys_w; g.phys_h = cfg->phys_h;
    g.panel_w = cfg->panel_w; g.panel_h = cfg->panel_h;
    g.chan_alive = 1;
    g.raw = (cfg->evdev && cfg->evdev[0]) ? 1 : 0;

    double sx = (double)g.panel_w / g.phys_w, sy = (double)g.panel_h / g.phys_h;
    g.scale = sx < sy ? sx : sy;
    if (g.scale <= 0) g.scale = 1.0;
    g.off_x = (g.panel_w - g.phys_w * g.scale) / 2.0;
    g.off_y = (g.panel_h - g.phys_h * g.scale) / 2.0;

    if (g.raw)
        LOG("✓ 反控已开: 原始 evdev 注入 → %s (结构 %d B, 手机 %dx%d, 屏 %dx%d)\n",
            cfg->evdev, g.struct_size, g.phys_w, g.phys_h, g.panel_w, g.panel_h);
    else
        LOG("⚠ 反控退回 input 命令模式 (手机 shell 不在 input 组): 点按/滑动可用, 无实时拖动\n");

    g.fd = open(cfg->board_evdev, O_RDONLY | O_NONBLOCK);
    if (g.fd < 0) { LOG("✗ 打不开板端触摸 %s: %s\n", cfg->board_evdev, strerror(errno)); return -1; }

    g.stop = 0;
    if (pthread_create(&g_th, NULL, reader, NULL) != 0) { close(g.fd); return -1; }
    g_running = 1;
    return 0;
}

/* 注入一段手势: 按下 → 分步移动 → 抬起。每步一个批次一个 WRTE。 */
int touch_gesture(int sx0, int sy0, int sx1, int sy1, int ms)
{
    if (!g.chan_alive) return -1;
    int px, py;
    uint8_t batch[32 * 24];

    map_point(sx0, sy0, &px, &py);
    if (g.raw) {
        int k = 0;
        k = ev_pack(batch, k, EV_ABS, ABS_MT_SLOT, 0);
        k = ev_pack(batch, k, EV_ABS, ABS_MT_TRACKING_ID, 7);
        k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_X, px);
        k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_Y, py);
        k = ev_pack(batch, k, EV_KEY, BTN_TOUCH, 1);
        k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
        ev_flush(batch, k);
    } else {
        g.fb_x0 = px; g.fb_y0 = py;
    }
    LOG("自测点按下: 屏(%d,%d) → 手机(%d,%d)\n", sx0, sy0, px, py);

    int steps = ms / 16; if (steps < 4) steps = 4; if (steps > 60) steps = 60;
    for (int i = 1; i <= steps; i++) {
        int x = sx0 + (sx1 - sx0) * i / steps;
        int y = sy0 + (sy1 - sy0) * i / steps;
        map_point(x, y, &px, &py);
        if (g.raw) {
            int k = 0;
            k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_X, px);
            k = ev_pack(batch, k, EV_ABS, ABS_MT_POSITION_Y, py);
            k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
            ev_flush(batch, k);
        } else {
            g.fb_x1 = px; g.fb_y1 = py;
        }
        usleep(ms * 1000 / steps);
    }

    map_point(sx1, sy1, &px, &py);
    if (g.raw) {
        int k = 0;
        k = ev_pack(batch, k, EV_ABS, ABS_MT_TRACKING_ID, -1);
        k = ev_pack(batch, k, EV_KEY, BTN_TOUCH, 0);
        k = ev_pack(batch, k, EV_SYN, SYN_REPORT, 0);
        ev_flush(batch, k);
    } else {
        fb_cmd("input swipe %d %d %d %d %d", g.fb_x0, g.fb_y0, g.fb_x1, g.fb_y1, ms);
    }
    LOG("自测点抬起: 屏(%d,%d) → 手机(%d,%d), 共 %d 步\n", sx1, sy1, px, py, steps);
    return 0;
}

void touch_stop(void)
{
    if (!g_running) return;
    g.stop = 1;
    pthread_join(g_th, NULL);
    if (g.fd > 0) close(g.fd);
    g.fd = -1;
    g_running = 0;
    LOG("反控已停\n");
}

void touch_note_closed(void) { g.chan_alive = 0; }
