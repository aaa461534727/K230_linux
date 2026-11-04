/*
 * audio_out.c —— Miracast 音频出口: RTP 里的 LPCM (16bit 大端) → 命名管道里的 S16LE
 *
 * 为什么走管道而不是直接开 ALSA:
 *   ALSA 设备 (inno codec) 同一时刻只能被一个进程独占。苹果 AirPlay 也要出声,
 *   而两者又是互斥的 —— 所以统一由一个常驻播放器 (pcmplay) 持有 ALSA,
 *   两条投屏链路都只是「往 /run/wfd-audio.pcm 写 S16LE 48k 2ch」。
 *   换源时播放器不用重启, 也就不会有爆音/断音。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "audio_out.h"

static int g_fd = -1;
static unsigned long long g_bytes = 0;
static unsigned long long g_drop = 0;

int audio_out_open(const char *path)
{
    if (!path || !*path)
        path = getenv("WFD_AUDIO_FIFO");
    if (!path || !*path)
        path = "/run/wfd-audio.pcm";
    if (getenv("WFD_AUDIO") && !strcmp(getenv("WFD_AUDIO"), "0")) {
        fprintf(stderr, "[audio] WFD_AUDIO=0, 不开音频出口\n");
        return 0;
    }
    mkfifo(path, 0666);
    /* 非阻塞: 播放器没起时不能把 sink 卡死 */
    g_fd = open(path, O_WRONLY | O_NONBLOCK);
    if (g_fd < 0 && errno == ENXIO) {
        fprintf(stderr, "[audio] ⚠ 管道 %s 暂无读端 (pcmplay 没起), 音频丢弃\n", path);
        g_fd = -1;
        return 0;
    }
    if (g_fd < 0) {
        fprintf(stderr, "[audio] 打开 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[audio] ✅ 音频出口就绪: %s (S16LE 48k 2ch)\n", path);
    return 0;
}

void audio_out_close(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = -1;
}

unsigned long long audio_out_bytes(void) { return g_bytes; }
unsigned long long audio_out_dropped(void) { return g_drop; }

/* 把一段大端 16bit 立体声 PCM 转成小端写进管道 (字节数必须是 4 的整数倍) */
void audio_out_pcm_be16(const unsigned char *p, int len)
{
    unsigned char tmp[8192];
    int i;

    if (g_fd < 0 || !p || len <= 0)
        return;
    len &= ~3;                       /* 只处理完整的 L/R 帧 */
    while (len > 0) {
        int n = len < (int)sizeof(tmp) ? len : (int)sizeof(tmp);
        for (i = 0; i + 1 < n; i += 2) {
            tmp[i]     = p[i + 1];
            tmp[i + 1] = p[i];
        }
        ssize_t w = write(g_fd, tmp, (size_t)n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                g_drop += (unsigned long long)n;   /* 播放器跟不上: 丢这一小段 */
            } else {
                audio_out_close();
                return;
            }
        } else {
            g_bytes += (unsigned long long)w;
        }
        p += n;
        len -= n;
    }
}

/* 已经是小端 16bit 的 PCM: 原样写 (TS 里的 LPCM 有的 muxer 直接给主机序) */
void audio_out_pcm_le16(const unsigned char *p, int len)
{
    unsigned char tmp[8192];

    if (g_fd < 0 || !p || len <= 0)
        return;
    len &= ~3;
    while (len > 0) {
        int n = len < (int)sizeof(tmp) ? len : (int)sizeof(tmp);
        memcpy(tmp, p, (size_t)n);
        ssize_t w = write(g_fd, tmp, (size_t)n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                g_drop += (unsigned long long)n;
            else {
                audio_out_close();
                return;
            }
        } else {
            g_bytes += (unsigned long long)w;
        }
        p += n;
        len -= n;
    }
}
