/* h264_decoder.c — 见 h264_decoder.h
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "h264_decoder.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int cp_decoder_start(struct cp_h264_decoder *d, const char *out_path, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->pid = -1;
    d->in_fd = -1;
    d->w = w;
    d->h = h;
    snprintf(d->in_path, sizeof(d->in_path), "/run/carplay-%d.h264", (int)getpid());
    snprintf(d->out_path, sizeof(d->out_path), "%s", out_path);

    unlink(d->in_path);
    if (mkfifo(d->in_path, 0666) != 0 && errno != EEXIST) {
        LOGE("mkfifo %s: %s", d->in_path, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char ws[16], hs[16];
        snprintf(ws, sizeof(ws), "%d", w);
        snprintf(hs, sizeof(hs), "%d", h);
        execl(CP_DEC_BIN, CP_DEC_BIN, d->in_path, d->out_path,
              "-w", ws, "-h", hs, (char *)NULL);
        _exit(127);
    }
    d->pid = pid;

    /* 写端: 等解码器把读端打开 */
    for (int i = 0; i < 100; i++) {
        d->in_fd = open(d->in_path, O_WRONLY);
        if (d->in_fd >= 0)
            break;
        usleep(50000);
    }
    if (d->in_fd < 0) {
        LOGE("打不开解码器输入 FIFO %s: %s", d->in_path, strerror(errno));
        kill(pid, SIGKILL);
        d->pid = -1;
        return -1;
    }
    LOGI("硬解已起: pid=%d, %s → %s (%dx%d)", (int)pid, d->in_path, out_path, w, h);
    return 0;
}

int cp_decoder_spawn(struct cp_h264_decoder *d, const char *in_path, const char *out_path,
                     int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->pid = -1;
    d->in_fd = -1;
    d->w = w;
    d->h = h;
    snprintf(d->in_path, sizeof(d->in_path), "%s", in_path);
    snprintf(d->out_path, sizeof(d->out_path), "%s", out_path);

    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (w > 0 && h > 0) {
            char ws[16], hs[16];
            snprintf(ws, sizeof(ws), "%d", w);
            snprintf(hs, sizeof(hs), "%d", h);
            execl(CP_DEC_BIN, CP_DEC_BIN, in_path, out_path, "-w", ws, "-h", hs, (char *)NULL);
        } else {
            execl(CP_DEC_BIN, CP_DEC_BIN, in_path, out_path, (char *)NULL);
        }
        _exit(127);
    }
    d->pid = pid;
    LOGI("硬解已起 (外部管道): pid=%d, %s → %s", (int)pid, in_path, out_path);
    return 0;
}

ssize_t cp_decoder_feed(struct cp_h264_decoder *d, const void *data, size_t len)
{
    if (d->in_fd < 0)
        return -1;
    const uint8_t *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(d->in_fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EPIPE)
                return (ssize_t)(len - left);
            d->in_fd = -1;
            return -1;
        }
        p += n;
        left -= (size_t)n;
        d->fed_bytes += (uint64_t)n;
    }
    return (ssize_t)len;
}

int cp_decoder_alive(struct cp_h264_decoder *d)
{
    if (d->pid <= 0)
        return 0;
    int st = 0;
    pid_t r = waitpid(d->pid, &st, WNOHANG);
    if (r == 0)
        return 1;
    LOGE("硬解退出: pid=%d status=0x%x", (int)d->pid, st);
    d->pid = -1;
    return 0;
}

void cp_decoder_stop(struct cp_h264_decoder *d)
{
    if (d->in_fd >= 0) {
        close(d->in_fd);
        d->in_fd = -1;
    }
    if (d->pid > 0) {
        kill(d->pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            if (waitpid(d->pid, NULL, WNOHANG) == d->pid) {
                d->pid = -1;
                break;
            }
            usleep(100000);
        }
        if (d->pid > 0) {
            kill(d->pid, SIGKILL);
            waitpid(d->pid, NULL, 0);
            d->pid = -1;
        }
    }
    unlink(d->in_path);
}
