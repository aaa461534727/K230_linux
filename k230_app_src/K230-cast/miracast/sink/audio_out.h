#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/* Miracast 音频出口 (实现在 audio_out.c): 写 /run/wfd-audio.pcm, 由 pcmplay 出声 */
int audio_out_open(const char *path);   /* path 可为 NULL (走默认/环境变量) */
void audio_out_close(void);
/* 输入: RTP 里的 LPCM 大端 16bit 立体声 (WFD 规定 LPCM 为 48 kHz/16 bit/2ch) */
void audio_out_pcm_be16(const unsigned char *p, int len);
/* 输入: 已经是小端 16bit 的 PCM (TS 里某些 muxer 直接给主机序), 原样写 */
void audio_out_pcm_le16(const unsigned char *p, int len);
unsigned long long audio_out_bytes(void);
unsigned long long audio_out_dropped(void);

#endif