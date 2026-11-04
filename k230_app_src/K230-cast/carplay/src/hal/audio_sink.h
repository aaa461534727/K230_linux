/* audio_sink.h — PCM (S16LE 48k 2ch) → 板载常驻播放器 pcmplay 的 FIFO
 *
 * 与 display_sink 同一思路: 音频设备已被 k230-audio.service 独占 (常驻 pcmplay), 换源不重开设备,
 * 所以空闲无爆音。CarPlay 的 AAC-LC 解码成 PCM 后写 /run/wfd-audio.pcm 即可出声。
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_AUDIO_SINK_H
#define CARPLAY_AUDIO_SINK_H

#include <stddef.h>
#include <stdint.h>

#define CP_AUDIO_DEFAULT_PATH "/run/wfd-audio.pcm"
#define CP_AUDIO_RATE 48000
#define CP_AUDIO_CH 2

struct cp_audio_sink {
    int fd;
    uint64_t bytes;
};

int cp_audio_open(struct cp_audio_sink *s, const char *path, int wait_s);
int cp_audio_write(struct cp_audio_sink *s, const void *pcm, size_t len);
void cp_audio_close(struct cp_audio_sink *s);

#endif
