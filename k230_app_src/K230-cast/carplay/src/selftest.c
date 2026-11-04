/* selftest.c — carplay HAL 冒烟测试 (板端跑, 不需要蓝牙/不需要手机)
 *
 * 子命令:
 *   screen [秒]            往显示管道推彩条+移动方块, 验证「上屏这条路」通
 *   touch  [秒]            读触摸并打印 12 字节 HID 报告, 验证「回控这条路」通
 *   audio  [秒]            播 440 Hz 正弦 (S16LE 48k 2ch), 验证「出声这条路」通
 *   decode <file.h264>     把 H.264 文件喂给硬解 → 显示管道, 验证「视频链路」通
 *   all    [秒]            screen + touch 并行 (默认 8 s)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal/audio_sink.h"
#include "hal/display_sink.h"
#include "hal/h264_decoder.h"
#include "hal/touch_evdev.h"
#include "log.h"

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* 彩条 + 移动方块。Y 平面 + NV12 UV (128,128 = 灰度)。 */
static void paint(uint8_t *buf, int w, int h, int phase)
{
    static const uint8_t bars[8] = { 235, 210, 170, 145, 110, 80, 50, 20 };
    uint8_t *y = buf;
    uint8_t *uv = buf + (size_t)w * h;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int idx = (col * 8) / w;
            if (idx > 7) idx = 7;
            y[(size_t)row * w + col] = bars[idx];
        }
    }
    /* 顶部一条黑, 底部一条白: 一眼看出是竖屏还是被拉伸 */
    memset(y, 16, (size_t)w * 24);
    memset(y + (size_t)(h - 24) * w, 235, (size_t)w * 24);

    int sq = 80;
    int sx = (phase * 6) % (w - sq);
    int sy = (h - sq) / 2;
    for (int r = 0; r < sq; r++)
        memset(y + (size_t)(sy + r) * w + sx, 255, (size_t)sq);
    memset(uv, 128, (size_t)w * h / 2);
}

static int run_screen(int seconds)
{
    struct cp_display_sink s;
    if (cp_display_open(&s, CP_SINK_DEFAULT_PATH, CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H, 5) != 0)
        return 1;

    size_t len = s.frame_bytes;
    uint8_t *frame = malloc(len);
    if (!frame) return 1;

    const int fps = 30;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int frames = 0;
    for (int i = 0; i < seconds * fps && !g_stop; i++) {
        paint(frame, s.w, s.h, i);
        if (cp_display_write(&s, frame, len) == 0)
            frames++;
        next.tv_nsec += 1000000000L / fps;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    free(frame);
    LOGI("screen 结果: 写入 %d 帧, 累计 %llu 帧, 平均 %.1f fps",
         frames, (unsigned long long)s.frames, cp_display_fps(&s, NULL));
    cp_display_close(&s);
    return frames > 0 ? 0 : 1;
}

static int run_audio(int seconds)
{
    struct cp_audio_sink s;
    if (cp_audio_open(&s, CP_AUDIO_DEFAULT_PATH, 5) != 0)
        return 1;
    const int rate = CP_AUDIO_RATE, chunk = 480;
    int16_t buf[480 * 2];
    double phase = 0;
    for (int n = 0; n < rate / chunk * seconds && !g_stop; n++) {
        for (int i = 0; i < chunk; i++) {
            int16_t v = (int16_t)(sin(phase) * 6000);
            phase += 2 * M_PI * 440 / rate;
            if (phase > 2 * M_PI) phase -= 2 * M_PI;
            buf[i * 2] = v;
            buf[i * 2 + 1] = v;
        }
        cp_audio_write(&s, buf, sizeof(buf));
    }
    LOGI("audio 结果: 写了 %llu 字节 PCM", (unsigned long long)s.bytes);
    cp_audio_close(&s);
    return 0;
}

static void touch_cb(const uint8_t rep[CP_HID_REPORT_LEN], void *user)
{
    (void)user;
    LOGI("HID %02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x | "
         "f0 id=%u down=%u (%u,%u)  f1 id=%u down=%u (%u,%u)",
         rep[0], rep[1], rep[2], rep[3], rep[4], rep[5],
         rep[6], rep[7], rep[8], rep[9], rep[10], rep[11],
         rep[0], rep[1], rep[2] | (rep[3] << 8), rep[4] | (rep[5] << 8),
         rep[6], rep[7], rep[8] | (rep[9] << 8), rep[10] | (rep[11] << 8));
}

static int run_touch(int seconds, int grab)
{
    struct cp_touch t;
    if (cp_touch_open(&t, CP_TOUCH_DEFAULT_DEV, CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H,
                      grab, touch_cb, NULL) != 0)
        return 1;
    int loops = seconds * 20;
    for (int i = 0; i < loops && !g_stop; i++)
        cp_touch_poll(&t, 50);
    LOGI("touch 结果: 收到 %llu 个 HID 报告", (unsigned long long)t.events);
    cp_touch_close(&t);
    return 0;
}

static int run_decode(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { LOGE("打不开 %s", path); return 1; }
    struct cp_h264_decoder d;
    if (cp_decoder_start(&d, CP_SINK_DEFAULT_PATH, CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H) != 0) {
        fclose(f);
        return 1;
    }
    static uint8_t buf[64 * 1024];
    size_t n;
    uint64_t total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ssize_t w = cp_decoder_feed(&d, buf, n);
        if (w < 0) break;
        total += (uint64_t)w;
    }
    fclose(f);
    LOGI("decode 结果: 喂入 %llu 字节, 等 3 s 让解码器收尾", (unsigned long long)total);
    sleep(3);
    int alive = cp_decoder_alive(&d);
    cp_decoder_stop(&d);
    return alive ? 0 : 1;
}

static int run_all(int seconds)
{
    struct cp_touch t;
    int have_touch = cp_touch_open(&t, CP_TOUCH_DEFAULT_DEV, CP_SINK_DEFAULT_W,
                                   CP_SINK_DEFAULT_H, 1, touch_cb, NULL) == 0;
    struct cp_display_sink s;
    if (cp_display_open(&s, CP_SINK_DEFAULT_PATH, CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H, 5) != 0) {
        if (have_touch) cp_touch_close(&t);
        return 1;
    }
    uint8_t *frame = malloc(s.frame_bytes);
    if (!frame) return 1;
    const int fps = 30;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int i = 0; i < seconds * fps && !g_stop; i++) {
        paint(frame, s.w, s.h, i);
        cp_display_write(&s, frame, s.frame_bytes);
        if (have_touch)
            cp_touch_poll(&t, 1);
        next.tv_nsec += 1000000000L / fps;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    LOGI("all 结果: 显示 %llu 帧 / 触摸 %llu 报告", (unsigned long long)s.frames,
         (unsigned long long)t.events);
    free(frame);
    cp_display_close(&s);
    if (have_touch) cp_touch_close(&t);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const char *cmd = argc > 1 ? argv[1] : "all";
    if (!strcmp(cmd, "screen"))       return run_screen(argc > 2 ? atoi(argv[2]) : 5);
    if (!strcmp(cmd, "touch"))        return run_touch(argc > 2 ? atoi(argv[2]) : 10,
                                                       argc > 3 ? atoi(argv[3]) : 0);
    if (!strcmp(cmd, "audio"))        return run_audio(argc > 2 ? atoi(argv[2]) : 2);
    if (!strcmp(cmd, "decode"))       return argc > 2 ? run_decode(argv[2]) : 1;
    if (!strcmp(cmd, "all"))          return run_all(argc > 2 ? atoi(argv[2]) : 8);

    fprintf(stderr, "用法: %s {screen|touch|audio|decode <file.h264>|all} [秒]\n", argv[0]);
    return 2;
}
