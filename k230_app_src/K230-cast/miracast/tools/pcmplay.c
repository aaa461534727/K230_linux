/*
 * pcmplay.c —— 常驻 ALSA 播放器: 从命名管道吃 S16LE/48k/2ch PCM, 空闲时补静音
 *
 * 为什么不用 aplay: aplay 遇到管道空就退出/报 underrun 重开, 投屏断流时设备跟着
 * 关开 (pop 声), 而且每换一次 source 要重启进程。这里做成常驻:
 *   · 管道用 O_RDWR 打开 (自己也持有写端) ⇒ 写端进程退出也不会 EOF;
 *   · 缓冲里有数据就写, 没有就写静音 —— ALSA 设备永远跑着, 不重开、不爆音。
 *
 * 用法: pcmplay [-f <fifo>] [-d <alsa dev>] [-r rate] [-c ch]
 *   默认 -f /run/wfd-audio.pcm -d default -r 48000 -c 2
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
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <alsa/asoundlib.h>

#define LOG(...) do { fprintf(stderr, "[pcmplay] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* 环形缓冲 (单帧 = 4 字节, 立体声 16bit) */
#define RING_FRAMES 96000            /* 2 s @48k, 足够吸收网络抖动 */
static unsigned char ring[RING_FRAMES * 4];
static int r_head = 0, r_tail = 0, r_used = 0;

static void ring_put(const unsigned char *p, int n)
{
    while (n > 0 && r_used < RING_FRAMES) {
        int chunk = n < RING_FRAMES - r_used ? n : RING_FRAMES - r_used;
        for (int i = 0; i < chunk; i++) {
            memcpy(ring + (r_head * 4), p + i * 4, 4);
            r_head = (r_head + 1) % RING_FRAMES;
        }
        r_used += chunk;
        p += chunk * 4;
        n -= chunk;
    }
}

static void ring_get(unsigned char *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (r_used > 0) {
            memcpy(p + i * 4, ring + (r_tail * 4), 4);
            r_tail = (r_tail + 1) % RING_FRAMES;
            r_used--;
        } else {
            memset(p + i * 4, 0, 4);
        }
    }
}

int main(int argc, char **argv)
{
    const char *fifo = "/run/wfd-audio.pcm";
    const char *dev = "default";
    unsigned rate = 48000;
    int ch = 2;
    int gain = 100;          /* 软件增益 %, 100 = 不动 (2026-10-29: 硬件音量之外再压一道) */
    int opt;

    while ((opt = getopt(argc, argv, "f:d:r:c:g:h")) != -1) {
        switch (opt) {
        case 'f': fifo = optarg; break;
        case 'd': dev = optarg; break;
        case 'r': rate = (unsigned)atoi(optarg); break;
        case 'c': ch = atoi(optarg); break;
        case 'g': gain = atoi(optarg); break;
        default:
            fprintf(stderr, "用法: %s [-f fifo] [-d alsa_dev] [-r rate] [-c ch] [-g 增益%%]\n", argv[0]);
            return 2;
        }
    }
    if (gain < 0) gain = 0;
    if (gain > 100) gain = 100;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    mkfifo(fifo, 0666);
    int ffd = open(fifo, O_RDWR | O_NONBLOCK);   /* 自带写端: 永不 EOF */
    if (ffd < 0) {
        LOG("打不开管道 %s: %s", fifo, strerror(errno));
        return 1;
    }

    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        LOG("打开 ALSA %s 失败: %s", dev, snd_strerror(rc));
        return 1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)ch);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    snd_pcm_uframes_t per = 1024;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, 0);
    snd_pcm_uframes_t bufsz = per * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsz);
    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        LOG("设置参数失败: %s", snd_strerror(rc));
        return 1;
    }
    LOG("ALSA 就绪: dev=%s %u Hz %dch S16_LE, period=%lu buffer=%lu, 管道 %s, 软件增益 %d%%",
        dev, rate, ch, (unsigned long)per, (unsigned long)bufsz, fifo, gain);

    int prebuf = (int)per * 2;                  /* 先攒 2 个 period 再开播, 压住抖动 */
    unsigned char pcmbuf[4096 * 4];
    long long frames = 0, silent_frames = 0;
    time_t t_stat = time(NULL);
    int started = 0;

    while (!g_stop) {
        unsigned char tmp[8192];
        ssize_t n;

        /* 尽量把管道里现成的数据搬进环形缓冲 (一次最多 8 KB) */
        while ((n = read(ffd, tmp, sizeof(tmp))) > 0) {
            int nf = (int)(n / 4);
            if (nf > 0)
                ring_put(tmp, nf);
        }

        if (!started) {
            if (r_used < prebuf) {
                struct pollfd pf = { .fd = ffd, .events = POLLIN, .revents = 0 };
                poll(&pf, 1, 20);
                continue;
            }
            started = 1;
        }

        if (r_used == 0) {
            /* 空闲: 补一个 period 的静音, 让设备保持 running (不重开、不爆音) */
            memset(pcmbuf, 0, per * 4);
            rc = snd_pcm_writei(pcm, pcmbuf, per);
            if (rc == -EPIPE) { snd_pcm_prepare(pcm); continue; }
            if (rc < 0) { LOG("写失败: %s", snd_strerror(rc)); break; }
            silent_frames += rc;
            continue;
        }

        int want = r_used < (int)per ? r_used : (int)per;
        ring_get(pcmbuf, want);
        /* 软件增益: 硬件音量被设大也不会吵 (2026-10-29 加)。100% 时完全跳过, 零开销。 */
        if (gain != 100) {
            short *s = (short *)pcmbuf;
            int n = want * (ch > 0 ? ch : 2);
            for (int i = 0; i < n; i++)
                s[i] = (short)((int)s[i] * gain / 100);
        }
        rc = snd_pcm_writei(pcm, pcmbuf, (snd_pcm_uframes_t)want);
        if (rc == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (rc < 0) {
            LOG("写失败: %s", snd_strerror(rc));
            break;
        }
        frames += rc;
        if (rc < want) {                       /* 没写完的把剩余帧退回 */
            r_tail = (r_tail - (want - rc) + RING_FRAMES * 2) % RING_FRAMES;
            r_used += (want - rc);
        }

        if (time(NULL) - t_stat >= 10) {
            LOG("已播 %.1f s (其中静音 %.1f s), 缓冲 %d 帧",
                (double)frames / rate, (double)silent_frames / rate, r_used);
            t_stat = time(NULL);
        }
    }

    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    close(ffd);
    LOG("退出");
    return 0;
}
