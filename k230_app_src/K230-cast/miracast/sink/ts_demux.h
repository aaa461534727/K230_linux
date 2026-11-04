/*
 * ts_demux.h — MPEG-2 TS → H.264 Annex-B NAL 解包 + 音频 ES 提取 (Miracast sink 用)
 *
 * 为什么需要: 实测能投屏的参考实现 (海奇板 p2p0 抓包) 里, 手机发的是
 *   RTP payload type 33 (MPEG-TS), 不是裸 H.264 (PT=96)。
 *   所以 sink 必须先把 TS 解成 ES, 再按 NAL 交给下游 (解码/上屏) 复用。
 *
 * 2026-10-27 扩展 (音频): 同一路 TS 里可能有多个 elementary stream
 *   (视频 + 音频)。原来的实现只取第一个 stream_type=0x1B 的 PID, 其余全丢,
 *   于是 LPCM 音频 PID 被无声丢弃 (现场: M4 已谈成 LPCM, 但 audio_pkts=0)。
 *   现在: PMT 里**每条流**都登记 (最多 TS_MAX_ES 条), 视频 PID 走 NAL 回调,
 *   音频 PID 走音频回调 (PES 头已剥掉, 直接是 ES 字节)。
 *   音频到底是什么编码, 由 stats 里的 es[] 表 + 首包 hex 打印回答 —— 没有
 *   真机证据之前不猜。
 *
 * 用法:
 *   ts_demux_t *d = ts_demux_new(on_nal, ctx);
 *   ts_demux_set_audio_cb(d, on_audio, ctx);          // 可选
 *   ts_demux_feed(d, rtp_payload, rtp_payload_len);   // 每次 RTP 负载直接喂
 *   // 回调里收到的是 **不含起始码** 的单个 H.264 NAL
 *   ts_demux_free(d);
 *
 * 特性: 自动找 PAT/PMT → 取第一个 stream_type=0x1B(AVC) 的 PID 当视频;
 *       支持 RTP 负载里跨包切断 (内部 carry 缓冲) + 每 PID 连续性计数校验。
 * 依赖: 无 (纯 C99)。
 */
#ifndef TS_DEMUX_H
#define TS_DEMUX_H

#include <stddef.h>

#define TS_MAX_ES 8

typedef struct ts_demux ts_demux_t;

/* 每解出一个完整 H.264 NAL 调一次; nal 不含 00 00 01 起始码 */
typedef void (*ts_nal_cb)(void *user, const unsigned char *nal, int len);

/* 每收齐一个音频 PES 调一次; es 已去掉 PES 头 (stream_type 见 ts_demux_stats_t) */
typedef void (*ts_audio_cb)(void *user, const unsigned char *es, int len,
                            int stream_type);

typedef struct {
    int pid;
    int stream_type;
    unsigned long pkts;    /* 该 PID 的 TS 包数 (证明它到底有没有在发) */
    unsigned long bytes;   /* 该 PID 的 payload 字节数 */
} ts_es_entry_t;

typedef struct {
    unsigned long ts_pkts;    /* 收到的 188 字节 TS 包 */
    unsigned long pes_pkts;   /* 完整收齐的视频 PES */
    unsigned long nals;       /* 交给回调的 NAL 数 */
    unsigned long bytes;      /* 视频 ES 字节数 */
    unsigned long cc_errors;  /* 连续性计数错误 (丢包证据) */
    int pat_seen, pmt_seen, video_pid, pmt_pid;
    /* ---- 多流探测 (2026-10-27): 音频到底在不在 TS 里 ---- */
    int es_count;                 /* PMT 里登记的流条数 */
    ts_es_entry_t es[TS_MAX_ES];  /* 每条流: type/pid/包数/字节 */
    int audio_pid;                /* 选中的音频 PID (0 = 没有) */
    int audio_stream_type;
    unsigned long audio_pes;      /* 收齐的音频 PES 数 */
    unsigned long audio_bytes;    /* 交给音频回调的 ES 字节数 */
} ts_demux_stats_t;

ts_demux_t *ts_demux_new(ts_nal_cb cb, void *user);
void ts_demux_free(ts_demux_t *d);

/* 注册音频回调 (不调用 = 只解视频, 与旧行为一致) */
void ts_demux_set_audio_cb(ts_demux_t *d, ts_audio_cb cb, void *user);

/* 喂一段 TS 数据 (任意长度, 内部按 188 对齐/重同步) */
void ts_demux_feed(ts_demux_t *d, const unsigned char *data, int len);

/* 会话结束收尾: 吐出最后一个 PES / NAL */
void ts_demux_flush(ts_demux_t *d);

void ts_demux_get_stats(ts_demux_t *d, ts_demux_stats_t *out);

#endif /* TS_DEMUX_H */
