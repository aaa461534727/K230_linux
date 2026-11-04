/*
 * ts_demux.c — MPEG-2 TS → H.264 Annex-B NAL + 音频 ES 解包
 *
 * 参考证据: K230/captures/p2p0.pcap (手机能投屏到海奇板的那一场)
 *   RTP payload type = 33 (MPEG-TS), 225 帧 / 6.8 s, 内层 TS 里有 PAT/PMT。
 *   本文件把 TS 还原成 ES, 再按 00 00 01 切成 NAL 交给上层。
 *
 * 只处理"能投屏"所必需的最小集:
 *   PAT(PID 0) → 第一个 program 的 PMT PID
 *   PMT        → 视频 = 第一个 stream_type=0x1B (H.264/AVC) 的 PID
 *                音频 = 第一个"像音频"的 stream_type (见 is_audio_type)
 *   每个 PID   → PES 重组 → ES → 视频按起始码切 NAL / 音频整段交回调
 *
 * 2026-10-27: 原实现只认视频 PID, 音频 PID 全丢 (现场 audio_pkts=0 的可能原因之一)。
 * 现在 PMT 里每一条流都登记进 stats.es[], 并在 stderr 打一行 "PMT 流清单",
 * 这样一眼就能回答"手机到底有没有把音频塞进 TS"。
 */
#include "ts_demux.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TS_PKT      188
#define ES_CAP      (4 * 1024 * 1024)   /* 单个 NAL 最大跨度 (关键帧可能很大) */
#define PES_CAP     (2 * 1024 * 1024)

/* 一条 elementary stream 的上下文 (视频/音频各一份 PES 重组缓冲) */
typedef struct {
    int used;
    int pid;
    int stream_type;
    int is_video;

    unsigned char *pes;
    int pes_len, pes_cap;

    int last_cc;          /* 本 PID 的连续性计数, -1 = 未知 */
    int have_cc;
} es_ctx_t;

struct ts_demux {
    ts_nal_cb cb;
    void *user;
    ts_audio_cb acb;
    void *auser;

    unsigned char carry[TS_PKT];
    int carry_len;

    unsigned char *es;
    int es_len, es_cap;

    es_ctx_t pids[TS_MAX_ES];

    int audio_probe_done;   /* 音频首包 hex 只打一次 */
    int pmt_logged;         /* PMT 流清单只打一次 */

    ts_demux_stats_t st;
};

/* ---------------------------------------------------------------- 小工具 */

static int buf_reserve(unsigned char **buf, int *cap, int need, int max_cap)
{
    if (need <= *cap)
        return 0;
    int n = *cap ? *cap : 64 * 1024;
    while (n < need && n < max_cap)
        n *= 2;
    if (n > max_cap)
        n = max_cap;
    if (n < need)
        return -1;
    unsigned char *p = realloc(*buf, (size_t)n);
    if (!p)
        return -1;
    *buf = p;
    *cap = n;
    return 0;
}

/* 从 from 开始找下一个起始码 00 00 01, 返回索引; -1 = 没找到 */
static int find_start(const unsigned char *b, int len, int from)
{
    for (int i = from; i + 3 <= len; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
    }
    return -1;
}

/* 从 ES 缓冲里切出所有**完整**的 NAL 并回调, 尾巴 (未收全的 NAL) 保留
 *
 * 关键: 尾巴必须**从下一个 NAL 的起始码开始保留**。早期实现保留的是
 * "起始码之后"的字节, 下次再切时找不到起始码 → 那个跨 PES 的大 NAL
 * (I 帧 slice) 被整段丢弃, 现象是只剩 AUD/SPS/PPS 而 ffprobe 报
 * "missing picture in access unit" (2026-10-21 自测踩到)。 */
static void es_emit(ts_demux_t *d, int final)
{
    if (d->es_len < 4)
        return;

    int cur = find_start(d->es, d->es_len, 0);
    if (cur < 0) {
        if (final)
            d->es_len = 0;
        return;
    }

    while (1) {
        int nxt = find_start(d->es, d->es_len, cur + 3);
        if (nxt < 0)
            break;                       /* 当前 NAL 还没收全 */
        int len = nxt - (cur + 3);
        if (len > 0 && d->cb) {
            d->st.nals++;
            d->cb(d->user, d->es + cur + 3, len);
        }
        cur = nxt;
    }

    if (final) {
        int len = d->es_len - (cur + 3);
        if (len > 0 && d->cb) {
            d->st.nals++;
            d->cb(d->user, d->es + cur + 3, len);
        }
        d->es_len = 0;
        return;
    }

    /* 保留完整 NAL 的起始码 + 未收全的内容 */
    if (cur > 0) {
        memmove(d->es, d->es + cur, (size_t)(d->es_len - cur));
        d->es_len -= cur;
    }
}

static void es_append(ts_demux_t *d, const unsigned char *p, int len)
{
    if (len <= 0)
        return;
    if (buf_reserve(&d->es, &d->es_cap, d->es_len + len, ES_CAP) < 0) {
        /* 太大 → 丢掉陈旧数据重新开始, 避免卡死 */
        d->es_len = 0;
        if (buf_reserve(&d->es, &d->es_cap, len, ES_CAP) < 0)
            return;
    }
    memcpy(d->es + d->es_len, p, (size_t)len);
    d->es_len += len;
    d->st.bytes += (unsigned long)len;
    es_emit(d, 0);
}

static const char *audio_type_name(int t)
{
    switch (t) {
    case 0x03: return "MPEG-1 audio";
    case 0x04: return "MPEG-2 audio";
    case 0x0f: return "AAC ADTS";
    case 0x11: return "AAC LATM";
    case 0x81: return "AC-3";
    case 0x87: return "E-AC-3";
    case 0x80: return "LPCM(0x80)";
    case 0x83: return "LPCM(0x83)";
    case 0x84: return "LPCM(0x84)";
    case 0x85: return "LPCM(0x85)";
    case 0x86: return "LPCM(0x86)";
    case 0x06: return "private(0x06)";
    default:   return "?";
    }
}

/* 音频 PES 收齐: 剥掉 PES 头 (必要时再剥 private-stream 子头) 后交回调 */
static void audio_emit(ts_demux_t *d, es_ctx_t *e)
{
    const unsigned char *p = e->pes;
    int len = e->pes_len;
    e->pes_len = 0;

    if (len <= 0)
        return;

    int off = -1, sid = -1;
    if (len >= 9 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        sid = p[3];
        off = 9 + p[8];                  /* 9 = start code(3)+id(1)+len(2)+flags(2)+hdrlen(1) */
    } else {
        for (int i = 0; i + 3 < len; i++) {
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
                sid = p[i + 3];
                off = i + 9 + p[i + 8];
                break;
            }
        }
    }
    if (off <= 0 || off >= len)
        return;

    const unsigned char *a = p + off;
    int alen = len - off;

    /* private_stream_1 (0xBD) 里的 LPCM 子流: 首字节是 substream id 0xA0..0xAF,
     * 后面跟 3 字节子头 (帧数 / 首个访问单元指针)。只剥这一层, 不做别的猜测。 */
    if (sid == 0xbd && alen >= 4 && a[0] >= 0xa0 && a[0] <= 0xaf) {
        a += 4;
        alen -= 4;
    }

    if (!d->audio_probe_done) {
        d->audio_probe_done = 1;
        fprintf(stderr, "[TS] 音频首包: stream_type=0x%02x (%s) pid=%d "
                        "PES_stream_id=0x%02x ES=%d 字节, 前 16 字节 =",
                e->stream_type, audio_type_name(e->stream_type), e->pid,
                (unsigned)(sid & 0xff), alen);
        for (int i = 0; i < 16 && i < alen; i++)
            fprintf(stderr, " %02x", a[i]);
        fprintf(stderr, "\n");
    }

    d->st.audio_pes++;
    d->st.audio_bytes += (unsigned long)alen;
    if (d->acb && alen > 0)
        d->acb(d->auser, a, alen, e->stream_type);
}

/* 一个 PES 收齐 (视频: 追加到 ES; 音频: 交音频回调) */
static void pes_flush(ts_demux_t *d, es_ctx_t *e)
{
    if (e->pes_len <= 0) {
        e->pes_len = 0;
        return;
    }
    if (!e->is_video) {
        audio_emit(d, e);
        return;
    }

    const unsigned char *p = e->pes;
    int len = e->pes_len;
    e->pes_len = 0;

    int off = -1;
    if (len >= 9 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        off = 9 + p[8];                  /* 9 = start code(3)+id(1)+len(2)+flags(2)+hdrlen(1) */
    } else {
        for (int i = 0; i + 3 < len; i++) {
            if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
                off = i + 9 + p[i + 8];
                break;
            }
        }
    }
    if (off > 0 && off < len) {
        d->st.pes_pkts++;
        es_append(d, p + off, len - off);
    }
}

/* ------------------------------------------------------------ PSI 解析 */

static void parse_pat(ts_demux_t *d, const unsigned char *payload, int plen, int pusi)
{
    int off = 0;
    if (pusi) {
        if (plen < 1)
            return;
        off = 1 + payload[0];            /* pointer_field */
    }
    if (plen - off < 12)
        return;
    const unsigned char *s = payload + off;
    if (s[0] != 0x00)                    /* table_id: PAT */
        return;
    int sec_len = ((s[1] & 0x0f) << 8) | s[2];
    int end = 3 + sec_len - 4;           /* 去掉 CRC32 */
    if (end > plen - off)
        end = plen - off;
    for (int i = 8; i + 4 <= end; i += 4) {
        int prog = (s[i] << 8) | s[i + 1];
        int pid = ((s[i + 2] & 0x1f) << 8) | s[i + 3];
        if (prog != 0) {                 /* 第一个非 NIT 的节目 */
            d->st.pmt_pid = pid;
            d->st.pat_seen = 1;
            return;
        }
    }
}

/* 这条 stream_type 是不是音频 (用于选音频 PID; 0x06 私有流也算候选) */
static int is_audio_type(int t)
{
    switch (t) {
    case 0x03: case 0x04:                /* MPEG audio */
    case 0x0f: case 0x11:                /* AAC ADTS / LATM */
    case 0x81: case 0x87:                /* AC-3 / E-AC-3 */
    case 0x80: case 0x83: case 0x84: case 0x85: case 0x86:  /* LPCM 家族 */
    case 0x06:                           /* private data (可能是 LPCM) */
        return 1;
    default:
        return 0;
    }
}

static es_ctx_t *es_slot(ts_demux_t *d, int pid, int stream_type, int is_video)
{
    for (int i = 0; i < TS_MAX_ES; i++) {
        if (d->pids[i].used && d->pids[i].pid == pid)
            return &d->pids[i];
        if (!d->pids[i].used) {
            es_ctx_t *e = &d->pids[i];
            memset(e, 0, sizeof(*e));
            e->used = 1;
            e->pid = pid;
            e->stream_type = stream_type;
            e->is_video = is_video;
            e->last_cc = -1;
            return e;
        }
    }
    return NULL;
}

static void parse_pmt(ts_demux_t *d, const unsigned char *payload, int plen, int pusi)
{
    int off = 0;
    if (pusi) {
        if (plen < 1)
            return;
        off = 1 + payload[0];
    }
    if (plen - off < 16)
        return;
    const unsigned char *s = payload + off;
    if (s[0] != 0x02)                    /* table_id: PMT */
        return;
    int sec_len = ((s[1] & 0x0f) << 8) | s[2];
    int end = 3 + sec_len - 4;
    if (end > plen - off)
        end = plen - off;
    int prog_info_len = ((s[10] & 0x0f) << 8) | s[11];
    int i = 12 + prog_info_len;
    while (i + 5 <= end) {
        int stype = s[i];
        int epid = ((s[i + 1] & 0x1f) << 8) | s[i + 2];
        int es_info_len = ((s[i + 3] & 0x0f) << 8) | s[i + 4];

        /* 登记到 stats.es[] (给现场日志看 "手机到底发了哪几路流") */
        int found = 0;
        for (int k = 0; k < d->st.es_count; k++) {
            if (d->st.es[k].pid == epid) {
                d->st.es[k].stream_type = stype;
                found = 1;
                break;
            }
        }
        if (!found && d->st.es_count < TS_MAX_ES) {
            ts_es_entry_t *en = &d->st.es[d->st.es_count++];
            en->pid = epid;
            en->stream_type = stype;
            en->pkts = 0;
            en->bytes = 0;
        }

        if (stype == 0x1b && d->st.video_pid == 0) {   /* 0x1B = H.264/AVC */
            d->st.video_pid = epid;
            d->st.pmt_seen = 1;
            es_slot(d, epid, stype, 1);
        } else if (is_audio_type(stype) && d->st.audio_pid == 0) {
            d->st.audio_pid = epid;
            d->st.audio_stream_type = stype;
            es_slot(d, epid, stype, 0);
        }
        i += 5 + es_info_len;
    }

    if (!d->pmt_logged && d->st.es_count > 0) {
        d->pmt_logged = 1;
        fprintf(stderr, "[TS] PMT: %d 条流", d->st.es_count);
        for (int k = 0; k < d->st.es_count; k++)
            fprintf(stderr, " | type=0x%02x(%s) pid=%d",
                    d->st.es[k].stream_type,
                    d->st.es[k].stream_type == 0x1b ? "H.264" :
                    audio_type_name(d->st.es[k].stream_type),
                    d->st.es[k].pid);
        fprintf(stderr, " ⇒ video_pid=%d audio_pid=%d\n",
                d->st.video_pid, d->st.audio_pid);
    }
}

/* ------------------------------------------------------------ 单个 TS 包 */

static void handle_packet(ts_demux_t *d, const unsigned char *p)
{
    int pid = ((p[1] & 0x1f) << 8) | p[2];
    int pusi = (p[1] & 0x40) != 0;
    int afc = (p[3] >> 4) & 0x03;
    int cc = p[3] & 0x0f;
    int off = 4;

    if (afc & 0x02) {                    /* adaptation field */
        int alen = p[4];
        off = 5 + alen;
    }
    if (!(afc & 0x01) || off >= TS_PKT)  /* 无 payload */
        return;
    const unsigned char *pl = p + off;
    int plen = TS_PKT - off;

    d->st.ts_pkts++;

    if (pid == 0) {
        parse_pat(d, pl, plen, pusi);
        return;
    }
    if (pid == d->st.pmt_pid && d->st.pmt_pid) {
        parse_pmt(d, pl, plen, pusi);
        return;
    }

    es_ctx_t *e = NULL;
    for (int i = 0; i < TS_MAX_ES; i++) {
        if (d->pids[i].used && d->pids[i].pid == pid) {
            e = &d->pids[i];
            break;
        }
    }
    if (!e)
        return;

    /* 每 PID 计数 (证明"音频 PID 在发/不在发") */
    for (int k = 0; k < d->st.es_count; k++) {
        if (d->st.es[k].pid == pid) {
            d->st.es[k].pkts++;
            d->st.es[k].bytes += (unsigned long)plen;
            break;
        }
    }

    /* 连续性计数: 丢包会跳号 (= 上游丢包/乱序证据) */
    if (e->have_cc && cc != ((e->last_cc + 1) & 0x0f))
        d->st.cc_errors++;
    e->last_cc = cc;
    e->have_cc = 1;

    if (pusi)
        pes_flush(d, e);
    if (buf_reserve(&e->pes, &e->pes_cap, e->pes_len + plen, PES_CAP) == 0) {
        memcpy(e->pes + e->pes_len, pl, (size_t)plen);
        e->pes_len += plen;
    } else {
        e->pes_len = 0;                  /* PES 太大 (异常) → 丢掉重来 */
    }
}

/* ---------------------------------------------------------------- 对外 API */

ts_demux_t *ts_demux_new(ts_nal_cb cb, void *user)
{
    ts_demux_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->cb = cb;
    d->user = user;
    return d;
}

void ts_demux_set_audio_cb(ts_demux_t *d, ts_audio_cb cb, void *user)
{
    if (!d)
        return;
    d->acb = cb;
    d->auser = user;
}

void ts_demux_free(ts_demux_t *d)
{
    if (!d)
        return;
    for (int i = 0; i < TS_MAX_ES; i++)
        free(d->pids[i].pes);
    free(d->es);
    free(d);
}

void ts_demux_feed(ts_demux_t *d, const unsigned char *data, int len)
{
    if (!d || !data || len <= 0)
        return;

    int i = 0;
    /* 先把上次剩下的半包补满 */
    while (d->carry_len > 0 && d->carry_len < TS_PKT && i < len) {
        d->carry[d->carry_len++] = data[i++];
        if (d->carry_len == TS_PKT) {
            if (d->carry[0] == 0x47)
                handle_packet(d, d->carry);
            d->carry_len = 0;            /* 失同步 (非 0x47) 也直接丢弃重找 */
        }
    }

    while (i + TS_PKT <= len) {
        const unsigned char *p = data + i;
        if (p[0] != 0x47) {              /* 重同步: 逐字节找 0x47 */
            i++;
            continue;
        }
        handle_packet(d, p);
        i += TS_PKT;
    }

    /* 尾巴留到下次 */
    int left = len - i;
    if (left > 0 && left < TS_PKT) {
        memcpy(d->carry, data + i, (size_t)left);
        d->carry_len = left;
    }
}

void ts_demux_flush(ts_demux_t *d)
{
    if (!d)
        return;
    for (int i = 0; i < TS_MAX_ES; i++) {
        if (d->pids[i].used)
            pes_flush(d, &d->pids[i]);
    }
    es_emit(d, 1);     /* 最后一个 NAL 收尾 */
}

void ts_demux_get_stats(ts_demux_t *d, ts_demux_stats_t *out)
{
    if (d && out)
        *out = d->st;
}
