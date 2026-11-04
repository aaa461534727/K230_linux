/*
 * wfd_sink.c - Miracast (WFD) Sink — RTSP 服务端 + RTP 接收 + H.264 重组 (阶段 1)
 *
 * 方向与仓里 `wfd-source`(git d878b0a: miracast/src/miracast-source) 正好相反:
 *   Source = RTSP 客户端, 主动连 sink:7236; Sink = 服务端, 在本文件里实现。
 *
 * 报文时序 (与 wfd-source/src/wfd_rtsp.c 头注释、以及真实抓包一致):
 *
 *   Source                                          Sink (本程序)
 *     ├─ M1 OPTIONS (Require org.wfa.wfd1.0) ───────►│
 *     │◄──────────────────────── 200 Public ─────────┤
 *     │◄─ M2 GET_PARAMETER (问 source 能力) ─────────┤
 *     ├─ 200 + video_formats/audio/rtp_ports/uibc ──►│
 *     │◄─ M3 SET_PARAMETER (选定格式 + presentation_URL + client_rtp_ports)
 *     ├─ 200 ───────────────────────────────────────►│
 *     │◄─ M4 SET_PARAMETER wfd_trigger_method: SETUP ┤
 *     ├─ 200 ───────────────────────────────────────►│
 *     ├─ M5 SETUP Transport client_port=49152 ──────►│
 *     │◄─ 200 Session + Transport server_port=27460 ─┤
 *     ├─ M6 PLAY ───────────────────────────────────►│
 *     │◄──────────────────── 200 Range: npt=now- ────┤
 *     │══════ RTP/UDP (PT=96 视频, PT=97 音频) ══════►│ → H.264 FU-A 重组 → dump
 *     │◄─ M16 keepalive GET_PARAMETER (每 20s) ───────┤
 *     ├─ M8 TEARDOWN ───────────────────────────────►│
 *
 * 2026-10-22 修订 (双向建链): 真实抓包 captures/p2p0.pcap 里 TCP 是 **sink 侧发起**
 * (192.168.49.128:49562 → 192.168.49.1:7236), 而手机 :7236 才是 LISTEN 端。
 * 不同手机固件下"谁拨谁"并不固定, 所以本程序现在:
 *   · 无条件监听 :7236 (等对端拨入, 我们作 RTSP server)
 *   · 给了 -c 时同时主动拨 peer:7236 (作 RTSP client)
 *   · 谁先通就用谁; 会话还没开始谈时若对端拨进来, 让位给对端拨入的那条
 * 不论哪种角色, 收到对端请求都照常应答 (on_request), 收到响应照常推进握手。
 *
 * 本阶段 (阶段 1) 验收: Source 连上后能打印 SPS/PPS 与首包 RTP。
 * 硬解/上屏留到阶段 2 (见 docs/MIRACAST_PORT.md §8)。
 *
 * 编译: make            运行: ./wfd-sink -p 7236 -d /tmp/rx.h264 -v
 * 依赖: 无第三方库 (POSIX socket + select)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ts_demux.h"
#include "uibc.h"
#include "audio_out.h"

#define RTSP_BUF      4096
#define DEFAULT_RTSP_PORT 7236
#define DEFAULT_RTP_PORT  27460
#define CONNECT_RTP_PORT  42030   /* 参考抓包 (captures/p2p0.pcap): sink 报给手机的就是 42030 */
#define KEEPALIVE_SEC     20
/* 2026-10-26 实测: K80 偶尔收下 M3/M4 后**不回 SETUP 200**, 我们干等 29 s 直到
 * 对端自己断开 (13:16:48 现场)。4 s 没应答就主动放弃重拨 —— 成功那轮 (10:44:27)
 * 对端 1 s 内就回 200, 所以这个超时只影响"已经失败"的那一轮。 */
/* 2026-10-28 18:10 K80 现场 A/B (每档 8 轮, 同一脚本):
 *   1500 ms + 同 CSeq 重发  → 6/8
 *  12000 ms + 不重发        → 4/8   ← 拉长超时更差
 *   4000 ms + 不重发        → 旧基线 418 轮 15 次超时 = 96.4%
 * 拉长超时的副作用: 一轮卡住 12 s 才断, 期间对端还挂着上一个会话, 下一轮
 * 立刻重拨又撞上去 ⇒ 失败会连成串。所以**恢复 4 s**, 卡住就快点让位。
 * 重发也撤掉: 同 CSeq 重发被 MTK 当错误 (200 之后补一条 400), 换新 CSeq 又被
 * 当"第二条 SETUP"直接关 TCP —— 两条路都通不了。 */
/* 2026-10-31: 改成可配 (默认仍 4 s)。今天 K80 三次里有两次卡在这 4 s 上
 * (`SETUP 发出 4008/4010 ms 仍无 200`), 而成功那轮 1 s 内就回了 ⇒ 说明这台
 * 手机偶尔回得慢。用 WFD_SETUP_TIMEOUT_MS=8000 单独放大, 不改变其它机型默认。 */
#define SETUP_ACK_TIMEOUT_MS 4000
static long long setup_ack_timeout_ms(void)
{
    static long long v = -1;
    if (v < 0) {
        const char *o = getenv("WFD_SETUP_TIMEOUT_MS");
        v = (o && *o) ? strtoll(o, NULL, 0) : SETUP_ACK_TIMEOUT_MS;
        if (v < 500) v = SETUP_ACK_TIMEOUT_MS;
    }
    return v;
}
#define SETUP_MAX_TRIES      1   /* 1 = 不重发, 超时直接交给外层重拨 */
/* 2026-10-26: 手机收下我们的能力后**不发 M3** (SET_PARAMETER 选格式) 时, 原来只能
 * 干等它自己 20~25 s 关 TCP; 期间 7236 被占、管线空转, 用户点第二次也进不来。
 * 12 s 没 M3 就主动 TEARDOWN 收尾, 让下一次重试干净。 */
#define M3_TIMEOUT_MS 12000
#define FU_BUF_SIZE   (4 * 1024 * 1024)

static int g_verbose = 0;
static volatile int g_stop = 0;

/* ============ UIBC 反控 (2026-10-26 新增) ============
 * source (手机) 在能力应答里用 wfd_uibc_capability 的 port= 通告它自己的 UIBC
 * 服务端口; 拿到就在 PLAY 之后连上去推触摸事件。个别机型 port=0 (不监听) ⇒
 * 退化成本端监听 (端口写在我们自己的能力串里, 由手机连进来)。 */
static int  g_src_uibc_port = 0;    /* source 通告的 UIBC 端口 (0 = 未通告/不监听) */
static int  g_uibc_started = 0;     /* 本会话已起过反控 */
static long long g_uibc_at = 0;     /* 延迟启动时刻 (ms): 先等 GET_PARAMETER 回 port */

/* 双模建链辅助 (定义在文件尾, run_session 里要先用到) */
static int accept_nb(int lfd, char *peer_ip, int peer_len);
static int has_pending(int fd);

/* 日志一律走 stderr: stdout 留给 `-d /dev/stdout` 的码流直通管道
 * (否则 sink | decoder | drmplay 管道会被日志污染) */
#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)
#define LOGE(...) do { fprintf(stderr, "ERR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)
#define LOGD(...) do { if (g_verbose) { fprintf(stderr, "  "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while (0)

/* ============ 会话状态 ============ */
typedef enum {
    S_WAIT_M1 = 0,   /* 等 source 的 OPTIONS */
    S_WAIT_OPTIONS_ACK, /* 本端拨号后已发 M1 OPTIONS, 等对端 200 */
    S_WAIT_CAPS,     /* 已发 M2, 等 source 回能力 */
    S_WAIT_M3,       /* 已发 M3(选格式), 等 200 */
    S_WAIT_M4,       /* 已发 M4(trigger=SETUP), 等 200 */
    S_WAIT_SETUP,    /* 等 source SETUP */
    S_WAIT_PLAY,     /* 已回 SETUP 200, 等 PLAY */
    S_STREAMING,     /* 推流中 */
} sink_state_t;

typedef struct {
    int rtsp_fd;
    sink_state_t state;
    int server_cseq;     /* 本端发请求用的 CSeq */
    int session_id;
    char peer_ip[64];

    /* 2026-10-21 抓包实证: 安卓手机投屏时它是 P2P GO 且**自己就是 RTSP 服务端** (7236,
     * `Server: stagefright/1.2 (Linux;Android 16)`), sink 反过来是 RTSP 客户端。
     * connect_mode=1 时本程序主动连 <GO>:7236, 按抓包顺序发 OPTIONS/SETUP/PLAY。 */
    int connect_mode;
    int sent_setup;
    int setup_tries;        /* 本次会话已发过几次 SETUP (含首发, 见 SETUP_MAX_TRIES) */
    int cseq_pin;           /* >0 = 下一条请求复用这个 CSeq (RTSP 重传语义, 见 send_setup) */
    /* 2026-10-27 (K80 实测): 本端每发一个请求 (SET_PARAMETER 声明 UIBC enable 等)
     * 都会占一个 CSeq, 而手机可能对其中某个**回 4xx**。老代码 do_setup_resp() 只判
     * `sent_setup && !playing` 就把**任何**应答当成 SETUP 的应答 ⇒ 手机对 SETUP 前
     * 那条 SET_PARAMETER 回的 454 (Session Not Found) 被误判成"SETUP 被拒", 会话
     * 当场结束 (屏黑 + 无画面)。现在记下真正 SETUP / PLAY 用的 CSeq, 只认对应的应答。 */
    int setup_cseq;
    int play_cseq;
    int sent_opt;                 /* connect 模式: 已发本端 M1 OPTIONS */
    int sent_m2;                  /* 已发 M2 GET_PARAMETER (仅对端拨入路径; 海奇 p2p0 里 sink 不问能力) */
    long long opt_sent_ms;        /* M1 OPTIONS 发出的时刻 (ms) */
    long long setup_ms;           /* SETUP 发出的时刻 (ms), 用于"等 200"看护 */
    long long caps_ms;            /* 回完能力的时刻 (ms), 用于"等 M3"看护 */
    int got_m3;                   /* 已收到 M3 (SET_PARAMETER 选格式) */
    long long conn_ms;            /* 双模建链完成的时刻 (ms), OPTIONS 兜底计时 */
    int playing;
    char presentation_uri[256];   /* source 的 wfd_presentation_URL, SETUP/PLAY 用它 */

    int req_video_port;  /* SETUP client_port (source 侧, 仅记录) */
    int rtp_video_port;  /* 本端 server_port */
    int rtp_audio_port;

    int udp_v;
    int udp_a;      /* 音频候选口 1 (rtp_video_port + 2) */
    int udp_a2;     /* 音频候选口 2 (rtp_video_port + 4) */
    int udp_a0, udp_a20;   /* 上面两口实际绑到的端口 (0 = 没开) */
    int pt96_audio;        /* PT=96 的载荷判定: -1 未定 / 0 = H.264 / 1 = LPCM */

    FILE *dump;
    const char *dump_path;

    FILE *adump;                 /* 音频 ES 取证 (/run/k230-audio.raw), 可选 */
    unsigned long long adump_bytes;   /* 已写入字节 */
    unsigned long long adump_max;     /* 硬上限 (到顶即截断, 绝不无节制涨) */
    int  adump_capped;                /* 到顶已通知过 (只记一次日志) */
    int  audio_probe_logged;     /* 首包日志只打一次 */
    int  audio_pcm_env;          /* WFD_PCM_LE=1 时按小端直通 (默认按大端转) */

    /* RTP (端口, PT) 观测表: 现场一眼看出"手机到底往哪些口/哪些 PT 发过" */
    struct { int port, pt; long n; } pt_seen[8];
    int pt_seen_n;

    /* 统计 */
    long rtsp_req, rtsp_rsp;
    long pkt_video, pkt_audio, bytes_video, bytes_audio;
    long frames, keyframes, nals;
    long marker_count;      /* 收到 RTP marker=1 的包数 (真实 Source 每帧 1 个) */
    long seq_lost;          /* 视频 RTP 序号缺口 (丢包/乱序证据) */
    int  last_seq;

    /* 上游时延漂移 (2026-10-26 加): 用 RTP 时间戳 (90 kHz 媒体时钟) 和本机单调钟比,
     * 漂移 = (真实经过时间) - (媒体时间经过)。漂移持续变大 ⇒ 包到得比它自己的
     * 时间轴慢 ⇒ 对端编码/发送侧在积压 (延迟长在我们管不到的那一段);
     * 漂移≈0 ⇒ 实时到达, 延迟只可能来自我们这段管线。 */
    double ts_ref_ms;       /* 首个视频包的到达时刻 (CLOCK_MONOTONIC, ms) */
    uint32_t ts_ref_rtp;    /* 该包的 RTP 时间戳 */
    uint32_t last_rtp_ts;
    int  ts_ref_set;
    int  saw_sps, saw_pps;
    int  sps_w, sps_h;

    /* FU-A 重组 */
    unsigned char *fu;
    int fu_len;

    /* PT=33 (MPEG-TS) 解包: 手机实测发 MPEG-TS, 见 captures/p2p0.pcap */
    ts_demux_t *ts;

    int have_last_keepalive;
    time_t last_keepalive;
} sink_t;

typedef struct {
    char first[256];
    char method[16];
    char uri[256];
    int  code;          /* response 时有效 */
    char headers[2048];
    char body[4096];
    int  body_len;
    char cseq[32];
} rtsp_msg_t;

/* ============ 小工具 ============ */

static const char *now_str(void)
{
    static char b[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof(b), "%H:%M:%S", &tm);
    return b;
}

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* 从任意 RTSP 报文体里找 source 的 UIBC 端口 (M2 能力应答 / M4 SET_PARAMETER 都可能带) */
static void scan_uibc_caps(const char *body)
{
    const char *u, *p;
    int v;
    if (!body || !*body)
        return;
    u = strstr(body, "wfd_uibc_capability");
    if (!u)
        return;
    LOGI("[%s] source UIBC 能力: %s", now_str(), u);
    p = strstr(u, "port=");
    if (!p)
        return;
    v = atoi(p + 5);
    if (v > 0) {
        if (v != g_src_uibc_port)
            LOGI("[%s] ✅ source UIBC 端口 = %d", now_str(), v);
        g_src_uibc_port = v;
    } else {
        LOGI("[%s] source 通告 port=0 (它不监听) → 反控走本端监听 %d", now_str(), uibc_port());
    }
}

static int write_all(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t r = send(fd, buf + off, (size_t)(len - off), 0);
        if (r <= 0) {
            LOGE("send 失败: %s", strerror(errno));
            return -1;
        }
        off += (int)r;
    }
    return 0;
}

/* 从 headers 里取某个头的值 (大小写不敏感) */
static int get_header(const char *headers, const char *name, char *out, int out_len)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        if (strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen;
            while (*v == ' ' || *v == '\t')
                v++;
            int i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i < out_len - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return 0;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return -1;
}

/* ============ RTSP 报文 I/O ============ */

static int send_response(sink_t *c, const char *cseq, const char *code,
                         const char *extra_headers, const char *body)
{
    char buf[RTSP_BUF];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                    "RTSP/1.0 %s\r\nCSeq: %s\r\n", code, cseq ? cseq : "0");
    if (extra_headers)
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s", extra_headers);
    if (body && body[0]) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\n%s",
                        (int)strlen(body), body);
    } else {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "\r\n");
    }
    LOGI("[%s] --> %s (CSeq %s)", now_str(), code, cseq ? cseq : "0");
    if (body && body[0])
        LOGD("  body: %s", body);
    return write_all(c->rtsp_fd, buf, len);
}

static int send_request(sink_t *c, const char *method, const char *uri,
                        const char *extra_headers, const char *body)
{
    char buf[RTSP_BUF];
    int len = 0;
    if (c->cseq_pin > 0)
        c->server_cseq = c->cseq_pin;   /* 重传: 同一个 CSeq, 对端按重复请求处理 */
    else
        c->server_cseq++;
    len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                    "%s %s RTSP/1.0\r\nCSeq: %d\r\n"
                    "User-Agent: wfdsink/1.0 (Linux;Miracast Sink)\r\n",
                    method, uri, c->server_cseq);
    if (extra_headers)
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s", extra_headers);
    if (body && body[0]) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "Content-Type: text/parameters\r\nContent-Length: %d\r\n\r\n%s",
                        (int)strlen(body), body);
    } else {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "\r\n");
    }
    LOGI("[%s] <-- %s %s (CSeq %d)%s%s", now_str(), method, uri, c->server_cseq,
         (body && body[0]) ? "\n" : "", (body && body[0]) ? body : "");
    return write_all(c->rtsp_fd, buf, len);
}

static int read_line(int fd, char *buf, int buf_len)
{
    int n = 0;
    while (n < buf_len - 1) {
        char ch;
        ssize_t r = recv(fd, &ch, 1, 0);
        if (r <= 0)
            return -1;
        if (ch == '\n') {
            if (n > 0 && buf[n - 1] == '\r')
                n--;
            buf[n] = '\0';
            return n;
        }
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return n;
}

/* 读一条完整 RTSP 报文; 返回 0 成功, -1 连接断开 */
static int read_message(int fd, rtsp_msg_t *m)
{
    char line[2048];
    int content_length = 0;
    int hlen = 0;

    memset(m, 0, sizeof(*m));
    if (read_line(fd, m->first, sizeof(m->first)) < 0)
        return -1;

    m->headers[0] = '\0';
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            content_length = atoi(line + 15);
        if (strncasecmp(line, "CSeq:", 5) == 0) {
            const char *v = line + 5;
            while (*v == ' ')
                v++;
            snprintf(m->cseq, sizeof(m->cseq), "%.*s", (int)sizeof(m->cseq) - 1, v);
        }
        if (hlen < (int)sizeof(m->headers) - n - 3) {
            memcpy(m->headers + hlen, line, (size_t)n);
            hlen += n;
            m->headers[hlen++] = '\r';
            m->headers[hlen++] = '\n';
            m->headers[hlen] = '\0';
        }
    }

    if (content_length > 0) {
        if (content_length > (int)sizeof(m->body) - 1)
            content_length = (int)sizeof(m->body) - 1;
        int got = 0;
        while (got < content_length) {
            ssize_t r = recv(fd, m->body + got, (size_t)(content_length - got), 0);
            if (r <= 0)
                return -1;
            got += (int)r;
        }
        m->body[got] = '\0';
        m->body_len = got;
    }

    /* 解析首行 */
    if (strncmp(m->first, "RTSP/1.0 ", 9) == 0) {
        m->code = atoi(m->first + 9);
        return 0;
    }
    char *sp1 = strchr(m->first, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2) {
        LOGE("无法解析首行: %s", m->first);
        return -1;
    }
    int mlen = (int)(sp1 - m->first);
    int ulen = (int)(sp2 - sp1 - 1);
    if (mlen >= (int)sizeof(m->method)) mlen = (int)sizeof(m->method) - 1;
    if (ulen >= (int)sizeof(m->uri)) ulen = (int)sizeof(m->uri) - 1;
    memcpy(m->method, m->first, (size_t)mlen);
    m->method[mlen] = '\0';
    memcpy(m->uri, sp1 + 1, (size_t)ulen);
    m->uri[ulen] = '\0';
    return 0;
}

/* ============ H.264 重组 ============ */

static unsigned char *g_nal;   /* 暂存单个重组后的 NAL */
static int g_nal_cap;

static int nal_reserve(int need)
{
    if (g_nal_cap >= need)
        return 0;
    int cap = g_nal_cap ? g_nal_cap : 65536;
    while (cap < need)
        cap *= 2;
    unsigned char *p = realloc(g_nal, (size_t)cap);
    if (!p)
        return -1;
    g_nal = p;
    g_nal_cap = cap;
    return 0;
}

/* SPS 解析 (取宽高) —— 失败的场合只影响打印, 不影响统计 */
typedef struct { const unsigned char *d; int len; int pos; int err; } bitreader_t;

static int br_bit(bitreader_t *b)
{
    if (b->pos >= b->len * 8) { b->err = 1; return 0; }
    int v = (b->d[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static unsigned br_bits(bitreader_t *b, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (unsigned)br_bit(b);
    return v;
}

static unsigned br_ue(bitreader_t *b)
{
    int zeros = 0;
    while (!br_bit(b)) {
        if (b->err || ++zeros > 31) { b->err = 1; return 0; }
    }
    return ((1u << zeros) - 1) + br_bits(b, zeros);
}

static int br_se(bitreader_t *b)
{
    unsigned v = br_ue(b);
    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

static void skip_scaling_list(bitreader_t *b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size && !b->err; j++) {
        if (next != 0) {
            int delta = br_se(b);
            next = (last + delta + 256) % 256;
        }
        if (next != 0)
            last = next;
    }
}

static void sps_parse(const unsigned char *nal, int len, int *w, int *h)
{
    /* 去 emulation prevention: 00 00 03 → 00 00 */
    static unsigned char buf[1024];
    int n = 0;
    for (int i = 0; i < len && n < (int)sizeof(buf); i++) {
        if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            buf[n++] = 0;
            buf[n++] = 0;
            i += 2;
            continue;
        }
        buf[n++] = nal[i];
    }
    bitreader_t b = { buf, n, 8 * 1, 0 }; /* 跳过 1 字节 NAL header */
    unsigned profile = br_bits(&b, 8);
    br_bits(&b, 8);          /* constraint flags */
    br_bits(&b, 8);          /* level */
    br_ue(&b);               /* sps id */
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
        profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
        profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        chroma = br_ue(&b);
        if (chroma == 3)
            br_bit(&b);
        br_ue(&b);           /* bit_depth_luma_minus8 */
        br_ue(&b);           /* bit_depth_chroma_minus8 */
        br_bit(&b);          /* qpprime_y_zero_transform_bypass */
        if (br_bit(&b)) {    /* seq_scaling_matrix_present */
            int lists = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < lists && !b.err; i++) {
                if (br_bit(&b))
                    skip_scaling_list(&b, i < 6 ? 16 : 64);
            }
        }
    }
    br_ue(&b);               /* log2_max_frame_num_minus4 */
    unsigned poc_type = br_ue(&b);
    if (poc_type == 0) {
        br_ue(&b);
    } else if (poc_type == 1) {
        br_bit(&b);
        br_se(&b);
        br_se(&b);
        unsigned n = br_ue(&b);
        for (unsigned i = 0; i < n && !b.err; i++)
            br_se(&b);
    }
    br_ue(&b);               /* max_num_ref_frames */
    br_bit(&b);              /* gaps_in_frame_num_allowed */
    unsigned wmbs = br_ue(&b) + 1;
    unsigned hmbs = br_ue(&b) + 1;
    unsigned frame_mbs_only = br_bit(&b);
    if (!frame_mbs_only)
        br_bit(&b);
    br_bit(&b);              /* direct_8x8_inference */
    int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (br_bit(&b)) {        /* frame_cropping */
        crop_l = (int)br_ue(&b);
        crop_r = (int)br_ue(&b);
        crop_t = (int)br_ue(&b);
        crop_b = (int)br_ue(&b);
    }
    if (b.err) {
        *w = 0;
        *h = 0;
        return;
    }
    int cux = (chroma == 1 || chroma == 2) ? 2 : 1;
    int cuy = (chroma == 1) ? 2 : ((chroma == 2) ? 1 : 1) * (frame_mbs_only ? 1 : 2);
    if (chroma == 3) cuy = frame_mbs_only ? 1 : 2;
    *w = (int)wmbs * 16 - cux * (crop_l + crop_r);
    *h = (int)(2 - frame_mbs_only) * (int)hmbs * 16 - cuy * (crop_t + crop_b);
}

/* 判断一个 VCL NAL 是否是新图像起始 (first_mb_in_slice == 0)
 *
 * 帧边界不能只靠 RTP marker 位: 仓里的参考 Source (wfd_rtp_video.c:188)
 * 恒把 marker=0 发出, 于是按 marker 计帧会得到 frames=0。真实手机/Source
 * 都会置 marker, 但为了两边都能用, 这里两个信号都认:
 *   - RTP marker=1            → 本帧结束
 *   - VCL NAL 的 first_mb_in_slice==0 → 新帧开始
 * first_mb_in_slice 是 slice 头第一个 ue(v) 字段, ue==0 ⇒ 编码就是单个 '1' bit。 */
static int is_picture_start(const unsigned char *nal, int len)
{
    if (len < 2)
        return 0;
    return (nal[1] & 0x80) != 0;
}

static void emit_nal(sink_t *c, const unsigned char *nal, int len)
{
    if (len <= 0)
        return;
    int type = nal[0] & 0x1f;
    c->nals++;
    if ((type == 1 || type == 5) && is_picture_start(nal, len)) {
        c->frames++;
        if (c->frames == 1)
            LOGI("[%s] ✅ 第一个完整图像 (first_mb_in_slice=0) 到达", now_str());
    }
    if (type == 7) {
        c->saw_sps = 1;
        int w = 0, h = 0;
        sps_parse(nal, len, &w, &h);
        if (w && h) {
            c->sps_w = w;
            c->sps_h = h;
            LOGI("[%s] ✅ 收到 SPS: %dx%d (%d 字节)", now_str(), w, h, len);
        } else {
            LOGI("[%s] ✅ 收到 SPS (%d 字节)", now_str(), len);
        }
    } else if (type == 8) {
        c->saw_pps = 1;
        LOGI("[%s] ✅ 收到 PPS (%d 字节)", now_str(), len);
    } else if (type == 5) {
        c->keyframes++;
    }
    if (c->dump) {
        static const unsigned char sc[4] = { 0, 0, 0, 1 };
        fwrite(sc, 1, 4, c->dump);
        fwrite(nal, 1, (size_t)len, c->dump);
    }
}

static void handle_rtp_h264(sink_t *c, const unsigned char *p, int len)
{
    if (len < 1)
        return;
    int type = p[0] & 0x1f;

    if (type >= 1 && type <= 23) {                     /* 单 NAL */
        emit_nal(c, p, len);
    } else if (type == 24) {                           /* STAP-A */
        int i = 1;
        while (i + 2 <= len) {
            int n = (p[i] << 8) | p[i + 1];
            i += 2;
            if (n <= 0 || i + n > len)
                break;
            emit_nal(c, p + i, n);
            i += n;
        }
    } else if (type == 28) {                           /* FU-A */
        if (len < 2)
            return;
        int start = (p[1] & 0x80) != 0;
        int end = (p[1] & 0x40) != 0;
        unsigned char hdr = (unsigned char)((p[0] & 0xe0) | (p[1] & 0x1f));
        if (start) {
            c->fu_len = 0;
            if (nal_reserve(1) == 0)
                c->fu[c->fu_len++] = hdr;
        }
        if (c->fu_len > 0) {
            int body = len - 2;
            if (nal_reserve(c->fu_len + body) == 0) {
                memcpy(c->fu + c->fu_len, p + 2, (size_t)body);
                c->fu_len += body;
            }
            if (end) {
                emit_nal(c, c->fu, c->fu_len);
                c->fu_len = 0;
            }
        }
    }
}

/* 解析 RTP 头, 返回 payload 偏移; -1 = 非法 */
static int rtp_payload_off(const unsigned char *pkt, int len, int *pt, int *marker)
{
    if (len < 12 || (pkt[0] >> 6) != 2)
        return -1;
    int cc = pkt[0] & 0x0f;
    int ext = (pkt[0] >> 4) & 1;
    int pad = (pkt[0] >> 5) & 1;
    *marker = pkt[1] >> 7;
    *pt = pkt[1] & 0x7f;
    int off = 12 + 4 * cc;
    if (ext) {
        if (len < off + 4)
            return -1;
        int elen = (pkt[off + 2] << 8) | pkt[off + 3];
        off += 4 + 4 * elen;
    }
    if (off > len)
        return -1;
    if (pad) {
        int padlen = pkt[len - 1];
        if (padlen <= 0 || off + padlen > len)
            return -1;
        len -= padlen;   /* 仅影响 payload 结尾, 由调用者按 payload_len 处理 */
    }
    return off;
}

static void emit_nal(sink_t *c, const unsigned char *nal, int len);

/* ---- 音频取证 dump: 带硬上限 (2026-10-29, 主人要求「抓包文件要限制大小不能无节制」) ----
 * 只在 touch /run/k230-audio-dump 或 WFD_AUDIO_DUMP=路径 时开; 默认上限 32 MB
 * (48k*2ch*2B = 192 KB/s ⇒ 32 MB ≈ 2 分 46 秒, 够分析一段音乐又不至于把 /run 撑爆)。
 * 到顶就关文件 + 记一次日志, 之后静默丢弃 —— 不删文件、不轮转, 保持「抓到的就是完整的开头」。 */
#define ADUMP_DEFAULT_MAX (32ULL * 1024 * 1024)

static void audio_dump_write(sink_t *c, const unsigned char *p, int len)
{
    if (!c->adump || len <= 0)
        return;
    if (c->adump_max && c->adump_bytes + (unsigned long long)len > c->adump_max) {
        int room = (int)(c->adump_max - c->adump_bytes);
        if (room > 0) {
            fwrite(p, 1, (size_t)room, c->adump);
            c->adump_bytes += (unsigned long long)room;
        }
        fclose(c->adump);
        c->adump = NULL;
        if (!c->adump_capped) {
            c->adump_capped = 1;
            LOGI("[%s] 音频 dump 到达上限 %.1f MB → 已截断关闭 (保留前段, 不再写入)",
                 now_str(), (double)c->adump_max / (1024.0 * 1024.0));
        }
        return;
    }
    size_t w = fwrite(p, 1, (size_t)len, c->adump);
    c->adump_bytes += (unsigned long long)w;
    fflush(c->adump);
}

/* ts_demux 回调: TS 里切出来的 H.264 NAL 直接进原有统计/dump 路径 */
static void on_ts_nal(void *user, const unsigned char *nal, int len)
{
    emit_nal((sink_t *)user, nal, len);
}

/* ts_demux 回调: TS 里的音频 ES (PES 头已剥) —— 2026-10-27 加。
 *
 * 为什么要这条路径: 真机 M4 已经谈成 LPCM, 但 audio_pkts=0。手机把音视频一起
 * 复用在同一路 MPEG-TS (RTP PT=33) 里, 而老 ts_demux 只取 H.264 的 PID, 音频
 * PID 直接丢弃 ⇒ 永远收不到音频。现在音频 PID 单独重组 PES, 这里接住。
 *
 * 只有 LPCM 家族 (0x83/0x80/0x06 等) 才直接写 PCM 出口; AAC 等需要解码器,
 * 本板没有, 只记录 + 落盘取证, 绝不把压缩码流当 PCM 放出去 (那是刺耳噪音)。 */
static void on_ts_audio(void *user, const unsigned char *es, int len, int stype)
{
    sink_t *c = (sink_t *)user;
    if (len <= 0)
        return;
    c->pkt_audio++;
    c->bytes_audio += len;

    if (!c->audio_probe_logged) {
        c->audio_probe_logged = 1;
        LOGI("[%s] ✅ 收到首个音频 ES: stream_type=0x%02x, %d 字节",
             now_str(), stype, len);
    }
    audio_dump_write(c, es, len);
    if (stype == 0x83 || stype == 0x80 || stype == 0x06) {
        if (c->audio_pcm_env)
            audio_out_pcm_le16(es, len);      /* WFD_PCM_LE=1: 原样直通 */
        else
            audio_out_pcm_be16(es, len);
    }
}

static double mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

static void rtp_ts_track(sink_t *c, const unsigned char *pkt)
{
    uint32_t rts = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
                   ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
    if (!c->ts_ref_set) {
        c->ts_ref_ms = mono_ms();
        c->ts_ref_rtp = rts;
        c->ts_ref_set = 1;
    }
    c->last_rtp_ts = rts;
}

/* RTP (端口, PT) 观测: 每个新组合只打一次, 之后累计计数 —— 现场判"音频是不是
 * 发到别的端口/别的 PT"用 (2026-10-27 音频排查)。 */
static void pt_note(sink_t *c, int port, int pt, int len)
{
    for (int i = 0; i < c->pt_seen_n; i++) {
        if (c->pt_seen[i].port == port && c->pt_seen[i].pt == pt) {
            c->pt_seen[i].n++;
            return;
        }
    }
    if (c->pt_seen_n < (int)(sizeof(c->pt_seen) / sizeof(c->pt_seen[0]))) {
        c->pt_seen[c->pt_seen_n].port = port;
        c->pt_seen[c->pt_seen_n].pt = pt;
        c->pt_seen[c->pt_seen_n].n = 1;
        c->pt_seen_n++;
        LOGI("[%s] RTP 观测: udp=%d PT=%d 首包 %d 字节",
             now_str(), port, pt, len);
    }
}

/* aux_port != 0: 该包来自音频候选口 (rtp_video_port+2/+4)。WFD 允许音视频用
 * 两条 RTP 会话, 手机可能把音频单独发到我们多开的端口上 —— 那些口上除了
 * PT=33 (TS) 以外一律按音频处理, 不再走 H.264 的 NAL 猜测。 */
static void handle_rtp_ex(sink_t *c, const unsigned char *pkt, int len, int aux_port)
{
    int pt = 0, marker = 0;
    int off = rtp_payload_off(pkt, len, &pt, &marker);
    if (off < 0)
        return;
    int plen = len - off;
    if (plen <= 0)
        return;
    pt_note(c, aux_port ? aux_port : c->rtp_video_port, pt, plen);

    if (aux_port && pt != 33) {          /* 音频专用口: 直接当 LPCM */
        c->pkt_audio++;
        c->bytes_audio += plen;
        if (c->pkt_audio == 1) {
            LOGI("[%s] ✅ 收到首包 RTP 音频 (PT=%d, 端口 %d, %d 字节), 前 16 字节 =",
                 now_str(), pt, aux_port, plen);
            for (int i = 0; i < 16 && i < plen; i++)
                fprintf(stderr, " %02x", pkt[off + i]);
            fprintf(stderr, "\n");
        }
        audio_dump_write(c, pkt + off, plen);
        if (c->audio_pcm_env)
            audio_out_pcm_le16(pkt + off, plen);
        else
            audio_out_pcm_be16(pkt + off, plen);
        return;
    }

    if (pt == 33) {          /* MPEG-TS (手机/Android 实测用这个, 见 captures/p2p0.pcap) */
        int seq = (pkt[2] << 8) | pkt[3];
        if (c->last_seq >= 0) {
            int d = (seq - c->last_seq) & 0xffff;
            if (d > 1 && d < 0x8000)
                c->seq_lost += d - 1;
        }
        c->last_seq = seq;
        rtp_ts_track(c, pkt);
        c->pkt_video++;
        c->bytes_video += plen;
        if (c->pkt_video == 1)
            LOGI("[%s] ✅ 收到首包 RTP 视频 (PT=33 MPEG-TS, %d 字节 payload)",
                 now_str(), plen);
        if (c->ts)
            ts_demux_feed(c->ts, pkt + off, plen);
        if (marker)
            c->marker_count++;
    } else if (pt == 96) {
        /* PT=96 既可能是裸 H.264 (自家 PC Source), 也可能是 LPCM (部分手机)。
         * 2026-10-27: 改成**首包定死本会话**, 不再逐包猜 —— 逐包猜会让 PCM 随机
         * 命中小概率 NAL 类型 (1/5/7/8) 而把音频当视频喂给解码器 (花屏/卡顿)。 */
        int nt = pkt[off] & 0x1f;
        if (c->pt96_audio < 0) {
            c->pt96_audio = (nt == 1 || nt == 5 || nt == 7 || nt == 8 ||
                             nt == 24 || nt == 28) ? 0 : 1;
            LOGI("[%s] PT=96 载荷判定: %s (首字节 0x%02x, NAL type %d, 本会话不再变)",
                 now_str(), c->pt96_audio ? "音频 LPCM" : "H.264 视频", pkt[off], nt);
        }
        if (c->pt96_audio) {
            c->pkt_audio++;
            c->bytes_audio += plen;
            if (c->pkt_audio == 1)
                LOGI("[%s] ✅ 收到首包 RTP 音频 (PT=96, %d 字节 LPCM)", now_str(), plen);
            if (c->audio_pcm_env)
                audio_out_pcm_le16(pkt + off, plen);
            else
                audio_out_pcm_be16(pkt + off, plen);
        } else {
            /* 序号缺口统计 (只跟视频流; 音视频共端口时序号各自独立) */
            int seq = (pkt[2] << 8) | pkt[3];
            if (c->last_seq >= 0) {
                int d = (seq - c->last_seq) & 0xffff;
                if (d > 1 && d < 0x8000)
                    c->seq_lost += d - 1;
            }
            c->last_seq = seq;
            c->pkt_video++;
            c->bytes_video += plen;
            if (c->pkt_video == 1)
                LOGI("[%s] ✅ 收到首包 RTP 视频 (PT=96, %d 字节 payload)", now_str(), plen);
            handle_rtp_h264(c, pkt + off, plen);
            if (marker)
                c->marker_count++;
        }
    } else if (pt == 97 || pt == 98) {   /* audio (LPCM) */
        /* 2026-10-26 加音频: WFD 的 LPCM 音频走独立 PT (实测 97/98), 载荷是
         * 48 kHz/16 bit/立体声 的**大端** PCM。转小端写进 /run/wfd-audio.pcm,
         * 由常驻 pcmplay 出声 (与 AirPlay 共用同一个 ALSA 出口)。 */
        c->pkt_audio++;
        c->bytes_audio += plen;
        if (c->pkt_audio == 1)
            LOGI("[%s] ✅ 收到首包 RTP 音频 (PT=%d, %d 字节 LPCM)", now_str(), pt, plen);
        audio_dump_write(c, pkt + off, plen);
        if (c->audio_pcm_env)
            audio_out_pcm_le16(pkt + off, plen);
        else
            audio_out_pcm_be16(pkt + off, plen);
    } else {
        LOGD("未知 RTP PT=%d len=%d", pt, plen);
    }
}

/* 主收流口 (= SETUP 里声明的 client_port) */
static void handle_rtp(sink_t *c, const unsigned char *pkt, int len)
{
    handle_rtp_ex(c, pkt, len, 0);
}

/* ============ UDP 端口 ============ */

static int udp_open(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        LOGE("bind UDP %d 失败: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return fd;
}

static int port_in_use(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    int r = bind(fd, (struct sockaddr *)&a, sizeof(a));
    close(fd);
    return r < 0 ? 1 : 0;
}

/* 音频候选口 (2026-10-27): 手机若把音频走**另一条 RTP 会话**, 目的端口一般是
 * SETUP 里 client_port 的 +2/+4。这两个口是"探测 + 接收"双用: 收到就按音频处理
 * (见 handle_rtp_ex 的 aux_port 分支), 收不到就只是两个空闲 UDP socket, 不影响视频。
 * 绑定失败只记一句, 绝不影响主流程。 */
static void open_audio_probe_ports(sink_t *c)
{
    if (c->udp_a < 0) {
        c->udp_a0 = c->rtp_video_port + 2;
        c->udp_a = udp_open(c->udp_a0);
        if (c->udp_a >= 0)
            LOGI("[%s] 音频候选口 udp=%d 已打开 (接收可能分离的音频 RTP)",
                 now_str(), c->udp_a0);
        else
            LOGI("[%s] ⚠ 音频候选口 udp=%d 打开失败 (不影响视频)", now_str(), c->udp_a0);
    }
    if (c->udp_a2 < 0) {
        c->udp_a20 = c->rtp_video_port + 4;
        c->udp_a2 = udp_open(c->udp_a20);
        if (c->udp_a2 >= 0)
            LOGI("[%s] 音频候选口 udp=%d 已打开 (接收可能分离的音频 RTP)",
                 now_str(), c->udp_a20);
    }
}

/* ============ WFD 参数 ============ */

/* 从 source 回的能力里挑一个我们支持的分辨率, 返回 CEA bit 与宽高 */
static uint32_t pick_format(const char *caps, int *w, int *h, int *fps)
{
    /* source 的 wfd_video_formats 第 5 个字段是 CEA bitmap (8 hex) */
    const char *p = strstr(caps, "wfd_video_formats:");
    uint32_t cea = 0;
    if (p) {
        /* 形如: <native> <preferred> <profile> <level> <cea 8hex> ... */
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", p + strlen("wfd_video_formats:"));
        char *nl = strchr(tmp, '\r');
        if (nl) *nl = '\0';
        if (nl) *nl = '\0';
        unsigned int f[9] = { 0 };
        int n = sscanf(tmp, "%x %x %x %x %x %x %x %x %x",
                       &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8]);
        if (n >= 5)
            cea = f[4];
        LOGD("source CEA bitmap = 0x%08x (字段数 %d)", cea, n);
    }

    /* 2026-10-26 (60fps): 720p60 提到 720p30 前面 —— 否则 source 两档都支持时
     * 我们又自己选回 30, 主人要的 60 永远上不去。 */
    struct { uint32_t bit; int w, h, fps; } tbl[] = {
        { 1u << 6,  1280, 720, 60 },   /* 1280x720p60 */
        { 1u << 5,  1280, 720, 30 },   /* 1280x720p30 */
        { 1u << 7,  1920, 1080, 30 },  /* 1920x1080p30 */
        { 1u << 8,  1920, 1080, 60 },  /* 1920x1080p60 */
        { 1u << 10, 1280, 720, 25 },
        { 1u << 11, 1280, 720, 50 },
        { 1u << 12, 1920, 1080, 25 },
        { 1u << 13, 1920, 1080, 50 },
        { 1u << 0,  640,  480, 60 },
        { 1u << 1,  720,  480, 60 },
    };

    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (cea & tbl[i].bit) {
            *w = tbl[i].w;
            *h = tbl[i].h;
            *fps = tbl[i].fps;
            return tbl[i].bit;
        }
    }
    *w = 1280;
    *h = 720;
    *fps = 30;
    return 1u << 5;
}

/* 广告给 source 的 CEA 分辨率位图。
 * 2026-10-27 (流畅度): 默认 = 720p60 | 720p30 两档。60fps 只是**上限与首选**,
 * 手机觉得跑不动仍可自己退回 720p30 —— 不是「只剩 720p60 一档」的硬绑。
 *
 * 2026-10-26 主人要 60fps 的铁律: **WFD 里没有单独的帧率字段** —— 源端编多少帧,
 * 完全由它在位图里挑中的那一档决定 (bit5=720p30, bit6=720p60, bit7=1080p30)。
 * 所以「我要 60fps」只能靠广告 bit6, 改码率/缓冲/看护都无济于事。
 * 实测: 广告 0x20 时手机上 60fps 内容, 我们收到的就是 30.0 fps (3076 帧 / 102.5 s) ——
 * 那是我们自己给的天花板, 不是手机掉帧, 也不是解码跟不上 (720p60 离线 88.8 fps)。 */
static uint32_t advertised_cea(void)
{
    static int init = 0;
    static uint32_t v;
    if (!init) {
        /* WFD_CEA 可直接给定 CEA 位图 (十六进制), 用于现场试档:
         *   0x20 = 720p30    0x60 = 720p60|720p30 (默认)   0x80 = 1080p30
         *   0x2000 = 1080p50 (K80 早期自己挑的那个)
         * 位图语义: 0-based bit N = WFD 模式表第 (N+1) 项。 */
        const char *o = getenv("WFD_CEA");
        const char *e = getenv("WFD_LOWLAT");
        int lowlat = (e && *e && strcmp(e, "0") != 0);
        if (o && *o) {
            v = (uint32_t)strtoul(o, NULL, 0);
        } else if (lowlat) {
            v = 1u << 5;                  /* WFD_LOWLAT=1 保留旧的低延迟单档 */
        } else {
            /* 2026-10-28 定版: 默认只广告 **720p30** (bit5)。
             *
             * 为什么从 0x60 退回 0x20 —— 板上同条件 A/B (努比亚 Z17 miniS, 通知栏下拉,
             * 每档 6 次全部检出; t0=发首个 down 包, t1=该帧 flip 后写日志, 同一块板同一
             * CLOCK_MONOTONIC, 无跨机对时误差):
             *     720p30 首帧变化 269/276/284/297/305/339 ms  (avg 295)
             *     720p60 首帧变化 646/676/697/725/791/815 ms  (avg 725)
             *   板端自身只占约 17 ms (整圈), 多出来的 430 ms 全在**手机编码器**那一侧:
             *   手机要压两倍张数的 1280x720, wfdservice 吃到 28% CPU、loadavg 6.8,
             *   连手机本机 UI 都变慢 (本机 input tap 的 Displayed +590~652 ms)。
             *   这块板是**单核** (nproc=1, 无 cpufreq), 而端到端不只算板端 ⇒
             *   60fps 换来的 60 张/秒, 是"手机上更慢地生成 60 张".
             *
             * 单档而不是 0x60 双档: 双档时 native/preferred 会跟着 cea_pref_bit() 指到
             *   bit6, 手机一律挑 60 (现场实测就是这么走的), 等于没退。
             *   每台手机都支持 720p30 (WFD 强制档), 不存在"手机选不了 ⇒ 黑屏"的风险。
             *
             * 需要 60fps 的场合 (看视频/演示): WFD_CEA=0x40 或 0x60 重启服务即可,
             * 不必换二进制。 */
            v = 1u << 5;
        }
        init = 1;
    }
    return v;
}

/* wfd_client_rtp_ports 的第二项 (音频 RTP 口)。
 *
 * 2026-10-27 现场回归: 把第二项从 0 改成「视频口+2」后, 小米手机 (能力查询里带
 * wfd_mtk_video_formats / wfd_xm_extension) **连续 3 次**在我们回完能力 1 s 内
 * 直接关 RTSP (Miracast 日志: 回能力(361 B) → 200 OK (CSeq 2) → RTSP 连接关闭),
 * 连 M3 都不发; 而 17:28 那场广告的是 `42030 0`, 能一路走到 PLAY。
 * ⇒ 广告一个"非 0 但我们并没有真的开音频 RTP 会话"的端口会把这台手机直接劝退。
 * 默认恢复为 0 (= 没有音频口, 与黄金版本一致); 只有显式 WFD_ADV_AUDIO_PORT=1
 * 才重新广告 video+2, 用于以后单独验证音频。 */
static int adv_audio_port(int video_port)
{
    const char *e = getenv("WFD_ADV_AUDIO_PORT");
    if (e && *e && strcmp(e, "0") != 0)
        return video_port + 2;
    return 0;
}

/* 从位图里挑「首选档」: 写进 native / preferred-display-mode 两个字段。
 * 光改第 5 字段不够 —— AOSP (WifiDisplaySource) 是照 sink 声明的 native 回 M3 的,
 * 而另一些实现是从第 5 字段挑最高的。两边都指到我们要的那一档才稳。 */
static uint32_t cea_pref_bit(uint32_t cea)
{
    static const uint32_t prio[] = {
        1u << 6,   /* 1280x720p60  */
        1u << 5,   /* 1280x720p30  */
        1u << 7,   /* 1920x1080p30 */
        1u << 8,   /* 1920x1080p60 */
        1u << 11,  /* 1280x720p50  */
        1u << 13,  /* 1920x1080p50 */
        1u << 10,  /* 1280x720p25  */
        1u << 12,  /* 1920x1080p25 */
    };
    for (size_t i = 0; i < sizeof(prio) / sizeof(prio[0]); i++)
        if (cea & prio[i])
            return prio[i];
    return cea & (~cea + 1u);             /* 兜底: 取最低位 */
}

/* CEA 位图里是否含 1080 系列 (bit7=1080p30, bit8=1080p60, bit12=1080p25, bit13=1080p50) */
static int cea_has_1080(uint32_t cea)
{
    return (cea & ((1u << 7) | (1u << 8) | (1u << 12) | (1u << 13))) != 0;
}

static void build_video_formats(uint32_t cea_bit, int profile, int level, char *out, int n)
{
    /* Sink 选定格式 (WFD 1.1 表 6-3, **14 个字段**):
     * <native> <preferred-display-mode> <profile> <level>
     * <CDF> <CEA> <VESA> <HH> <latency> <min-slice> <slice-enc>
     * <frame-rate-control> <max-hres> <max-vres>
     *
     * 2026-10-26 两个致命错都在这行:
     *  ① 原来只有 13 个字段 (缺 frame-rate-control), max-hres/max-vres 整体错位;
     *  ② CDF 和 VESA 写的是「全模式都支持」。手机 (K80) 是**从 CDF 里挑**的,
     *     于是它在 0001FEFF 里选了 bit13 = 1920x1080p50 (实测 5074 帧 / 101.5 s),
     *     我们 CEA 里只开 720p30 完全没被看到 ⇒ 白改 CEA 位图。
     * 现在只留 CEA 一个位, CDF/VESA 置 0, 手机没有别的可挑。 */
    /* 2026-10-26 (60fps): 第 1/2 字段原来是 00 00 (等于「native 就是 720p30」),
     * AOSP 是照 native 回 M3 的 ⇒ 广告 720p60 也会被拉回 30。
     * 2026-10-26 晚: 主人要「先保证有一版能使用」⇒ 只有**显式**给了 WFD_CEA /
     * WFD_LOWLAT 才让这两字段跟着位图走; 默认仍回 00 00 (30fps 基线的原样)。 */
    /* 2026-10-27: native / preferred 恒跟随首选档 (默认位图 0x60 → 40 40 = 720p60)。
     * 光改 CEA 位图不够: AOSP 主要照 native 回 M3, 只把位图开成双档会被它按 00 00
     * 拉回最高档里"自己挑一个" (历史: 挑到 1080p50)。两个字段都指到 720p60 才稳。 */
    char native[16];
    snprintf(native, sizeof(native), "%02x %02x", cea_pref_bit(cea_bit), cea_pref_bit(cea_bit));
    snprintf(out, n,
             "%s %02x %02x %08x 00000000 00000000 00 0000 0000 00 00 %s",
             native, profile, level, cea_bit,
             cea_has_1080(cea_bit) ? "0780 0438" : "0500 02d0");
}

/* ============ 状态机 ============ */

/* 按 source 的 GET_PARAMETER 请求体逐项回能力 —— 请求体里出现的**每一项**都回,
 * 不认识的回 none。Windows 10 是逐项校验的严格实现: 少回一项它就不发 M3,
 * 干等 80 s 后以 inactivity 把我们踢出组 (2026-10-25 实机: 10:58:42 回能力 → 11:00:02 被踢)。
 * 另: wfd_video_formats 必须 14 个字段 (WFD 1.1 表 6-3):
 *   <native> <pref> <profile> <level> <CDF> <CEA> <VESA> <HH> <latency>
 *   <min-slice> <slice-enc> <frc> <max-hres> <max-vres>
 * 旧实现只有 13 个字段 (HH 只有 1 字节且缺 max-vres) —— 手机不校验所以能过, Windows 直接丢。 */
static void build_caps_body(sink_t *c, const char *req, char *out, size_t n)
{
    int port = c->rtp_video_port > 0 ? c->rtp_video_port : DEFAULT_RTP_PORT;
    size_t u = 0;
    char tmp[1024];
    out[0] = 0;
    if (strlen(req) >= sizeof(tmp))
        return;
    memcpy(tmp, req, strlen(req) + 1);

    for (char *save = NULL, *line = strtok_r(tmp, "\r\n", &save);
         line != NULL; line = strtok_r(NULL, "\r\n", &save)) {
        char name[64], vbuf[192];
        size_t j = 0;
        const char *val;
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
            line++;
        while (line[j] != 0 && line[j] != ':' && line[j] != '=' &&
               line[j] != ' ' && line[j] != '\t' && j + 1 < sizeof(name)) {
            name[j] = line[j];
            j++;
        }
        name[j] = 0;
        if (j < 4)
            continue;

        if (strcmp(name, "wfd_video_formats") == 0) {
            /* 13 个字段 (native pref profile level CDF CEA VESA HH latency
             * min-slice slice-enc max-hres max-vres); 末尾 none = 分辨率不设上限。
             * 取值照抄 Windows 实测可用的 MS-WFDPE 组合 (miraclecast #471):
             * profile=02(CHP) level=10 HH=00 latency/min-slice/slice-enc 全 0。
             * 旧值 1f/00ff/11/0780/0438 把这几个槽位写错 ⇒ Windows 判为非法能力。 */
            /* 2026-10-25 降延迟: 上限从 none(不限) 收到 1280x720 —— 手机不再发 1080p,
             * 管道流量砍一半多, CPU 缩放也轻; 要恢复 1080p 把末尾改成 none none。 */
            /* 2026-10-26 实测修正: 这里原来把 CEA 位图写死成 3FFFFFFF (全部 CEA 模式),
             * 手机就从里面挑了 1920x1080 @50fps (16:48 现场: res=1920x1080, dec 49.9fps),
             * 单核板上要处理 4 倍于 720p 的像素 + 50fps 上屏, 直接卡。
             * 现在 CEA 位图由 advertised_cea() 决定 (WFD_LOWLAT=1 时只有 bit6=720p60)。
             * native 位图保留 (未压缩模式, 手机不会选), 末两字段仍限 1280x720。 */
            /* 2026-10-26 关键修正 (K80 实测铁证): 第 5 字段原来是 0001FEFF = 「所有模式」,
             * 而第 6 字段才是我方想开的 720p30 —— **位置写反了**。手机是从第 5 字段挑的,
             * 于是它在全模式里选了 bit13 = 1920x1080p50 (sink 统计 res=1920x1080,
             * 5074 帧 / 101.5 s = 50.0 fps, play 48.0 fps), 我们开 720p30 完全没生效。
             * 对照 K80 自己的 M3 也是把选定档放第 5 字段 (00002000 = bit13 = 1080p50)。
             * 现在: 第 5 字段 = 我方唯一想开的档, 第 6/7 置 0; 并补齐第 12 字段
             * frame-rate-control (原来只有 13 个字段, max-hres/max-vres 整体错位)。 */
            build_video_formats(advertised_cea(), 0x02, 0x10, vbuf, sizeof(vbuf));
            val = vbuf;
        } else if (strcmp(name, "wfd_audio_codecs") == 0) {
            /* 2026-10-26 加音频: 只广告 LPCM 48 kHz/16 bit/2ch。
             * 为什么不带 AAC: 本板没有 AAC 解码器, 而 LPCM 是 WFD 强制必须支持的格式,
             * 手机一定会接受; 带 AAC 反而可能被手机优先选中导致无声。 */
            val = "LPCM 00000002 00";
        } else if (strcmp(name, "wfd_client_rtp_ports") == 0) {
            /* WFD 1.1 表 6-9: "RTP/AVP/UDP;unicast <视频口> <音频口> mode=play"。
             * 2026-10-27 音频实验: 曾把第二项改成视频口+2, 结果小米手机见到非 0
             * 直接关 RTSP (见 adv_audio_port() 注释) ⇒ 默认回 0,
             * 只有 WFD_ADV_AUDIO_PORT=1 时才广告旁路音频口。 */
            snprintf(vbuf, sizeof(vbuf), "RTP/AVP/UDP;unicast %d %d mode=play",
                     port, adv_audio_port(port));
            val = vbuf;
        } else if (strcmp(name, "wfd_uibc_capability") == 0) {
            /* 2026-10-26 反控: 原来写死 none (= 不支持反控)。现在声明 GENERIC
             * (单指/多指) + 本端监听端口, 手机据此建 UIBC TCP 通道。 */
            val = uibc_capability();
        } else if (strcmp(name, "wfd_uibc_setting") == 0) {
            /* 2026-10-27 修: source 若把 wfd_uibc_setting 也列进能力查询, 原来会
             * 落到 else 回 "none" —— 等于当面告诉手机"别开反控"。现在明确回 enable。 */
            val = "enable";
        } else if (strcmp(name, "wfd_connector_type") == 0) {
            val = "05";                      /* 05 = HDMI */
        } else if (strcmp(name, "wfd_idr_request_capability") == 0) {
            val = "1";
        } else if (strcmp(name, "wfd2_video_stream_control") == 0) {
            val = "00";
        } else if (strcmp(name, "microsoft_video_formats") == 0) {
            val = "0000003fffff";
        } else if (strcmp(name, "microsoft_max_bitrate") == 0) {
            val = "25000000";
        } else if (strcmp(name, "intel_friendly_name") == 0) {
            val = "K230-CAST";
        } else if (strcmp(name, "intel_sink_manufacturer_name") == 0) {
            val = "Canaan";
        } else if (strcmp(name, "intel_sink_model_name") == 0) {
            val = "K230";
        } else if (strcmp(name, "intel_sink_version") == 0) {
            val = "product_ID=K230 hw_version=1.0 sw_version=1.0";
        } else if (strcmp(name, "intel_sink_device_URL") == 0) {
            val = "http://192.168.137.1";
        } else {
            /* content_protection / display_edid / uibc / coupled_sink / wfd2_* /
             * microsoft_* 其余项: 规范允许用 none 表示不支持 */
            val = "none";
        }

        if (n - u < 2)
            break;
        int w = snprintf(out + u, n - u, "%s: %s\r\n", name, val);
        if (w < 0)
            break;
        u += (size_t)w;
    }
    if (u >= n)
        out[n - 1] = 0;
    if (out[0] == 0)
        snprintf(out, n,
                 "wfd_video_formats: 00 00 02 1f %08x 1fffffff 00000fff 00000002 "
                 "0000 0000 0000 0000 0780 0438\r\n"
                 "wfd_audio_codecs: LPCM 00000002 00\r\n"
                 "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d %d mode=play\r\n"
                 "wfd_content_protection: none\r\n", advertised_cea(), port,
                 adv_audio_port(port));
}

/* 从报文体的某一行取参数值 ("wfd_video_formats: ..." → "...")
 * 用途: M3 的 200 应答里必须把 source 选定的格式原样回显 (WFD 1.1 §6.5),
 * 只回空 body 时部分手机(含本次实测的安卓)会把 UIBC/音频当成"未确认"而不启用。 */
static int body_get_param(const char *body, const char *key, char *out, size_t n)
{
    const char *p = body ? strstr(body, key) : NULL;
    size_t i = 0;
    if (!p || !out || n < 2)
        return -1;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t')
        p++;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < n) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return i > 0 ? 0 : -1;
}

static void on_request(sink_t *c, rtsp_msg_t *m);
static void uibc_after_play(sink_t *c);
static void uibc_enable_to_source(sink_t *c, const char *when);
static int send_setup(sink_t *c);   /* connect 模式: 手机喊 trigger=SETUP 后由我们发 SETUP */

/* M2: 问 source 能力 (wfd_video_formats 等)。拨号/被拨两条路都只发一次。
 * 2026-10-24: 之前 connect_mode 下从不发 M2 ⇒ 手机收不到 sink 能力, 不会给
 * presentation_URL / trigger=SETUP, 握手 1.5 s 就断 (18:44 现场)。 */
static int send_m2(sink_t *c)
{
    if (c->sent_m2)
        return 0;
    const char *q =
        "wfd_video_formats\r\n"
        "wfd_audio_codecs\r\n"
        "wfd_client_rtp_ports\r\n"
        "wfd_uibc_capability\r\n"
        "wfd_content_protection\r\n";
    if (send_request(c, "GET_PARAMETER", "rtsp://localhost/wfd1.0", NULL, q) != 0)
        return -1;
    c->sent_m2 = 1;
    c->state = S_WAIT_CAPS;
    LOGI("[%s] ✅ M2 GET_PARAMETER 已发 (问 source 能力)", now_str());
    return 0;
}

static int do_handshake_resp(sink_t *c, rtsp_msg_t *m)
{
    /* 本端发的 M1 OPTIONS 的应答。海奇 p2p0 实证: 应答后 sink **不发** M2,
     * 由手机 (source) 主动发 GET_PARAMETER 来问 sink 能力 —— 谁先问谁驱动。 */
    if (c->state == S_WAIT_OPTIONS_ACK) {
        if (m->code == 200)
            LOGI("[%s] ✅ 手机已确认本端 M1 OPTIONS (200), 等它 GET_PARAMETER 问能力",
                 now_str());
        else
            LOGE("M1 OPTIONS 应答 %d", m->code);
        c->state = S_WAIT_M1;
        return 0;
    }

    /* source 回了能力 (M2 的响应) → 选格式 → 发 M3 */
    if (c->state != S_WAIT_CAPS)
        return 0;

    if (m->code != 200) {
        LOGE("M2 能力查询失败: %d", m->code);
        return -1;
    }

    scan_uibc_caps(m->body);

    if (strstr(m->body, "wfd_video_formats")) {
        LOGI("[%s] ✅ M2 收到 source 能力:", now_str());
        LOGI("%s", m->body);
        int w = 0, h = 0, fps = 0;
        uint32_t bit = pick_format(m->body, &w, &h, &fps);
        LOGI("[%s] 选定格式 %dx%d@%d (CEA bit 0x%08x)", now_str(), w, h, fps, bit);

        char vf[256], body[1024];
        build_video_formats(bit, 0x02 /* CHP */, 0x08 /* L4.1 */, vf, sizeof(vf));
        snprintf(body, sizeof(body),
                 "wfd_video_formats: %s\r\n"
                 "wfd_audio_codecs: LPCM 00000002 00\r\n"
                 "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n"
                 "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d %d mode=play\r\n"
                 "wfd_uibc_capability: %s\r\n",
                 vf, c->peer_ip, c->rtp_video_port, c->rtp_audio_port,
                 uibc_capability());
        if (send_request(c, "SET_PARAMETER", "rtsp://localhost/wfd1.0", NULL, body) != 0)
            return -1;
        c->state = S_WAIT_M3;
        return 0;
    }
    return 0;
}

static int do_m3_resp(sink_t *c, rtsp_msg_t *m)
{
    if (c->state != S_WAIT_M3)
        return 0;
    if (m->code != 200) {
        LOGE("M3 选格式被拒: %d", m->code);
        return -1;
    }
    LOGI("[%s] ✅ M3 选格式完成 → 发 M4 trigger=SETUP", now_str());
    if (send_request(c, "SET_PARAMETER", "rtsp://localhost/wfd1.0", NULL,
                     "wfd_trigger_method: SETUP\r\n") != 0)
        return -1;
    uibc_enable_to_source(c, "M3 200 之后");
    c->state = S_WAIT_M4;
    return 0;
}

static void close_streams(sink_t *c)
{
    uibc_stop();
    g_uibc_started = 0;
    g_uibc_at = 0;
    if (c->udp_v >= 0) { close(c->udp_v); c->udp_v = -1; }
    if (c->udp_a >= 0) { close(c->udp_a); c->udp_a = -1; }
    if (c->udp_a2 >= 0) { close(c->udp_a2); c->udp_a2 = -1; }
    if (c->adump) { fclose(c->adump); c->adump = NULL; }
    if (c->dump) { fclose(c->dump); c->dump = NULL; }
}

static int on_setup(sink_t *c, rtsp_msg_t *m)
{
    char transport[256] = { 0 };
    int client_port = 0;

    if (c->state != S_WAIT_M4 && c->state != S_WAIT_SETUP) {
        LOGD("SETUP 在非预期状态 %d 到达 (仍受理)", c->state);
    }

    get_header(m->headers, "Transport:", transport, sizeof(transport));
    LOGI("[%s] M5 SETUP: %s", now_str(), transport);
    const char *cp = strstr(transport, "client_port=");
    if (cp)
        client_port = atoi(cp + 12);
    c->req_video_port = client_port;

    if (c->rtp_video_port <= 0 || port_in_use(c->rtp_video_port))
        c->rtp_video_port = c->rtp_video_port > 0 ? c->rtp_video_port : DEFAULT_RTP_PORT;
    c->rtp_audio_port = c->rtp_video_port + 2;   /* 音频单独一口 (2026-10-27):
                                                 * WFD 的 wfd_client_rtp_ports 是
                                                 * "<video_port> <audio_port>"; 原来
                                                 * 第二项报 0 = "没有音频口" ⇒ 手机
                                                 * 干脆不发音频 (audio_pkts=0)。
                                                 * 现在给 video+2, 由 udp_a 接。 */

    c->udp_v = udp_open(c->rtp_video_port);
    if (c->udp_v < 0)
        return send_response(c, m->cseq, "500 Internal Server Error", NULL, NULL);
    open_audio_probe_ports(c);

    c->session_id = (int)(time(NULL) & 0x7fffffff);
    char extra[512];
    snprintf(extra, sizeof(extra),
             "Session: %08x;timeout=30\r\n"
             "Transport: RTP/AVP/UDP;unicast;client_port=%d;server_port=%d\r\n",
             (unsigned)c->session_id, client_port, c->rtp_video_port);
    LOGI("[%s] ✅ SETUP 应答: server_port=%d (source client_port=%d)",
         now_str(), c->rtp_video_port, client_port);
    if (send_response(c, m->cseq, "200 OK", extra, NULL) != 0)
        return -1;
    c->state = S_WAIT_PLAY;
    return 0;
}

static int on_play(sink_t *c, rtsp_msg_t *m)
{
    char extra[128];
    snprintf(extra, sizeof(extra), "Session: %08x\r\nRange: npt=now-\r\n",
             (unsigned)c->session_id);
    LOGI("[%s] ✅ M6 PLAY → 200 应答, 开始等 RTP (UDP %d)",
         now_str(), c->rtp_video_port);
    c->state = S_STREAMING;
    c->last_keepalive = time(NULL);
    c->have_last_keepalive = 1;
    int rr = send_response(c, m->cseq, "200 OK", extra, NULL);
    uibc_after_play(c);
    return rr;
}

/* 向 source 声明 UIBC 打开 (SET_PARAMETER wfd_uibc_setting: enable)。
 * 可多次调用 (幂等): 规范时机是 M3 之后, 但实测有些安卓要等 PLAY 完成才起
 * UIBC 服务端, 所以两处都发。 */
static void uibc_enable_to_source(sink_t *c, const char *when)
{
    if (send_request(c, "SET_PARAMETER", "rtsp://localhost/wfd1.0", NULL,
                     "wfd_uibc_setting: enable\r\n") == 0)
        LOGI("[%s] UIBC: 已向 source 声明 wfd_uibc_setting: enable (%s)",
             now_str(), when);
}

/* PLAY 之后启动反控: 先问一次 source 的 UIBC 端口 (server 模式下还没见过),
 * 再告诉它 UIBC 打开, 800 ms 后按拿到的端口建通道 (拿不到就本端监听)。 */
static void uibc_after_play(sink_t *c)
{
    if (g_uibc_started)
        return;
    g_uibc_started = 1;
    g_uibc_at = now_ms() + 800;
    if (g_src_uibc_port <= 0 &&
        send_request(c, "GET_PARAMETER", "rtsp://localhost/wfd1.0", NULL,
                     "wfd_uibc_capability\r\n") == 0)
        LOGI("[%s] UIBC: 已向 source 查询 wfd_uibc_capability", now_str());
    uibc_enable_to_source(c, "PLAY 之后");
}

static void on_request(sink_t *c, rtsp_msg_t *m)
{
    c->rtsp_req++;
    scan_uibc_caps(m->body);
    LOGI("[%s] <== %s %s (CSeq %s)", now_str(), m->method, m->uri, m->cseq);

    if (strcmp(m->method, "OPTIONS") == 0) {
        if (c->connect_mode) {
            /* 逐字照抄海奇 p2p0.pcap: 手机(server)先发 OPTIONS, sink 只回这三项;
             * 紧接着 sink 再发自己的 OPTIONS (M1)。**不发 M2** —— 能力由手机来问。 */
            /* Public = 本端(RTSP server 侧)支持的方法, 必须包含 SETUP/PLAY/PAUSE
             * —— Windows 10 source 会按这张表判断能否发给 sink; 只报 GET/SET
             * 会被严格的 source 判为"不能收流"(2026-10-24 加, 手机对多报不敏感) */
            send_response(c, m->cseq, "200 OK",
                          "Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, "
                          "GET_PARAMETER, SET_PARAMETER\r\n", NULL);
            if (!c->sent_opt) {
                send_request(c, "OPTIONS", "*", "Require: org.wfa.wfd1.0\r\n", NULL);
                c->sent_opt = 1;
                c->opt_sent_ms = now_ms();
                c->state = S_WAIT_OPTIONS_ACK;
            }
            return;
        }
        /* 对端拨入 (本端是 RTSP server): 回全量 Public, 然后发 M2 问能力 */
        send_response(c, m->cseq, "200 OK",
                      "Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, "
                      "GET_PARAMETER, SET_PARAMETER\r\n", NULL);
        if (c->state == S_WAIT_M1) {
            LOGI("[%s] ✅ M1 OPTIONS 完成 → 发 M2 GET_PARAMETER 问 source 能力",
                 now_str());
            if (send_m2(c) != 0)
                LOGE("M2 发送失败: %s", strerror(errno));
        }
        return;
    }

    if (strcmp(m->method, "GET_PARAMETER") == 0) {
        /* keepalive (M16) 或 source 反查 sink 能力 */
        if (strstr(m->body, "wfd_video_formats") || strstr(m->body, "wfd_client_rtp_ports")) {
            char vf[256], body[1024];
            int port = c->rtp_video_port > 0 ? c->rtp_video_port : DEFAULT_RTP_PORT;
            if (c->connect_mode) {
                /* 按请求逐项回答 (WFD 规范: 请求里问到的每个参数都必须回, 不支持的回 none)。
                 * 2026-10-25: Windows 10 会问 wfd_content_protection, 旧实现固定只回 4 项、
                 * 且第二条 wfd_video_formats 少字段 (格式非法) ⇒ Windows 收到能力后**不再发 M3**,
                 * 干等 76 s 后关连接 (实机日志 00:44:06→00:45:22)。 */
                LOGI("[%s] <== GET_PARAMETER 请求体(%zu B):\n%s",
                     now_str(), strlen(m->body), m->body);
                build_caps_body(c, m->body, body, sizeof(body));
                LOGI("[%s] 回能力(%zu B):\n%s", now_str(), strlen(body), body);
            } else {
                /* 广告自身能力 (对端=手机的 Android 会从这里选)
                 * CEA 位 5/6/7 = 720p30 / 720p60 / 1080p30, 与参考实现同款式 */
                build_video_formats(advertised_cea(), 0x02, 0x08, vf, sizeof(vf));
                snprintf(body, sizeof(body),
                         "wfd_video_formats: %s\r\n"
                         "wfd_audio_codecs: LPCM 00000002 00\r\n"
                         "wfd_client_rtp_ports: RTP/AVP/UDP;unicast %d %d mode=play\r\n"
                         "wfd_uibc_capability: %s\r\n",
                         vf, port, adv_audio_port(port), uibc_capability());
            }
            send_response(c, m->cseq, "200 OK", NULL, body);
            /* 回完能力即开始等 M3 (SET_PARAMETER 选格式); 12 s 不来就主动收尾。
             * 注意两种模式都要记: 真手机 (Android/Windows) 都是拨进我们 (server 模式),
             * 只有 connect_mode 才算的话看护永远不触发 —— 2026-10-26 实测踩到。 */
            c->caps_ms = now_ms();
            c->got_m3 = 0;
        } else {
            send_response(c, m->cseq, "200 OK", NULL, NULL);
        }
        return;
    }

    if (strcmp(m->method, "SETUP") == 0) {
        on_setup(c, m);
        return;
    }
    if (strcmp(m->method, "PLAY") == 0) {
        on_play(c, m);
        return;
    }
    if (strcmp(m->method, "SET_PARAMETER") == 0) {
        /* M3: source 用 wfd_video_formats 告诉我们它最终选了哪个格式 —— 必须记日志,
         * 否则"为什么这么卡/为什么只有一条"只能猜。 */
        if (strstr(m->body, "wfd_video_formats")) {
            const char *f = strstr(m->body, "wfd_video_formats:");
            c->got_m3 = 1;
            c->caps_ms = 0;
            LOGI("[%s] M3 选格式: %s", now_str(), f ? f : "(空)");
        }
        /* 抓包里手机给的播放地址 (帧64): wfd_presentation_URL: rtsp://192.168.49.1/wfd1.0/streamid=0 none */
        const char *pu = strstr(m->body, "wfd_presentation_URL:");
        if (pu) {
            pu += strlen("wfd_presentation_URL:");
            while (*pu == ' ') pu++;
            int i = 0;
            while (pu[i] && pu[i] != '\r' && pu[i] != '\n' && pu[i] != ' ' &&
                   i < (int)sizeof(c->presentation_uri) - 1) {
                c->presentation_uri[i] = pu[i];
                i++;
            }
            c->presentation_uri[i] = '\0';
            LOGI("[%s] source 指定播放地址: %s", now_str(), c->presentation_uri);
        }
        /* 2026-10-27: M3 的 200 必须带「已选参数」回执。WFD 1.1 §6.5 规定 sink 在
         * M3 应答里确认 video/audio 格式并带上自己的 wfd_uibc_capability;
         * 老实现回空 body, 实测这台安卓手机因此既不开 UIBC 也不发音频。 */
        {
            static int echo_on = -1;
            char vf[256] = { 0 }, ac[128] = { 0 }, m3body[768];
            if (echo_on < 0)
                echo_on = !(getenv("WFD_M3_ECHO") && !strcmp(getenv("WFD_M3_ECHO"), "0"));
            /* 只有**真正的 M3** (带 wfd_video_formats) 才回执; 手机后续那些只带
             * wfd_uibc_setting 的 SET_PARAMETER 若也回执, 会把 vf 取空写成
             * "wfd_video_formats: none" —— 等于当面告诉手机"我没有任何视频格式"
             * (2026-10-27 K80 场次实际发出去过 210 B 的这种应答)。 */
            if (!echo_on || !strstr(m->body, "wfd_video_formats")) {
                send_response(c, m->cseq, "200 OK", NULL, NULL);
            } else {
                body_get_param(m->body, "wfd_video_formats", vf, sizeof(vf));
                body_get_param(m->body, "wfd_audio_codecs", ac, sizeof(ac));
                snprintf(m3body, sizeof(m3body),
                         "wfd_video_formats: %s\r\n"
                         "wfd_audio_codecs: %s\r\n"
                         "wfd_uibc_capability: %s\r\n"
                         "wfd_content_protection: none\r\n",
                         vf[0] ? vf : "none",
                         ac[0] ? ac : "LPCM 00000002 00",
                         uibc_capability());
                LOGI("[%s] 回 M3 应答体(%zu B):\n%s", now_str(), strlen(m3body), m3body);
                send_response(c, m->cseq, "200 OK", NULL, m3body);
            }
        }
        /* M3 之后声明 UIBC enable —— 但**只在会话已建立时**发。
         * 2026-10-27 K80 实测: 选完格式 (M3) 但还没 SETUP/PLAY 时发裸 SET_PARAMETER,
         * 手机会回 454 (Session Not Found); 虽然老代码也不会因此报错, 但那 454 会被
         * do_setup_resp() 误当成"SETUP 被拒"直接结束会话 (屏黑)。
         * 规范里 wfd_uibc_setting 属于会话内消息, 所以默认推迟到 PLAY 之后
         * (do_play_resp → uibc_after_play 已经会发); 需要复现旧行为用 WFD_UIBC_EARLY=1。 */
        {
            const char *early = getenv("WFD_UIBC_EARLY");
            int allow_early = (early && *early && strcmp(early, "0") != 0);
            if (allow_early && (strstr(m->body, "wfd_uibc_capability") ||
                                strstr(m->body, "wfd_uibc_setting")))
                uibc_enable_to_source(c, "M3 之后");
            else if (strstr(m->body, "wfd_uibc_setting") && !c->playing)
                LOGI("[%s] UIBC: 会话未建立, 推迟 wfd_uibc_setting 到 PLAY 之后"
                     " (K80 会在 SETUP 前回 454)", now_str());
        }
        if (strstr(m->body, "wfd_trigger_method")) {
            const char *tm = strstr(m->body, "wfd_trigger_method:");
            if (tm && strstr(tm, "TEARDOWN")) {
                LOGI("[%s] source 触发 TEARDOWN", now_str());
                g_stop = 1;
            } else if (tm && strstr(tm, "PAUSE")) {
                LOGI("[%s] source 触发 PAUSE", now_str());
            } else if (tm && strstr(tm, "SETUP") && c->connect_mode) {
                /* 手机是服务端, 它只喊「你现在去 SETUP」, 真正的 SETUP/PLAY 得我们发。
                 * 2026-10-28 K80 实测: 收到 trigger 后**立刻**发 SETUP, 约 7% 的轮次
                 * 对端 4 s 内不回 200 并直接关 TCP (17:03/17:21/17:30 三次现场)。
                 * 对端刚处理完我们 200 OK 的那条 SET_PARAMETER, RTSP 线程还没回空闲,
                 * 所以错开 250 ms 再发 (努比亚那条路本来就慢几十 ms, 不受影响)。 */
                send_setup(c);
            }
        }
        return;
    }
    if (strcmp(m->method, "TEARDOWN") == 0) {
        send_response(c, m->cseq, "200 OK", NULL, NULL);
        LOGI("[%s] M8 TEARDOWN, 会话结束", now_str());
        c->state = S_WAIT_M1;
        return;
    }
    send_response(c, m->cseq, "501 Not Implemented", NULL, NULL);
}

/* ============ connect 模式 (手机当 RTSP 服务端) ============ */

static int send_setup(sink_t *c)
{
    if (c->udp_v < 0) {
        c->udp_v = udp_open(c->rtp_video_port);
        if (c->udp_v < 0) {
            LOGE("connect 模式: UDP %d 打不开, 无法收流", c->rtp_video_port);
            return -1;
        }
        LOGI("[%s] UDP %d 已就绪 (等 RTP)", now_str(), c->rtp_video_port);
    }
    open_audio_probe_ports(c);
    char extra[128];
    snprintf(extra, sizeof(extra),
             "Transport: RTP/AVP/UDP;unicast;client_port=%d\r\n", c->rtp_video_port);
    const char *uri = c->presentation_uri[0] ? c->presentation_uri
                                             : "rtsp://localhost/wfd1.0/streamid=0";
    LOGI("[%s] ✅ trigger=SETUP → 发 SETUP (client_port=%d)%s", now_str(), c->rtp_video_port,
         c->setup_tries ? " [重发, 同 CSeq]" : "");
    c->sent_setup = 1;
    c->setup_tries++;
    /* 重发必须复用同一个 CSeq: 换新 CSeq 就成了"第二条 SETUP", 实测 K80 直接
     * 关 TCP (17:21:56 现场)。RTSP 的重复请求语义是 CSeq 相同、内容相同。 */
    c->cseq_pin = c->setup_tries > 1 ? c->setup_cseq : 0;
    int rc = send_request(c, "SETUP", uri, extra, NULL);
    c->setup_cseq = c->server_cseq;
    c->cseq_pin = 0;
    c->setup_ms = now_ms();
    return rc;
}

/* SETUP 的 200: 取 Session, 紧接着发 PLAY (抓包帧72→73) */
static int do_setup_resp(sink_t *c, rtsp_msg_t *m)
{
    if (!c->connect_mode || !c->sent_setup || c->playing)
        return 0;
    /* 只认 SETUP 自己的那个 CSeq: 别的请求 (比如 SETUP 前的 wfd_uibc_setting) 的
     * 应答不能算进来 —— K80 对那条回 454, 老代码因此把会话直接判死 (2026-10-27)。 */
    if (atoi(m->cseq) != c->setup_cseq) {
        LOGI("[%s] (忽略非 SETUP 应答: CSeq %s, 本端 SETUP=%d, code=%d)",
             now_str(), m->cseq, c->setup_cseq, m->code);
        return 0;
    }
    /* 已经收到应答 → 关掉"等 SETUP 200"看护; 后面 PLAY 慢一点不算失败 */
    if (c->setup_ms)
        LOGI("[%s] ⏱ SETUP 应答耗时 %lld ms (对端 rtsp 忙时会更久, 不要缩短看护)",
             now_str(), now_ms() - c->setup_ms);
    c->setup_ms = 0;
    if (m->code != 200) {
        LOGE("SETUP 被拒: %d", m->code);
        return -1;
    }
    char sess[64] = { 0 }, tr[160] = { 0 };
    if (get_header(m->headers, "Session:", sess, sizeof(sess)) != 0 || !sess[0])
        snprintf(sess, sizeof(sess), "0");
    c->session_id = (int)strtoul(sess, NULL, 10);
    if (get_header(m->headers, "Transport:", tr, sizeof(tr)) == 0)
        LOGI("[%s] ✅ SETUP 200: %s", now_str(), tr);
    else
        LOGI("[%s] ✅ SETUP 200 (Session %s)", now_str(), sess);

    char extra[96];
    snprintf(extra, sizeof(extra), "Session: %s\r\n", sess);
    const char *uri = c->presentation_uri[0] ? c->presentation_uri
                                             : "rtsp://localhost/wfd1.0/streamid=0";
    c->play_cseq = c->server_cseq + 1;
    if (send_request(c, "PLAY", uri, extra, NULL) != 0)
        return -1;
    c->playing = 1;
    return 0;
}

static int do_play_resp(sink_t *c, rtsp_msg_t *m)
{
    if (!c->connect_mode || !c->playing || c->state == S_STREAMING)
        return 0;
    if (atoi(m->cseq) != c->play_cseq) {
        LOGI("[%s] (忽略非 PLAY 应答: CSeq %s, 本端 PLAY=%d, code=%d)",
             now_str(), m->cseq, c->play_cseq, m->code);
        return 0;
    }
    if (m->code != 200) {
        LOGE("PLAY 被拒: %d", m->code);
        return -1;
    }
    c->state = S_STREAMING;
    c->last_keepalive = time(NULL);
    c->have_last_keepalive = 1;
    LOGI("[%s] ✅ PLAY 200 → 开始收 RTP (UDP %d)", now_str(), c->rtp_video_port);
    uibc_after_play(c);
    return 0;
}

/* ============ 主循环 ============ */

static int run_session(sink_t *c, int lfd, int timeout_sec, long max_frames)
{
    time_t start = time(NULL);
    time_t t_stat = 0;   /* 上次打会话统计的时刻 (每 5 s 一条) */

    /* 音频出口: 管道写端 (常驻 pcmplay 持读端)。打不开也不影响视频。 */
    audio_out_open(NULL);

    if (c->dump_path) {
        c->dump = fopen(c->dump_path, "wb");
        if (!c->dump)
            LOGE("无法写 dump 文件 %s: %s", c->dump_path, strerror(errno));
        else {
            /* 直通管道时必须无缓冲: 默认 4KB 缓冲会把小 NAL 攒着不发,
             * 下游解码器一直等不到完整帧 (现象: 解码器凑不齐一个 AU)。 */
            if (!strcmp(c->dump_path, "-") || !strcmp(c->dump_path, "/dev/stdout"))
                setvbuf(c->dump, NULL, _IONBF, 0);
            LOGI("[%s] H.264 dump → %s", now_str(), c->dump_path);
        }
    }

    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = c->rtsp_fd;
        FD_SET(c->rtsp_fd, &rfds);
        if (c->udp_v >= 0) {
            FD_SET(c->udp_v, &rfds);
            if (c->udp_v > maxfd) maxfd = c->udp_v;
        }
        if (c->udp_a >= 0) {
            FD_SET(c->udp_a, &rfds);
            if (c->udp_a > maxfd) maxfd = c->udp_a;
        }
        if (c->udp_a2 >= 0) {
            FD_SET(c->udp_a2, &rfds);
            if (c->udp_a2 > maxfd) maxfd = c->udp_a2;
        }
        if (lfd >= 0) {   /* 会话还没开始谈时, 允许切到对端拨入的连接 */
            FD_SET(lfd, &rfds);
            if (lfd > maxfd) maxfd = lfd;
        }

        struct timeval tv = { 0, 200000 };   /* 200ms */
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOGE("select: %s", strerror(errno));
            break;
        }

        /* 每 5 s 一条会话统计 (2026-10-26 加): 黑屏那一轮日志里只有首包和首帧,
         * 之后一片空白 —— 到底是「手机停发」还是「解码/上屏卡住」看不出来。
         * 有这条就能对时间轴判: rtp 不涨 = 手机停发; rtp 涨 frames 不涨 = 拆包;
         * rtp/frames 都涨而屏黑 = 解码/上屏侧 (看 dec.log / dmesg)。 */
        {
            time_t tnow = time(NULL);
            if ((c->pkt_video > 0 || c->frames > 0) && tnow - t_stat >= 5) {
                long tn = 0;
                ts_demux_stats_t tss;
                memset(&tss, 0, sizeof(tss));
                if (c->ts) {
                    ts_demux_get_stats(c->ts, &tss);
                    tn = (long)tss.nals;
                }
                double drift = 0, wall = 0, media = 0;
                if (c->ts_ref_set) {
                    wall = mono_ms() - c->ts_ref_ms;
                    media = (int32_t)(c->last_rtp_ts - c->ts_ref_rtp) / 90.0;
                    drift = wall - media;
                }
                LOGI("[%s] 统计: rtp=%ld bytes=%ld frames=%ld kf=%ld nals=%ld lost=%ld ts_nals=%ld res=%dx%d drift=%+.0fms (墙钟 %.0f / 媒体 %.0f) audio_pkts=%ld audio_kb=%llu ts_audio_pid=%d ts_audio_pes=%lu ts_audio_kb=%lu",
                     now_str(), c->pkt_video, c->bytes_video, c->frames, c->keyframes,
                     c->nals, c->seq_lost, tn, c->sps_w, c->sps_h, drift, wall, media,
                     c->pkt_audio, audio_out_bytes() / 1024,
                     tss.audio_pid, tss.audio_pes, tss.audio_bytes / 1024);
                t_stat = tnow;
            }
        }

        if (r > 0) {
            if (c->udp_v >= 0 && FD_ISSET(c->udp_v, &rfds)) {
                unsigned char buf[65536];
                for (int i = 0; i < 64; i++) {   /* 一次最多排 64 个包, 防饿死 RTSP */
                    ssize_t n = recv(c->udp_v, buf, sizeof(buf), MSG_DONTWAIT);
                    if (n <= 0)
                        break;
                    handle_rtp(c, buf, (int)n);
                }
            }
            if (c->udp_a >= 0 && FD_ISSET(c->udp_a, &rfds)) {
                unsigned char buf[65536];
                for (int i = 0; i < 64; i++) {
                    ssize_t n = recv(c->udp_a, buf, sizeof(buf), MSG_DONTWAIT);
                    if (n <= 0)
                        break;
                    handle_rtp_ex(c, buf, (int)n, c->udp_a0);
                }
            }
            if (c->udp_a2 >= 0 && FD_ISSET(c->udp_a2, &rfds)) {
                unsigned char buf[65536];
                for (int i = 0; i < 64; i++) {
                    ssize_t n = recv(c->udp_a2, buf, sizeof(buf), MSG_DONTWAIT);
                    if (n <= 0)
                        break;
                    handle_rtp_ex(c, buf, (int)n, c->udp_a20);
                }
            }
            /* 2026-10-22: 双方都在拨/都在听时会出现两条 TCP。若当前这条还没换过任何
             * RTSP 报文 (rtsp_req/rtsp_rsp 都 0), 而对端又拨进来了, 以对端拨入的为准 ——
             * 对端主动拨进来, 说明它那边把自己当 server, 再在我们的连接上谈会两边错位。 */
            if (lfd >= 0 && FD_ISSET(lfd, &rfds) &&
                c->state == S_WAIT_M1 && c->rtsp_req == 0 && c->rtsp_rsp == 0) {
                char nip[64] = { 0 };
                int nfd = accept_nb(lfd, nip, sizeof(nip));
                if (nfd >= 0) {
                    LOGI("[%s] 对端同时拨入 (%s), 放弃本端发起的连接, 改用对端拨入的", now_str(), nip);
                    close(c->rtsp_fd);
                    c->rtsp_fd = nfd;
                    snprintf(c->peer_ip, sizeof(c->peer_ip), "%s", nip);
                    c->connect_mode = 0;
                    c->server_cseq = 0;
                    continue;
                }
            }

            if (FD_ISSET(c->rtsp_fd, &rfds)) {
                rtsp_msg_t m;
                if (read_message(c->rtsp_fd, &m) != 0) {
                    LOGI("[%s] RTSP 连接关闭", now_str());
                    break;
                }
                if (m.code) {           /* 响应 */
                    c->rtsp_rsp++;
                    LOGI("[%s] ← 响应 %d (CSeq %s, body %d 字节)",
                         now_str(), m.code, m.cseq[0] ? m.cseq : "?", m.body_len);
                    if (do_handshake_resp(c, &m) != 0)
                        break;
                    if (do_m3_resp(c, &m) != 0)
                        break;
                    if (do_setup_resp(c, &m) != 0)
                        break;
                    if (do_play_resp(c, &m) != 0)
                        break;
                } else {
                    on_request(c, &m);
                }
            }
        }

        /* M1 OPTIONS 兜底: 手机迟迟不发 OPTIONS 时 (部分固件等 sink 先开口), 2 s 后本端发 */
        if (c->connect_mode && !c->sent_opt && c->conn_ms &&
            now_ms() - c->conn_ms >= 2000) {
            LOGI("[%s] 对端 2 s 未发 OPTIONS → 本端先发 M1", now_str());
            if (send_request(c, "OPTIONS", "*", "Require: org.wfa.wfd1.0\r\n", NULL) != 0)
                break;
            c->sent_opt = 1;
            c->opt_sent_ms = now_ms();
            c->state = S_WAIT_OPTIONS_ACK;
        }

        /* UIBC 反控: PLAY 之后 800 ms 起 (给 wfd_uibc_capability 应答留时间) */
        if (g_uibc_at && now_ms() >= g_uibc_at) {
            g_uibc_at = 0;
            uibc_start(c->peer_ip, g_src_uibc_port, uibc_port());
        }

        /* keepalive: 会话建立后每 20s 发一次 M16 */
        if (c->state == S_STREAMING && c->have_last_keepalive &&
            time(NULL) - c->last_keepalive >= KEEPALIVE_SEC) {
            c->last_keepalive = time(NULL);
            send_request(c, "GET_PARAMETER", "rtsp://localhost/wfd1.0", NULL, NULL);
        }

        /* M3 看护 (2026-10-26): 回完能力 12 s 还没收到 M3 (选格式) ⇒ 对端会话卡住,
         * 主动 TEARDOWN 收尾。原来只能干等对端 20~25 s 自己关 TCP, 期间 7236 被占、
         * 管线空转, 用户点第二次也进不来 (K80 要试 4 次的现象之一)。 */
        if (c->caps_ms && !c->got_m3 && !c->sent_setup &&
            now_ms() - c->caps_ms >= M3_TIMEOUT_MS) {
            LOGI("[%s] ⏱ 回完能力 %lld ms 仍无 M3 → 主动 TEARDOWN 收尾 (对端卡住, 不占着 7236)",
                 now_str(), now_ms() - c->caps_ms);
            send_request(c, "TEARDOWN", "rtsp://localhost/wfd1.0",
                         "Require: org.wfa.wfd1.0\r\n", NULL);
            c->caps_ms = 0;
            break;
        }

        /* SETUP 无应答看护 (2026-10-26): 对端收下 M3/M4 却不回 SETUP 200 时,
         * 干等只能等到它自己 29 s 断开。这里 4 s 就断, 让外层立刻重拨一次。 */
        if (c->connect_mode && c->sent_setup && !c->playing && c->setup_ms &&
            now_ms() - c->setup_ms >= setup_ack_timeout_ms()) {
            /* 2026-10-28 18:12 实测: 这里**绝不能发 TEARDOWN**。发了之后手机直接
             * 退出 P2P 组 (现场: 18:12:09 TEARDOWN → 18:12:13 wfd-p2p.sh 重拉
             * wfd-sink 时已经读不到 192.168.49.1, 只能退回单模监听, 之后 95 秒
             * 手机在组外一句话不发 ⇒ 屏黑)。只关 TCP, 让外层立刻双模重拨。 */
            LOGI("[%s] ⏱ SETUP 发出 %lld ms 仍无 200 → 断开重拨 (对端卡住, 不干等)",
                 now_str(), now_ms() - c->setup_ms);
            c->setup_ms = 0;
            break;
        }

        if (max_frames > 0 && c->frames >= max_frames) {
            LOGI("[%s] 已收到 %ld 帧, 达到 -n 上限, 退出", now_str(), c->frames);
            break;
        }
        if (timeout_sec > 0 && time(NULL) - start >= timeout_sec) {
            LOGI("[%s] 到达 -t %d 秒上限, 退出", now_str(), timeout_sec);
            break;
        }
    }
    return 0;
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* 非阻塞拨号: 立刻返回 fd, 连没连上由 establish_session 的 poll(POLLOUT)+SO_ERROR 判定 */
static int tcp_connect_nb(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_nb(int lfd, char *peer_ip, int peer_len)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0)
        return -1;
    if (peer_ip && peer_len > 0)
        inet_ntop(AF_INET, &peer.sin_addr, peer_ip, (socklen_t)peer_len);
    return fd;
}

/* 对端是否已经在发数据 (用来在"双向同时建链"时挑对端先开口的那条) */
static int has_pending(int fd)
{
    char ch;
    ssize_t n = recv(fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0;
}

/* 双模建链: 同时监听 :7236 和主动拨对端 :7236, 谁先通就用谁。
 * 2026-10-22 加: 参考实现 captures/p2p0.pcap 里 TCP 是 sink 侧(SYN 带 49562 源端口)发起的,
 * 但手机当 RTSP server 还是 client 随固件不同; 写死一种必有一边死等 ⇒ 两种同时做。
 * 优先级: 两边同一轮就绪时, 采用"已经在发数据"的那条 (对端拨入优先), 避免出现双连接各谈一半。 */
static int establish_session(sink_t *c, int lfd, int listen_port, const char *connect_to,
                             int timeout_sec, time_t t_start)
{
    char ip[64] = { 0 };
    int port = DEFAULT_RTSP_PORT;
    int dialfd = -1;
    time_t next_dial = 0;
    /* 2026-10-31 quirk (默认 0 = 黄金行为): 组刚建好就拨, 有些手机 (K80) 的
     * RTSP server 还没起来 —— TCP 拨得通、M1~M4 也收下, 但 SETUP 一直不回
     * (现场 `SETUP 发出 4008 ms 仍无 200`)。延后首拨给它一点启动时间。
     * 只影响"首次拨号时刻", 不动任何协议内容。WFD_DIAL_DELAY_MS=<ms> 开启。 */
    long dial_delay_ms = 0;
    {
        const char *o = getenv("WFD_DIAL_DELAY_MS");
        if (o && *o) dial_delay_ms = strtol(o, NULL, 0);
    }
    long long first_dial_at = 0;
    if (dial_delay_ms > 0) first_dial_at = now_ms() + dial_delay_ms;

    if (connect_to) {
        snprintf(ip, sizeof(ip), "%s", connect_to);
        char *colon = strrchr(ip, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        LOGI("[%s] 双模建链: 监听本机 :%d + 主动拨 %s:%d (谁先通谁赢)",
             now_str(), listen_port, ip, port);
    }

    while (!g_stop) {
        if (connect_to && dialfd < 0 && time(NULL) >= next_dial &&
            (first_dial_at == 0 || now_ms() >= first_dial_at)) {
            dialfd = tcp_connect_nb(ip, port);
            if (dialfd < 0)
                next_dial = time(NULL) + 2;
        }

        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = lfd;
        pfd[n].events = POLLIN;
        n++;
        if (dialfd >= 0) {
            pfd[n].fd = dialfd;
            pfd[n].events = POLLOUT;
            n++;
        }

        int r = poll(pfd, (nfds_t)n, 200);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOGE("poll: %s", strerror(errno));
            break;
        }

        if (r > 0) {
            int lfd_rdy = (pfd[0].revents & POLLIN) != 0;
            int dial_rdy = (n > 1 && (pfd[1].revents & (POLLOUT | POLLERR | POLLHUP)) != 0);

            if (dial_rdy) {
                int err = 0;
                socklen_t el = sizeof(err);
                getsockopt(dialfd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err == 0) {
                    /* 拨通了; 若这轮监听侧也有人连进来且已经在发 RTSP, 让给对端 */
                    if (lfd_rdy) {
                        int t = accept_nb(lfd, c->peer_ip, sizeof(c->peer_ip));
                        if (t >= 0 && has_pending(t)) {
                            LOGI("[%s] 双向同时建链: 采用对端拨入的连接 (对端已在发 RTSP)", now_str());
                            close(dialfd);
                            c->connect_mode = 0;
                            return t;
                        }
                        if (t >= 0)
                            close(t);
                    }
                    LOGI("[%s] ✅ 已拨通对端 RTSP %s:%d (本端作 RTSP client, 也会应答对端请求)",
                         now_str(), ip, port);
                    /* 2026-10-24: 拨号成功也必须记对端 IP —— M3 的 wfd_presentation_URL
                     * 要用它拼, 之前为空会发成 rtsp:///wfd1.0/streamid=0 */
                    snprintf(c->peer_ip, sizeof(c->peer_ip), "%s", ip);
                    c->connect_mode = 1;
                    return dialfd;
                }
                close(dialfd);
                dialfd = -1;
                next_dial = time(NULL) + 2;
            }

            if (lfd_rdy) {
                int fd = accept_nb(lfd, c->peer_ip, sizeof(c->peer_ip));
                if (fd >= 0) {
                    LOGI("[%s] ✅ 对端拨入 %s (本端作 RTSP server, 等对端发 M1)",
                         now_str(), c->peer_ip);
                    if (dialfd >= 0)
                        close(dialfd);
                    c->connect_mode = 0;
                    return fd;
                }
            }
        }

        if (timeout_sec > 0 && time(NULL) - t_start >= timeout_sec)
            break;
    }

    if (dialfd >= 0)
        close(dialfd);
    return -1;
}

int main(int argc, char **argv)
{
    int rtsp_port = DEFAULT_RTSP_PORT;
    int rtp_port = DEFAULT_RTP_PORT;
    int rtp_set = 0;
    const char *connect_to = NULL;   /* -c <GO-ip>[:port] → RTSP 客户端模式 */
    const char *dump = NULL;
    int timeout_sec = 0;
    long max_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            rtsp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rtp_port = atoi(argv[++i]);
            rtp_set = 1;
        }
        else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--connect") == 0) && i + 1 < argc)
            connect_to = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            timeout_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            max_frames = atol(argv[++i]);
        else if (strcmp(argv[i], "-v") == 0)
            g_verbose = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-c peer-ip[:port]] [-p rtsp_port] [-r rtp_port] [-d dump.h264] "
                   "[-t seconds] [-n frames] [-v]\n"
                   "  \u65e0 -c: \u4ec5\u76d1\u542c :7236, \u7b49 source \u62e8\u5165\n"
                   "  \u6709 -c: \u53cc\u6a21 \u2014\u2014 \u540c\u65f6\u76d1\u542c :7236 \u5e76\u4e3b\u52a8\u62e8 peer:7236, \u8c01\u5148\u901a\u7528\u8c01\n"
                   "         (\u53c2\u8003\u5b9e\u73b0 captures/p2p0.pcap: TCP \u7531 sink \u4fa7\u53d1\u8d77)\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (connect_to && !rtp_set)
        rtp_port = CONNECT_RTP_PORT;

    /* 2026-10-22: listener **无条件**创建 —— 即使给了 -c 也要能被对端拨进来。
     * 参考实现里 TCP 方向随手机固件会变, 只做一种必有一边死等 (见 establish_session 注释)。 */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        LOGE("socket: %s", strerror(errno));
        return 1;
    }
    {
        int on = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)rtsp_port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            LOGE("bind TCP %d 失败: %s", rtsp_port, strerror(errno));
            return 1;
        }
        listen(lfd, 4);
        int fl = fcntl(lfd, F_GETFL, 0);
        fcntl(lfd, F_SETFL, fl | O_NONBLOCK);
    }
    if (connect_to)
        LOGI("[%s] WFD Sink 就绪: 监听 RTSP %d (双模) + 主动拨 %s, RTP 起始端口 %d",
             now_str(), rtsp_port, connect_to, rtp_port);
    else
        LOGI("[%s] WFD Sink 就绪: 监听 RTSP %d, RTP 起始端口 %d",
             now_str(), rtsp_port, rtp_port);

    /* 只服务一个 source (测试足够); 结束后打印汇总 */
    sink_t c;
    memset(&c, 0, sizeof(c));
    c.rtsp_fd = -1;
    c.udp_v = c.udp_a = c.udp_a2 = -1;
    c.audio_pcm_env = (getenv("WFD_PCM_LE") && !strcmp(getenv("WFD_PCM_LE"), "1"));
    {
        const char *ad = getenv("WFD_AUDIO_DUMP");
        if ((!ad || !*ad) && access("/run/k230-audio-dump", F_OK) == 0)
            ad = "/run/k230-audio.raw";        /* 取证开关: touch /run/k230-audio-dump */
        if (ad && *ad) {
            const char *mx = getenv("WFD_AUDIO_DUMP_MAX_MB");
            long mb = (mx && *mx) ? atol(mx) : 32;
            if (mb <= 0)
                mb = 32;
            c.adump_max = (unsigned long long)mb * 1024ULL * 1024ULL;
            c.adump = fopen(ad, "wb");
            if (c.adump)
                LOGI("[%s] 音频 ES dump → %s (上限 %ld MB, 到顶自动截断)", now_str(), ad, mb);
        }
    }
    c.rtp_video_port = rtp_port;
    c.rtp_audio_port = rtp_port + 2;
    c.connect_mode = connect_to ? 1 : 0;
    c.dump_path = dump;
    c.fu = malloc(FU_BUF_SIZE);
    c.last_seq = -1;
    c.pt96_audio = -1;
    c.ts = ts_demux_new(on_ts_nal, &c);
    if (!c.ts)
        LOGE("ts_demux_new 失败: MPEG-TS (PT=33) 将无法解码");
    else
        ts_demux_set_audio_cb(c.ts, on_ts_audio, &c);

    time_t t_start = time(NULL);
    while (!g_stop) {
        /* 双模建链: 监听 + 拨号并行, 谁先通谁赢 */
        int fd = establish_session(&c, lfd, rtsp_port, connect_to, timeout_sec, t_start);
        if (fd < 0) {
            LOGI("[%s] 建链失败/超时, 退出", now_str());
            break;
        }

        /* 会话参数复位 (RTSP 连接重建时端口沿用) */
        c.rtsp_fd = fd;
        c.state = S_WAIT_M1;
        c.server_cseq = 0;
        c.session_id = 0;
        c.req_video_port = 0;
        c.fu_len = 0;
        c.rtsp_req = c.rtsp_rsp = 0;
        c.pkt_video = c.pkt_audio = c.bytes_video = 0;
        c.frames = c.keyframes = c.nals = c.marker_count = 0;
        c.seq_lost = 0;
        c.last_seq = -1;
        c.pt96_audio = -1;
        c.saw_sps = c.saw_pps = 0;
        c.sps_w = c.sps_h = 0;
        c.sent_setup = 0;
        c.setup_tries = 0;
        c.setup_ms = 0;
        c.caps_ms = 0;
        c.got_m3 = 0;
        c.sent_opt = 0;
        c.sent_m2 = 0;
        c.opt_sent_ms = 0;
        c.conn_ms = now_ms();
        c.playing = 0;
        c.presentation_uri[0] = '\0';

        /* 2026-10-24 (p2p0 逐字节): 拨通后**不抢发** OPTIONS —— 手机 0.5 s 内自己发,
         * sink 先回 200 再发自己的 OPTIONS (见 on_request)。手机不发时 2 s 兜底。 */

        run_session(&c, lfd, timeout_sec, max_frames);
        if (c.ts)
            ts_demux_flush(c.ts);   /* 把最后一个 PES/NAL 吐给解码器 */
        close_streams(&c);
        close(fd);
        c.rtsp_fd = -1;
        if (timeout_sec > 0 || max_frames > 0 || g_stop)
            break;
        /* 这一轮一个 RTP 包都没收到 ⇒ 对端卡住/秒断: 重拨前先停 1 s。
         * 不停的话手机断开瞬间会一秒内重拨几十次 (13:17:18 现场),
         * 把外层 pipeline 的重试额度烧光 ⇒ 之后连上也没人应答。 */
        if (c.pkt_video == 0)
            sleep(1);
        LOGI("[%s] 会话结束, 继续等下一个 source", now_str());
    }

    LOGI("===== SUMMARY =====");
    LOGI("rtsp_req=%ld rtsp_rsp=%ld", c.rtsp_req, c.rtsp_rsp);
    LOGI("rtp_video_pkts=%ld bytes=%ld audio_pkts=%ld",
         c.pkt_video, c.bytes_video, c.pkt_audio);
    LOGI("frames=%ld keyframes=%ld nals=%ld marker_pkts=%ld sps=%d pps=%d",
         c.frames, c.keyframes, c.nals, c.marker_count, c.saw_sps, c.saw_pps);
    LOGI("rtp_seq_lost=%ld", c.seq_lost);
    if (c.ts) {
        ts_demux_stats_t ts;
        ts_demux_get_stats(c.ts, &ts);
        LOGI("ts: pkts=%lu pes=%lu nals=%lu es_bytes=%lu cc_err=%lu video_pid=%d audio_pid=%d audio_pes=%lu audio_bytes=%lu",
             ts.ts_pkts, ts.pes_pkts, ts.nals, ts.bytes, ts.cc_errors,
             ts.video_pid, ts.audio_pid, ts.audio_pes, ts.audio_bytes);
        for (int i = 0; i < ts.es_count; i++)
            LOGI("ts_es[%d]: type=0x%02x pid=%d pkts=%lu bytes=%lu",
                 i, ts.es[i].stream_type, ts.es[i].pid,
                 ts.es[i].pkts, ts.es[i].bytes);
    }
    for (int i = 0; i < c.pt_seen_n; i++)
        LOGI("rtp_pt[%d]: udp=%d PT=%d pkts=%ld",
             i, c.pt_seen[i].port, c.pt_seen[i].pt, c.pt_seen[i].n);
    if (c.sps_w && c.sps_h)
        LOGI("stream_resolution=%dx%d", c.sps_w, c.sps_h);
    int ok = (c.pkt_video > 0 && c.saw_sps && c.saw_pps && c.frames > 0);
    LOGI("RESULT: %s", ok ? "PASS" : "FAIL");

    if (lfd >= 0)
        close(lfd);
    free(c.fu);
    ts_demux_free(c.ts);
    free(g_nal);
    return ok ? 0 : 1;
}
