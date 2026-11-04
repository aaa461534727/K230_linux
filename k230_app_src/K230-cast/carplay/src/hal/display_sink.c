/* display_sink.c — 见 display_sink.h
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "display_sink.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int cp_display_open(struct cp_display_sink *s, const char *path, int w, int h, int wait_s)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->w = w;
    s->h = h;
    s->frame_bytes = (size_t)w * (size_t)h * 3 / 2;

    /* 写端 open 会阻塞到有读者为止 —— 这正是我们要的语义: 播放器没起就别开始推流。
     * 但绝不无限等: 10 s 还没读者说明是"拿到旧 inode"那个经典故障, 要报错让人看见。 */
    int waited = 0;
    for (;;) {
        s->fd = open(path, O_WRONLY | O_NONBLOCK);
        if (s->fd >= 0) {
            /* 清掉 O_NONBLOCK: 后续写要跟随播放器节奏阻塞, 不能丢帧丢得比它还快 */
            int fl = fcntl(s->fd, F_GETFL, 0);
            if (fl >= 0)
                fcntl(s->fd, F_SETFL, fl & ~O_NONBLOCK);
            break;
        }
        if (errno != ENXIO && errno != EAGAIN) {
            LOGE("打不开 %s: %s (管道不存在? k230-screen 没起?)", path, strerror(errno));
            return -1;
        }
        if (waited >= wait_s * 10) {
            LOGE("%s 在 %d s 内没有读者 —— drmplay 没起, 或有人拿着旧 inode", path, wait_s);
            return -1;
        }
        usleep(100000);
        waited++;
    }
    LOGI("显示管道已接上: %s (%dx%d, 每帧 %zu B)", path, w, h, s->frame_bytes);
    return 0;
}

/* drmplay 以 -H 模式启动 (见 common/k230-screen.sh: PLAYER_OPTS="-H -p nv12 -w 480 -h 800"),
 * 它按 16 字节帧头切帧: [magic "NV12"][w:u16 LE][h:u16 LE][len:u32 LE][ts_ms:u32 LE]。
 * 格式与 tools/v4l2_dec.c 的 emit_header() 逐字节一致 —— 不加头, drmplay 会一路
 * resync 找 magic, 结果就是「写了好几百帧、屏上全黑」 (2026-10-31 实测踩过)。 */
static void put_header(uint8_t hd[16], int w, int h, uint32_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t ms = (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
    uint16_t ww = (uint16_t)w, hh = (uint16_t)h;
    memset(hd, 0, 16);
    memcpy(hd, "NV12", 4);
    memcpy(hd + 4, &ww, 2);
    memcpy(hd + 6, &hh, 2);
    memcpy(hd + 8, &len, 4);
    memcpy(hd + 12, &ms, 4);
}

int cp_display_write(struct cp_display_sink *s, const void *nv12, size_t len)
{
    if (s->fd < 0)
        return -1;
    if (len != s->frame_bytes) {
        LOGE("帧长不符: 收到 %zu, 期望 %zu (%dx%d NV12)", len, s->frame_bytes, s->w, s->h);
        return -1;
    }
    uint8_t hd[16];
    put_header(hd, s->w, s->h, (uint32_t)len);
    const uint8_t *segs[2] = { hd, nv12 };
    size_t sizes[2] = { sizeof(hd), len };
    for (int k = 0; k < 2; k++) {
    const uint8_t *p = segs[k];
    size_t left = sizes[k];
    while (left > 0) {
        ssize_t n = write(s->fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                usleep(2000); /* 播放器暂时跟不上: 短暂让出, 丢帧由上层决定 */
                s->frame_drops++;
                return -1;
            }
            s->fd = -1;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    }
    if (s->frames == 0)
        s->t_first_ns = now_ns();
    s->t_last_ns = now_ns();
    s->frames++;
    s->bytes += len;
    return 0;
}

double cp_display_fps(const struct cp_display_sink *s, uint64_t *total)
{
    if (total)
        *total = s->frames;
    if (s->frames < 2 || s->t_last_ns <= s->t_first_ns)
        return 0.0;
    double dt = (double)(s->t_last_ns - s->t_first_ns) / 1e9;
    return dt > 0 ? (double)(s->frames - 1) / dt : 0.0;
}

void cp_display_close(struct cp_display_sink *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
