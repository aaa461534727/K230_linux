/* display_sink.h — NV12 帧 → 板载常驻播放器 drmplay 的 FIFO
 *
 * 为什么这么做 (而不是自己 open /dev/dri):
 *   K230 上屏已有唯一所有者 k230-screen.service (drmplay -F /run/wfd-player.fifo -p nv12 -w 480 -h 800),
 *   Miracast / AirPlay 也都是"往管道里写 NV12"。CarPlay 走同一条路 ⇒ 上屏代码零新增,
 *   并且天然继承了"谁都不许杀播放器 / 空闲黑屏不出紫"的那套现场教训 (见 common/k230-screen.sh 头注释)。
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_DISPLAY_SINK_H
#define CARPLAY_DISPLAY_SINK_H

#include <stddef.h>
#include <stdint.h>

#define CP_SINK_DEFAULT_PATH "/run/wfd-player.fifo"
#define CP_SINK_DEFAULT_W 480
#define CP_SINK_DEFAULT_H 800

struct cp_display_sink {
    int fd;
    int w, h;
    size_t frame_bytes;
    uint64_t frames;
    uint64_t frame_drops;
    uint64_t bytes;
    int64_t t_first_ns;
    int64_t t_last_ns;
};

/* 打开管道 (写端)。wait_s 秒内等不到读者就失败 (drmplay 没起 / 拿着旧 inode 的经典坑)。 */
int cp_display_open(struct cp_display_sink *s, const char *path, int w, int h, int wait_s);

/* 写一帧 NV12 (必须恰好 w*h*3/2 字节)。返回 0 成功; -1 失败 (调用方可继续, 但要计 drop)。 */
int cp_display_write(struct cp_display_sink *s, const void *nv12, size_t len);

/* 累计帧率 (自上次调用以来) 与总帧数 */
double cp_display_fps(const struct cp_display_sink *s, uint64_t *total);

void cp_display_close(struct cp_display_sink *s);

#endif
