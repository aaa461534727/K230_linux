/* h264_decoder.h — 把解密后的 H.264 Annex-B 交给板载硬解, 解码产物直接进显示管道
 *
 * 复用的现成资产: /usr/bin/v4l2_dec (仓内 K230/src/K230-cast/miracast/tools/v4l2_dec.c,
 *   已在板上跑通 1080p/720p60 + SPS 裁剪快修)。用法: v4l2_dec <in> <out> -w W -h H
 *   ⇒ 它自己吃 FIFO 输入、往 FIFO 输出 NV12。**CarPlay 侧不需要任何 V4L2 代码。**
 *
 * 本模块只做一件事: 起/看护这个子进程, 并把码流写进它的输入 FIFO。
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_H264_DECODER_H
#define CARPLAY_H264_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CP_DEC_BIN "/usr/bin/v4l2_dec"

struct cp_h264_decoder {
    pid_t pid;
    int in_fd;      /* 写端: 喂 Annex-B */
    char in_path[64];
    char out_path[64];
    int w, h;
    uint64_t fed_bytes;
};

/* 起解码器: 建输入 FIFO → fork/exec v4l2_dec → 打开 FIFO 写端。
 * out_path 一般是 /run/wfd-player.fifo (drmplay 的入口)。 */
int cp_decoder_start(struct cp_h264_decoder *d, const char *out_path, int w, int h);

/* 用**外部已存在的输入管道**起解码器 (控制面要往 /run/carplay.h264 写码流时用):
 * 本模块不建 FIFO、不持有写端 (in_fd = -1), 只负责 fork/exec + 看护/重启。
 * w/h 传 0 表示"让解码器从 SPS 自己定型"(推荐)。 */
int cp_decoder_spawn(struct cp_h264_decoder *d, const char *in_path, const char *out_path,
                     int w, int h);

/* 喂码流。返回实际写入字节数 (被截断 = 解码器跟不上)。 */
ssize_t cp_decoder_feed(struct cp_h264_decoder *d, const void *data, size_t len);

/* 解码器是否还活着 (崩了就返回 0, 上层决定是否重启) */
int cp_decoder_alive(struct cp_h264_decoder *d);

void cp_decoder_stop(struct cp_h264_decoder *d);

#endif
