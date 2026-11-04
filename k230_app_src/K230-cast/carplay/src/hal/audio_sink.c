/* audio_sink.c — 见 audio_sink.h
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_sink.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int cp_audio_open(struct cp_audio_sink *s, const char *path, int wait_s)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    int waited = 0;
    for (;;) {
        s->fd = open(path, O_WRONLY | O_NONBLOCK);
        if (s->fd >= 0) {
            int fl = fcntl(s->fd, F_GETFL, 0);
            if (fl >= 0)
                fcntl(s->fd, F_SETFL, fl & ~O_NONBLOCK);
            break;
        }
        if (errno != ENXIO && errno != EAGAIN) {
            LOGE("打不开音频管道 %s: %s", path, strerror(errno));
            return -1;
        }
        if (waited >= wait_s * 10) {
            LOGE("音频管道 %s 在 %d s 内没有读者 (k230-audio 没起?)", path, wait_s);
            return -1;
        }
        usleep(100000);
        waited++;
    }
    LOGI("音频管道已接上: %s (S16LE %d Hz %d ch)", path, CP_AUDIO_RATE, CP_AUDIO_CH);
    return 0;
}

int cp_audio_write(struct cp_audio_sink *s, const void *pcm, size_t len)
{
    if (s->fd < 0)
        return -1;
    const uint8_t *p = pcm;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(s->fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                usleep(2000);
                return -1;
            }
            s->fd = -1;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    s->bytes += len;
    return 0;
}

void cp_audio_close(struct cp_audio_sink *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
