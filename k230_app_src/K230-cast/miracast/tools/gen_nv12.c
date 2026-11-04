/* gen_nv12: 往 drmplay 的 fifo 里按 60fps 灌 NV12 帧 (16B 头 + YUV420), 每帧内容不同 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/run/wfd-player.fifo";
    int fps = argc > 2 ? atoi(argv[2]) : 60;
    int nframes = argc > 3 ? atoi(argv[3]) : 600;
    int w = argc > 4 ? atoi(argv[4]) : 480;
    int h = argc > 5 ? atoi(argv[5]) : 800;
    /* 2026-10-27: 抖动模拟 —— 真实 Miracast 源会周期性「少发一拍、之后连发追平」
     * (板上日志: 帧间隔 avg 18~19.2 ms, 5 s 只涨 123 帧然后一簇 477 帧)。
     * argv[6] = 抖动帧占比 (%), 0 = 精确节拍。用来 A/B 管道容量扛不扛得住抖动。 */
    int jit_pct = argc > 6 ? atoi(argv[6]) : 0;
    if (argc > 7) srand(atoi(argv[7]));
    size_t ylen = (size_t)w * h, uvlen = ylen / 2, len = ylen + uvlen;
    unsigned char *buf = malloc(len);
    if (!buf) return 1;
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror("open fifo"); return 1; }
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    long long period = 1000000000LL / fps;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < nframes; f++) {
        /* 每帧移动的竖条: 内容 100% 不同 */
        int bar = (f * 7) % w;
        for (int y = 0; y < h; y++) {
            unsigned char *row = buf + (size_t)y * w;
            int yy = (y * 255) / h;
            for (int x = 0; x < w; x++) {
                int dx = x - bar; if (dx < 0) dx = -dx;
                int v = ((dx < 40) ? 235 : 16) + yy / 8;
                row[x] = (unsigned char)(v > 250 ? 250 : v);
            }
        }
        memset(buf + ylen, 128, uvlen);
        unsigned char hd[16];
        memcpy(hd, "NV12", 4);
        hd[4] = w & 0xff; hd[5] = (w >> 8) & 0xff;
        hd[6] = h & 0xff; hd[7] = (h >> 8) & 0xff;
        uint32_t l32 = (uint32_t)len;
        hd[8] = l32 & 0xff; hd[9] = (l32 >> 8) & 0xff; hd[10] = (l32 >> 16) & 0xff; hd[11] = (l32 >> 24) & 0xff;
        struct timespec ns; clock_gettime(CLOCK_MONOTONIC, &ns);
        uint32_t ms = (uint32_t)(ns.tv_sec * 1000u + ns.tv_nsec / 1000000);
        hd[12] = ms & 0xff; hd[13] = (ms >> 8) & 0xff; hd[14] = (ms >> 16) & 0xff; hd[15] = (ms >> 24) & 0xff;
        if (write(fd, hd, 16) != 16) break;
        size_t off = 0;
        while (off < len) {
            ssize_t r = write(fd, buf + off, len - off);
            if (r < 0) { if (errno == EINTR) continue; goto out; }
            off += (size_t)r;
        }
        next.tv_nsec += period;
        if (jit_pct > 0 && (rand() % 100) < jit_pct)
            next.tv_nsec += period;   /* 抖动: 这一拍空出来 (间隔变 2 拍), 之后自动连发追平 */
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) ;
    }
out:
    { struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
      double sec = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
      fprintf(stderr, "gen: %d 帧 %.2f s = %.1f fps\n", nframes, sec, sec>0?nframes/sec:0); }
    close(fd);
    return 0;
}
