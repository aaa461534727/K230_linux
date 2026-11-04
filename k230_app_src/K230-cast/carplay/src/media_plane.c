/* media_plane.c — 媒体面常驻进程 (协议面唯一需要对接的东西)
 *
 * 职责 (只做媒体, 不含任何 CarPlay 协议):
 *   ① 保证 /run/carplay.h264 存在 (控制面把解密后的 Annex-B 写进来)
 *   ② 起/看护 /usr/bin/v4l2_dec: /run/carplay.h264 → /run/wfd-player.fifo (NV12, 带 16 B 帧头)
 *   ③ 保证 /run/carplay.touch 存在, 把 FT5x06 的触摸转成 12 字节 HID 报告写进去
 *
 * 为什么拆成独立进程:
 *   解码器是"重、会崩、会卡死硬件"的东西; 协议面 (Rust) 只管控制与解密。
 *   协议进程崩了不该把 MVX 硬件一起带走, 反之亦然 —— 与板上既有的
 *   "k230-screen 独占播放器 / 其它人只往管道写" 是同一套纪律。
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal/h264_decoder.h"
#include "hal/touch_evdev.h"
#include "hal/display_sink.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CP_H264_FIFO  "/run/carplay.h264"
#define CP_TOUCH_FIFO "/run/carplay.touch"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int mkfifo_if_needed(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            LOGE("%s 存在但不是管道", p);
            return -1;
        }
        return 0;
    }
    if (mkfifo(p, 0666) != 0) {
        LOGE("mkfifo %s: %s", p, strerror(errno));
        return -1;
    }
    chmod(p, 0666);
    return 0;
}

static int touch_fd = -1;

static void touch_publish(const uint8_t rep[CP_HID_REPORT_LEN], void *user)
{
    (void)user;
    if (touch_fd < 0)
        return;
    ssize_t n = write(touch_fd, rep, CP_HID_REPORT_LEN);
    if (n != CP_HID_REPORT_LEN && errno != EAGAIN && errno != EPIPE)
        LOGE("写触摸管道失败: %s", strerror(errno));
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (mkfifo_if_needed(CP_H264_FIFO) != 0 || mkfifo_if_needed(CP_TOUCH_FIFO) != 0)
        return 1;

    /* 触摸管道: 非阻塞写, 没人读就丢 (触摸丢了不该拖累解码) */
    touch_fd = open(CP_TOUCH_FIFO, O_WRONLY | O_NONBLOCK);
    if (touch_fd < 0 && errno == ENXIO)
        LOGI("触摸管道暂无读者 (%s), 等控制面接上", CP_TOUCH_FIFO);

    struct cp_touch touch;
    int have_touch = cp_touch_open(&touch, CP_TOUCH_DEFAULT_DEV,
                                   CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H, 1,
                                   touch_publish, NULL) == 0;

    struct cp_h264_decoder dec;
    memset(&dec, 0, sizeof(dec));
    dec.pid = -1;
    int need_start = 1;
    int64_t last_start = 0;

    LOGI("媒体面就绪: 码流 %s → 硬解 → %s; 触摸 %s → %s",
         CP_H264_FIFO, CP_SINK_DEFAULT_PATH, CP_TOUCH_DEFAULT_DEV, CP_TOUCH_FIFO);

    while (!g_stop) {
        if (need_start && now_ms() - last_start > 500) {
            /* 解码器自己会阻塞等 /run/carplay.h264 的写端 (控制面), 不需要我们先开 */
            if (cp_decoder_spawn(&dec, CP_H264_FIFO, CP_SINK_DEFAULT_PATH, 0, 0) == 0) {
                need_start = 0;
                last_start = now_ms();
            }
        } else if (!need_start && !cp_decoder_alive(&dec)) {
            LOGE("硬解进程没了, 1 s 后重启 (两次常驻解码器是上限, 不许再叠第三个)");
            need_start = 1;
            last_start = now_ms();
            sleep(1);
        }

        if (have_touch) {
            if (cp_touch_poll(&touch, 100) < 0) {
                LOGE("触摸设备掉了, 尝试重开");
                cp_touch_close(&touch);
                sleep(1);
                have_touch = cp_touch_open(&touch, CP_TOUCH_DEFAULT_DEV,
                                           CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H, 1,
                                           touch_publish, NULL) == 0;
            }
        } else {
            sleep(1);
            have_touch = cp_touch_open(&touch, CP_TOUCH_DEFAULT_DEV,
                                       CP_SINK_DEFAULT_W, CP_SINK_DEFAULT_H, 1,
                                       touch_publish, NULL) == 0;
        }

        /* 触摸管道读者换了 (控制面重启) 就重开写端 */
        if (touch_fd < 0)
            touch_fd = open(CP_TOUCH_FIFO, O_WRONLY | O_NONBLOCK);
    }

    cp_decoder_stop(&dec);
    if (have_touch)
        cp_touch_close(&touch);
    if (touch_fd >= 0)
        close(touch_fd);
    LOGI("媒体面退出");
    return 0;
}
