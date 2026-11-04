/*
 * v4l2_probe.c — 探测 mem2mem 编解码设备支持哪些格式 (只读, 不改任何配置)
 *
 * 用途: K230 的 /dev/video0 (mvx, 平台 90400000.vpu) 是 V4L2 M2M 设备,
 *       ffmpeg 的 `-list_formats` 会以 "Not a video capture device" 拒绝,
 *       所以用这个裸 ioctl 小工具把 OUTPUT(码流口) 与 CAPTURE(图像口)
 *       两个队列的格式族列出来, 判断"该驱动到底支不支持 H.264 解码"。
 *
 * 编译 (板上直接编):  gcc -O2 -o v4l2_probe v4l2_probe.c
 * 用法:               ./v4l2_probe [/dev/video0]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void fourcc_str(unsigned int f, char out[5])
{
    out[0] = (char)(f & 0x7f);
    out[1] = (char)((f >> 8) & 0x7f);
    out[2] = (char)((f >> 16) & 0x7f);
    out[3] = (char)((f >> 24) & 0x7f);
    out[4] = 0;
}

static void enum_fmt(int fd, unsigned int type, const char *label)
{
    struct v4l2_fmtdesc d;
    int n = 0;

    printf("== %s (type=%u)\n", label, type);
    for (unsigned int i = 0; ; i++) {
        memset(&d, 0, sizeof(d));
        d.index = i;
        d.type = type;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) {
            if (i == 0)
                printf("   (不支持: %s)\n", strerror(errno));
            break;
        }
        char cc[5];
        fourcc_str(d.pixelformat, cc);
        printf("   %2u) %-4s  flags=0x%08x  %s\n", i, cc, d.flags, d.description);
        n++;
    }
    if (n)
        printf("   共 %d 种\n", n);
}

static void try_fmt(int fd, unsigned int type, unsigned int pixfmt, int w, int h)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = type;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        printf("   S_FMT %dx%d fourcc=0x%08x → 失败: %s\n", w, h, pixfmt, strerror(errno));
        return;
    }
    char cc[5];
    fourcc_str(f.fmt.pix_mp.pixelformat, cc);
    printf("   S_FMT %dx%d → 实际 %ux%u %s planes=%u sizeimage=[%u,%u,%u]\n",
           w, h, f.fmt.pix_mp.width, f.fmt.pix_mp.height, cc,
           f.fmt.pix_mp.num_planes,
           f.fmt.pix_mp.plane_fmt[0].sizeimage,
           f.fmt.pix_mp.plane_fmt[1].sizeimage,
           f.fmt.pix_mp.plane_fmt[2].sizeimage);
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        printf("driver=%s card=%s bus=%s caps=0x%08x\n\n",
               cap.driver, cap.card, cap.bus_info, cap.capabilities);

    enum_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT_MPLANE (输入=码流口)");
    enum_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "OUTPUT (单平面)");
    enum_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE_MPLANE (输出=图像口)");
    enum_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "CAPTURE (单平面)");

    printf("\n-- 试设格式 (解码方向: 码流 H264 -> 图像 NV12) --\n");
    try_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_H264, 640, 360);
    try_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_NV12, 640, 360);

    printf("\n-- 试设格式 (编码方向: 图像 NV12 -> 码流 H264) --\n");
    try_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_NV12, 640, 360);
    try_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_PIX_FMT_H264, 640, 360);

    close(fd);
    return 0;
}
