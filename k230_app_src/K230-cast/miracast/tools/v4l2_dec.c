/*
 * v4l2_dec.c — 最小 V4L2 M2M H.264 硬解码器 (K230 mvx / Linlon v5276)
 *
 * 为什么自己写:
 *   厂商 sample `mvx_decoder` 在 K230 上解不出帧 (采集缓冲 length=[1,1,1],
 *   输入整文件一次性带 EOS)。驱动本身支持解码 (v4l2_probe 实测 S_FMT
 *   H264(OUTPUT) + NV12(CAPTURE) 全通过, sizeimage 数值正确), 所以按标准
 *   V4L2 M2M 姿势自己实现: 先 S_FMT 两个口 → REQBUFS mplane → 按帧喂码流
 *   → DQBUF 收集 → 最后 VIDIOC_DECODER_CMD(V4L2_DEC_CMD_STOP) 收尾。
 *
 * 编译 (板上直接编, 只需内核头):
 *   gcc -O2 -o v4l2_dec v4l2_dec.c
 *
 * 用法:
 *   ./v4l2_dec <in.h264> [out.yuv] [-d /dev/video0] [-w 1280 -h 720] [-v]
 *   不给 out.yuv 时不落盘 (只跑通 + 报帧数/帧率, 用于投屏直通)
 *
 * 输出: NV12 (Y 平面 + 交织 UV 平面), 每帧 w*h*3/2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

/* 日志带墙钟时间 (2026-10-26 加): dec 的 stderr 落在 miracast-dec.log,
 * 以前没有时间戳 ⇒ 手机某一次投屏黑屏时根本对不上是哪一段。 */
static void dlog(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    va_list ap;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#ifndef V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT
#define V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT 0x08000000
#endif
#ifndef V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC
#define V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC  0x07000000
#endif
/* ★ 2026-10-28: 这两组标志**有新旧两套**, 本板固件用的是新的一套 ——
 *   老: FRAME_PRESENT=0x08000000, NEED_REALLOC=0x07000000
 *   新: FRAME_PRESENT=0x10000000, NEED_REALLOC=0x20000000
 *   (SDK 里 mvx-v4l2-controls.h 两套都写着, 驱动实际下发的是新的)
 *   只认老的后果: 驱动要求重分配时我们当成「空缓冲, 等 source」, 永远不重建,
 *   AirPlay 的 Mac 流就这样一直 0 帧 (DQBUF 回 flags=0x20004001)。
 *   取证: 用 LD_PRELOAD 追踪厂商 mvx_decoder, 它拿到的正是 0x20004001,
 *   随后 G_FMT→STREAMOFF(capture)→REQBUFS(0/10)→STREAMON 就出帧了。 */
#define V4L2_BUF_FLAG_MVX_FRAME_PRESENT_V2  0x10000000
#define V4L2_BUF_FLAG_MVX_NEED_REALLOC_V2   0x20000000

/* 判「本缓冲里真有帧」/「驱动要求重分配」—— 新旧两套标志都认 */
static inline int mvx_present(unsigned f)
{
    return (f & (V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT |
                 V4L2_BUF_FLAG_MVX_FRAME_PRESENT_V2)) != 0;
}
static inline int mvx_need_realloc(unsigned f)
{
    return (f & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) == V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC ||
           (f & V4L2_BUF_FLAG_MVX_NEED_REALLOC_V2) == V4L2_BUF_FLAG_MVX_NEED_REALLOC_V2;
}
/* ★ 关键: 告诉固件「这个输入缓冲的码流正好在一帧边界结束」。
 *   不设它, mvx 固件认为后面还会接着来 (MVX 没有 EOF), 只解出第一段就挂住 ——
 *   实测 87 帧的流只出 26 帧然后驱动 watchdog 打 "State change pending"。
 *   驱动里 V4L2_BUF_FLAG_END_OF_SUB_FRAME(0xC2000000) → MVE_BUFFER_BITSTREAM_FLAG_ENDOFSUBFRAME。 */
#ifndef V4L2_BUF_FLAG_END_OF_SUB_FRAME
#define V4L2_BUF_FLAG_END_OF_SUB_FRAME 0xC2000000
#endif

/* MAX_BUF 6 -> 12: 厂商 mvx_decoder 在「驱动要求重分配」之后要 REQBUFS(10),
 * 只给 4 个缓冲时重建完 STREAMON 直接失败 (干净环境实测: 一帧不出)。 */
#define MAX_BUF 24
/* 一个输入缓冲装多少码流: 64KB 在 8Mbps 下约 65ms, 解码 60fps 无压力, 延迟又够低 */
#define CHUNK_DEFAULT (64 * 1024)
#define CHUNK_PIPE    (32 * 1024)

typedef struct {
    void    *addr[VIDEO_MAX_PLANES];
    size_t   len[VIDEO_MAX_PLANES];
    unsigned num_planes;
} buf_t;

static int g_verbose;
/* 调试旋钮 (env 可覆盖): DEC_FLAG 0=不设标志 1=END_OF_FRAME 2=END_OF_SUB_FRAME 3=两者
 * DEC_AU 1=一个 AU 一投 (低延迟) 0=攒满 chunk 再切 (多帧一缓冲) */
static int g_flag = 1;
static int g_perau = 0;
static int g_cfg;      /* 1 = 第一个输入缓冲带 V4L2_BUF_FLAG_MVX_CODEC_CONFIG */
static int g_cfg_sent;
static int g_hdr;      /* 1 = 输出每帧前加 16 字节头 (magic/w/h/len), 给 drmplay -H 用 */

/* ---- 共享内存帧输出 (2026-10-28 零拷贝) ----
 * 帧体写 /run/wfd-frame 的两个槽 (mmap tmpfs), 管道里只留 16 B 帧头 + 4 B 槽号。
 * 为什么: 720p NV12 一帧 1.35 MB, 走管道要 dec 写一遍 + drmplay 读一遍, 单核实测
 *   「读管道」4.4~6 ms, 整圈 19~20 ms 卡在 50 fps。
 * ⚠ 槽大小/帧头长度必须和 drmplay.c 的 SHM_HEAD / SHM_SLOT 一致。 */
#define DEC_SHM_HEAD 64
#define DEC_SHM_SLOT (4u << 20)
static uint8_t *g_shm = NULL;        /* 共享内存映射基址 (NULL = 帧体走管道) */
static FILE *g_fsig = NULL;          /* 帧头/槽号走这个 (真正的管道) */
static FILE *g_mem[2] = { NULL, NULL };  /* 两个槽的写流 */
static unsigned g_slot = 0;
static uint32_t g_shm_seq = 0;
/* 槽头 (共享内存前 8 字节): [0]=写序号 (dec 发布), [4]=读序号 (drmplay 消费完发布)。
 * 为什么必须回压: 管道只剩 20 B/帧后, dec 再也不被写阻塞 ⇒ 实测帧龄一路涨到 5.4 s
 *   (dec 56 fps、源端 60 fps, 每秒多囤 4 帧, 全堆在 AU 拼包缓冲里)。
 *   dec 最多领先 1 帧 (2 槽交替), 就恢复了原来管道背压的效果。 */
static volatile uint32_t *g_shm_w = NULL, *g_shm_r = NULL;
static int g_yuyv;     /* 1 = 采集口输出 YUYV (4:2:2) 而不是 NV12 —— K230 的 VGLite 只吃 YUYV (gcFEATURE_VG_YUV_INPUT=0, YUY2_INPUT=1) */
static unsigned g_dsl_w, g_dsl_h;   /* -s WxH: VPU 硬件缩放 (MVE DEC_DOWNSCALE), 解码器直接出缩放后的图 */

/* mvx 自定义 ioctl (见 SDK 内核 drivers/media/platform/canaan/vpu/mvx-v4l2-controls.h):
 *   VIDIOC_S_MVX_DSL_FRAME  = _IOWR('V', BASE_VIDIOC_PRIVATE+6, struct v4l2_mvx_dsl_frame)
 *   BASE_VIDIOC_PRIVATE = 192, struct = { unsigned width, height } */
struct mvx_dsl_frame { unsigned int width; unsigned int height; };
#define VIDIOC_S_MVX_DSL_FRAME _IOWR('V', 192 + 6, struct mvx_dsl_frame)

/* VIDIOC_S_MVX_DEC_DST_CROP = _IOWR('V', 192+17, struct v4l2_mvx_crop_cfg)
 * 结构 = { unsigned char crop_en; unsigned short x, y, width, height; } */
struct mvx_crop_cfg {
    unsigned char crop_en;
    unsigned short x, y, width, height;
};
#define VIDIOC_S_MVX_DEC_DST_CROP _IOWR('V', 192 + 17, struct mvx_crop_cfg)

static unsigned g_crop_x, g_crop_y, g_crop_w, g_crop_h;   /* -c x y w h: VPU 硬件裁剪 */
static int g_rgb;                    /* -R: 采集口直接出 AR24 (VPU 硬件 YUV→RGB) */
static int g_lowdelay;               /* -D: 低延迟 (关帧重排 + 采集缓冲减到 2) */
static const char *g_learn;          /* -L <file>: 探测内容区并存文件, 下轮给 -c/-s 用 */
static int g_lb_x, g_lb_y, g_lb_x2, g_lb_y2, g_lb_n;   /* 学习到的内容区 (并集) */
static int g_fit_loaded;   /* 已经用了学习文件里的内容区 ⇒ 本会话不再学习 (否则会把结果冲掉) */
static int g_autofit;      /* -A: 按码流尺寸自动算 contain 尺寸并硬件缩放 (默认关: 输出原分辨率, 由下游裁/缩) */
static int g_restart;   /* 已经带硬件裁剪重启过 */

/* ===== 内容区探测 (与 drmplay 的 detect_bbox_nv12 同一算法) ===== */
static int detect_bbox_nv12(const uint8_t *yp, int sw, int sh,
                            int *bx, int *by, int *bw, int *bh)
{
    const int TH = 24;
    int minx = sw, maxx = -1, miny = sh, maxy = -1;
    for (int y = 0; y < sh; y += 2) {
        const uint8_t *row = yp + (size_t)y * sw;
        int first = -1, last = -1;
        for (int x = 0; x < sw; x += 4) {
            if (row[x] > TH) { if (first < 0) first = x; last = x; }
        }
        if (first >= 0) {
            if (first < minx) minx = first;
            if (last > maxx) maxx = last;
            if (y < miny) miny = y;
            maxy = y;
        }
    }
    if (maxx < 0 || maxy < 0) return 0;
    minx = minx - 4 > 0 ? minx - 4 : 0;
    maxx = maxx + 4 < sw ? maxx + 4 : sw - 1;
    miny = miny - 2 > 0 ? miny - 2 : 0;
    maxy = maxy + 2 < sh ? maxy + 2 : sh - 1;
    if (maxx - minx < sw / 5 || maxy - miny < sh / 5) return 0;
    if (maxx - minx > sw * 9 / 10 && maxy - miny > sh * 9 / 10) return 0;
    *bx = minx; *by = miny; *bw = maxx - minx + 1; *bh = maxy - miny + 1;
    return 1;
}

/* ---- 解码延迟探针 (2026-10-28) ----
 * 为什么加: drmplay 的"管道延迟"是从 **解码完成** 算起的, 解码本身耗时之前完全
 * 看不见。而端到端 = 手机(触控→采集→编码→网络) + **解码** + 管道+上屏。
 * 这里在"把一个完整 AU 喂进解码器"时压一个时刻, 取出对应帧时做差, 就能把解码
 * 那一段单独量出来 (V4L2 M2M 顺序进出, 先进先出即可对应)。
 * WFD_DEC_LAT=1 才开, 生产不设就没有任何额外开销。 */
static double g_feed_ts[256];
static int g_feed_h = 0, g_feed_t = 0;
static int g_dec_lat = -1;
static double g_dl_sum = 0, g_dl_max = 0;
static int g_dl_n = 0;

static void emit_hdr(FILE *f, unsigned w, unsigned h)
{
    if (g_dec_lat < 0) {
        const char *e = getenv("WFD_DEC_LAT");
        g_dec_lat = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (g_dec_lat && g_feed_t < g_feed_h) {
        struct timespec dts;
        clock_gettime(CLOCK_MONOTONIC, &dts);
        double now = (double)dts.tv_sec + dts.tv_nsec / 1e9;
        double d = (now - g_feed_ts[g_feed_t & 255]) * 1000.0;
        g_feed_t++;
        if (d >= 0 && d < 2000) {
            g_dl_sum += d; g_dl_n++;
            if (d > g_dl_max) g_dl_max = d;
            if (g_dl_n >= 60) {
                fprintf(stderr, "[dec] 解码延迟 avg %.1f ms max %.0f ms\n",
                        g_dl_sum / g_dl_n, g_dl_max);
                fflush(stderr);
                g_dl_sum = 0; g_dl_max = 0; g_dl_n = 0;
            }
        }
    }
    if (!g_hdr)
        return;
    /* 共享内存模式: 帧头必须走真正的管道 (帧体才写槽), len 最高位置 1 当标志 */
    if (g_fsig)
        f = g_fsig;
    unsigned char hd[16];
    uint16_t ww = (uint16_t)w, hh = (uint16_t)h;
    uint32_t len = g_rgb ? (w * h * 4) : (g_yuyv ? (w * h * 2) : (w * h * 3 / 2));
    if (g_fsig)
        len |= 0x80000000u;
    struct timespec ts;
    uint32_t ms;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ms = (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
    memset(hd, 0, sizeof(hd));
    memcpy(hd, g_rgb ? "AR24" : (g_yuyv ? "YUY2" : "NV12"), 4);
    memcpy(hd + 4, &ww, 2);
    memcpy(hd + 6, &hh, 2);
    memcpy(hd + 8, &len, 4);
    memcpy(hd + 12, &ms, 4);      /* 帧时刻 (ms, MONOTONIC) —— 给下游算延迟 */
    fwrite(hd, 1, sizeof(hd), f);
}

/* 前置声明 (feeder_* 用到, 定义在后面) */
static int qbuf_flags(int fd, unsigned int type, unsigned int index,
                      unsigned num_planes, unsigned bytes_used, unsigned flags);

/* ==========================================================================
 * AU 边界拼包 (实测必须做):
 *   mvx 的固件把**每个输入缓冲**当成若干个完整访问单元来处理 — 如果缓冲里
 *   正好切在某帧中间, 固件会报 `Send stream corrupt: header parse error`
 *   然后整条流不再出帧 (实测: 64KB 一块 → 0 帧; 256KB/512KB → 正常)。
 *   而管道一次 read() 往往只得到 32~64KB, 所以必须自己攒够**完整 AU** 再投喂。
 * ========================================================================== */

/* 这个 NAL 是不是一个 AU 的起点 */
static int nal_is_au_start(const uint8_t *nal, int len)
{
    int type = nal[0] & 0x1f;
    /* ⚠️ SPS/PPS/SEI 不能当边界: 它们紧跟在下一个 AU 的 slice 之前,
     *    在 PPS 处切会把 SPS 与 PPS 分开 → 固件报 header parse error。 */
    if (type == 9)                                            /* AUD: 合法边界 */
        return 1;
    if (type == 1 || type == 5) {                            /* 非 IDR / IDR slice */
        if (len < 2)
            return 0;
        /* first_mb_in_slice 是 ue(v): 值为 0 时就是单个 '1' 位 ⇒ 首字节最高位为 1 */
        return (nal[1] & 0x80) != 0;
    }
    return 0;
}

/* ★ 真正用于投喂的切点: 让**每个输入缓冲只装一个 AU** (含它自己的 SPS/PPS/SEI)。
 *   mvx 固件把每个缓冲当成「缓冲末尾 = 帧结束」(END_OF_SUB_FRAME) 的一段码流,
 *   一个缓冲塞多帧时只会解出其中一帧 (实测 87 帧只出 27 帧)。
 *   做法: 找到第一个 AU 起点(slice/AUD), 再往前回退它紧跟的 6/7/8/9 参数集, 在那里下刀。 */
static size_t first_au_cut(const uint8_t *b, size_t len)
{
    size_t offs[256];
    size_t hdr[256];
    int types[256];
    int n = 0;

    for (size_t i = 0; i + 4 <= len && n < 256; ) {
        size_t nal_off;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            nal_off = i + 3;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1)
            nal_off = i + 4;
        else {
            i++;
            continue;
        }
        offs[n] = i;
        hdr[n] = nal_off;
        types[n] = b[nal_off] & 0x1f;
        n++;
        i = nal_off;
    }

    for (int k = 1; k < n; k++) {
        if (!nal_is_au_start(b + hdr[k], (int)(len - hdr[k])))
            continue;
        /* 回退紧跟在前面的参数集 NAL, 划到它们前面 */
        int j = k;
        while (j > 0 && (types[j - 1] == 6 || types[j - 1] == 7 ||
                         types[j - 1] == 8 || types[j - 1] == 9))
            j--;
        if (j == 0)
            continue;
        return offs[j];
    }
    return 0;
}

/* 同一个扫描逻辑的“最后一个 AU 起点”版 (greedy 模式用: 一个缓冲尽量多塞帧) */
static size_t last_au_cut(const uint8_t *b, size_t len)
{
    size_t cut = 0;
    int seen_vcl = 0;
    for (size_t i = 0; i + 4 <= len; ) {
        size_t hdr;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            hdr = i + 3;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1)
            hdr = i + 4;
        else {
            i++;
            continue;
        }
        if (nal_is_au_start(b + hdr, (int)(len - hdr)) && seen_vcl)
            cut = i;
        if ((b[hdr] & 0x1f) == 1 || (b[hdr] & 0x1f) == 5)
            seen_vcl = 1;
        i = hdr;
    }
    return cut;
}

typedef struct {
    int infd, fd;
    buf_t *outp;
    int nout, *out_used, *eof;
    size_t chunk;
    uint8_t *stage;
    size_t slen, scap;
} feeder_t;

/* 从输入读一块追加到 stage; 返回 1=读到数据 0=EAGAIN/EOF(看 *eof) */
/* ===== 输入流: 回放 + 实时, 并把读过的码流记下来 (用于原地重启 + 盯分辨率) ===== */
static int g_infd = -1;
static int g_replay_fd = -1;
static uint8_t *g_seen;
static size_t g_seen_len, g_seen_cap;
static const char *g_replay_path;   /* -R <file>: 先读这个文件 (重启回放), 读完再读实时输入 */

/* 码流里"最近一次 SPS 说的分辨率" —— 流跑到一半变分辨率 (手机旋转/切 app/
 * 中途换档) 时, 只有它能给出真实宽高 (驱动不给 NEED_REALLOC 标志, 见下)。
 * g_seen 只是它的扫描载体, 所以有上限: 以前无上限, 长时间投屏会一直涨
 * (8 Mbps ≈ 1 MB/s ⇒ 一小时 3.6 GB), K230 迟早 OOM。 */
#define SEEN_MAX (8u << 20)
static size_t g_sps_scan;
static unsigned g_live_w, g_live_h;   /* 裁剪后尺寸**吸附到 16 对齐**后, S_FMT 用这个 */
static unsigned g_live_raw_w, g_live_raw_h; /* 裁剪后原始尺寸 (只做日志/变化判定) */
static unsigned g_live_cw, g_live_ch; /* coded 尺寸 (只用来判"流是否真换分辨率") */

/* MVX H.264 解码器只吃 16 对齐的宽高 —— 2026-10-31 板上 A/B 实测 (同一 720 高):
 *   400x720 → 90 帧全出; 416x720 → 90 帧全出; **406x720 → 0 帧** (3 s 超时, rc=3)。
 * SPS 的 frame_crop 给出的是任意像素宽度 (iPhone 竖屏镜像 = coded 1280x720 + crop
 * 406x720), 直接拿去 S_FMT 就是一帧不出 ⇒ 解码器退出 ⇒ 屏黑。所以把送进 S_FMT
 * 的尺寸统一向下吸附到 16 的整数倍: 宁可少掉右边几列 (406→400, 1.5%), 也不要把
 * 未定义的补齐列画到屏上; 上屏几何由 drmplay 等比 contain 补边, 少几列看不出来。
 * 关掉 (回旧行为, 做 A/B): DEC_ALIGN=0。 */
static unsigned g_align;                /* 0 = 未初始化; <2 = 关闭吸附 */
static unsigned sz_fit(unsigned v)
{
    if (!g_align) {
        const char *o = getenv("DEC_ALIGN");
        g_align = (o && *o) ? (unsigned)strtol(o, NULL, 0) : 16u;
        if (g_align && g_align < 2) g_align = 2;
    }
    if (g_align < 2) return v;
    unsigned a = v / g_align * g_align;
    return a >= g_align ? a : v;
}
static size_t g_sps_last;   /* 最后一个 SPS 在 g_seen 里的起始偏移 (兜底重启时从这回放) */
static void sps_watch(void);
static void au_norm_sps(uint8_t *buf, size_t *len, size_t cap);
static void dump_es(const void *p, size_t n);
static int cropfix_on(void);

/* ===== 机型/流类型兼容(quirk)开关 (2026-10-31) =====
 * 哲学同 drmplay 的 K80 自适应: **按流特征自动兜底, 不看手机型号**,
 * 换任何手机都自动适配; 个别机型要强制开/关某个 quirk 时走 env。
 *   DEC_QUIRK=no-<名字>  关掉某个 quirk (逗号分隔可多个)
 *   DEC_QUIRK=<名字>     强制开 (预留; 目前默认就开)
 * quirk 清单 (后续问题机型/问题类型**加一行名字 + 一段兜底**即可, 不影响其它):
 *   realloc-reexec  采集缓冲重建被固件拒 (V5/V7-H264DEC: RASC ABORT → STREAMON
 *                   EINVAL) 时, 带真实 SPS 尺寸原地重启解码器自愈。
 *                   触发面: 流中途换分辨率 (手机旋转/竖屏流, 如 iPhone 406x720)。 */
/* env 开关: 未设置 / 空 / "0" / "no" / "off" 都算关。
 * ⚠ 2026-10-31 现场踩过: 旧代码只判断 getenv() 是否非 NULL, 于是
 *   DEC_REALLOC_REEXEC=0 反而被当成"开" ⇒ 一直在 re-exec 循环里出不了帧。 */
static int env_on(const char *k)
{
    const char *v = getenv(k);
    if (!v || !*v) return 0;
    if (!strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "off")) return 0;
    return 1;
}

static int quirk_on(const char *name)
{
    const char *q = getenv("DEC_QUIRK");
    char pat[64];
    if (!q || !*q)
        return 1;                       /* 默认全开 (都只在失败路径触发) */
    snprintf(pat, sizeof pat, "no-%s", name);
    if (strstr(q, pat))
        return 0;
    return 1;
}

static ssize_t inp_read(void *buf, size_t n)
{
    if (g_replay_fd >= 0) {
        ssize_t r = read(g_replay_fd, buf, n);
        if (r > 0) return r;
        close(g_replay_fd);
        g_replay_fd = -1;
    }
    ssize_t r = read(g_infd, buf, n);
    if (r > 0) {
        size_t need = g_seen_len + (size_t)r;
        if (need > g_seen_cap && g_seen_cap < SEEN_MAX) {
            size_t nc = need * 2 + (1 << 20);
            if (nc > SEEN_MAX) nc = SEEN_MAX;
            uint8_t *p = realloc(g_seen, nc);
            if (p) { g_seen = p; g_seen_cap = nc; }
        }
        if (g_seen && need <= g_seen_cap) {
            memcpy(g_seen + g_seen_len, buf, (size_t)r);
            g_seen_len = need;
            sps_watch();          /* 顺手盯 SPS (只在有新字节时扫增量) */
        }
        dump_es(buf, (size_t)r);  /* DEC_DUMP_ES: 原始码流 (含开头的 SPS/PPS) 落盘 */
    }
    return r;
}

static int feeder_append(feeder_t *f)
{
    if (f->scap - f->slen < 8192) {
        size_t nc = f->scap ? f->scap * 2 : (1 << 20);
        uint8_t *p = realloc(f->stage, nc);
        if (!p)
            return 0;
        f->stage = p;
        f->scap = nc;
    }
    ssize_t n = inp_read(f->stage + f->slen, f->scap - f->slen);
    if (n > 0) { f->slen += (size_t)n; return 1; }
    if (n == 0) { *f->eof = 1; }
    else if (errno != EAGAIN && errno != EWOULDBLOCK) { *f->eof = 1; }
    return 0;
}

/* 把 stage 里完整的 AU 段投给空闲 OUTPUT 缓冲; 返回本次入队个数 */

/* DEC_DUMP_ES=<path>: 把喂进解码器的**原始**码流 (未改 SPS 前) 落盘一份 (上限 4 MB)。
 * 2026-10-31 加: 真机 iPhone 流诊断 —— 板上一帧不出的时侯, 拿这份 ES 到主机用软解
 * 对照, 才能分清是"流本身"还是"板上驱动"。生产不设这个变量就没有任何开销。 */
static void dump_es(const void *p, size_t n)
{
    static FILE *df;
    static size_t total;
    static int off;
    if (off) return;
    if (!df) {
        const char *path = getenv("DEC_DUMP_ES");
        if (!path || !*path) { off = 1; return; }
        df = fopen(path, "wb");
        if (!df) { off = 1; return; }
        dlog("[dec] 🔎 开始落盘原始 ES → %s (上限 4 MB)", path);
    }
    if (total > (4u << 20)) { fclose(df); df = NULL; off = 1; return; }
    fwrite(p, 1, n, df);
    fflush(df);
    total += n;
}

static int feeder_fill(feeder_t *f)
{
    int queued = 0;
    for (;;) {
        int idx = -1;
        for (int i = 0; i < f->nout; i++)
            if (!f->out_used[i]) { idx = i; break; }
        if (idx < 0)
            break;

        /* 三种攒包策略 (实测结论):
         *   g_perau=0 (默认): 攒满 chunk 再切 → 帧率最高、最稳 (二进制 60 帧流实收 60 帧)
         *   g_perau=2: 一有完整 AU 就切 → 直播低延迟 (不攒 chunk) */
        size_t cut = 0;
        if (g_perau == 1) {
            cut = first_au_cut(f->stage, f->slen);
            while (cut == 0 && !*f->eof)
                if (!feeder_append(f))
                    break;
            cut = first_au_cut(f->stage, f->slen);
        } else if (g_perau == 2) {
            cut = last_au_cut(f->stage, f->slen);
            while (cut == 0 && !*f->eof)
                if (!feeder_append(f))
                    break;
            cut = last_au_cut(f->stage, f->slen);
        } else {
            while (f->slen < f->chunk && !*f->eof)
                if (!feeder_append(f))
                    break;
            cut = last_au_cut(f->stage, f->slen);
        }
        int au_aligned = 1;                /* 切点是否落在真实 AU 边界 */
        if (cut == 0) {
            if (f->slen >= f->chunk) {
                cut = f->chunk;            /* 整帧超过缓冲: 只能硬切 */
                au_aligned = 0;
            } else if (*f->eof && f->slen > 0) {
                cut = f->slen;             /* 收尾: 把尾巴也投出去 */
            } else
                break;                     /* 数据还不够一个 AU, 等下一轮 */
        }
        if (cut > f->chunk) {
            cut = f->chunk;
            au_aligned = 0;
        }

        memcpy(f->outp[idx].addr[0], f->stage, cut);
        /* ★ 2026-10-31: 喂给驱动前把 SPS 的可见宽改成 16 对齐 (406 → 400)。
         * 驱动按 SPS 算采集缓冲尺寸, 只改 S_FMT 它还是会要求 406 ⇒ STREAMON EINVAL。 */
        size_t qlen = cut;
        if (cropfix_on() && quirk_on("sps-align"))
            au_norm_sps(f->outp[idx].addr[0], &qlen, f->outp[idx].len[0]);
        unsigned flags = 0;
        if (au_aligned) {
            if (g_flag & 1) flags |= V4L2_BUF_FLAG_KEYFRAME;          /* → END_OF_FRAME */
            if (g_flag & 2) flags |= V4L2_BUF_FLAG_END_OF_SUB_FRAME;
        }
        if (g_cfg && !g_cfg_sent) {
            flags |= 0xC1000000;                                     /* V4L2_BUF_FLAG_MVX_CODEC_CONFIG */
            g_cfg_sent = 1;
        }
        if (qbuf_flags(f->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (unsigned)idx,
                       f->outp[idx].num_planes, (unsigned)qlen, flags) < 0)
            break;
        if (au_aligned) {                 /* 一个完整 AU 入队 ⇒ 记一个解码起跑时刻 */
            struct timespec qts;
            clock_gettime(CLOCK_MONOTONIC, &qts);
            g_feed_ts[g_feed_h & 255] = (double)qts.tv_sec + qts.tv_nsec / 1e9;
            g_feed_h++;
        }
        f->out_used[idx] = 1;
        queued++;

        memmove(f->stage, f->stage + cut, f->slen - cut);
        f->slen -= cut;

        if (*f->eof && f->slen == 0)
            break;
    }
    return queued;
}

static const char *fourcc_str(unsigned int f, char out[5])
{
    out[0] = (char)(f & 0x7f);
    out[1] = (char)((f >> 8) & 0x7f);
    out[2] = (char)((f >> 16) & 0x7f);
    out[3] = (char)((f >> 24) & 0x7f);
    out[4] = 0;
    return out;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    if (r < 0)
        fprintf(stderr, "[dec] ✗ %s: %s\n", what, strerror(errno));
    return r;
}

/* S_FMT 一个口, 返回驱动实际接受的格式 */
static int set_fmt(int fd, unsigned int type, unsigned int pixfmt,
                   unsigned int w, unsigned int h, unsigned int sizeimage,
                   struct v4l2_format *out)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = type;
    f.fmt.pix_mp.width = w;
    f.fmt.pix_mp.height = h;
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
    if (xioctl(fd, VIDIOC_S_FMT, &f, "S_FMT") < 0)
        return -1;
    if (out)
        *out = f;
    if (g_verbose) {
        char cc[5];
        fprintf(stderr, "[dec] S_FMT type=%u → %ux%u %s planes=%u sizeimage=[%u,%u,%u]\n",
               type, f.fmt.pix_mp.width, f.fmt.pix_mp.height,
               fourcc_str(f.fmt.pix_mp.pixelformat, cc), f.fmt.pix_mp.num_planes,
               f.fmt.pix_mp.plane_fmt[0].sizeimage,
               f.fmt.pix_mp.plane_fmt[1].sizeimage,
               f.fmt.pix_mp.plane_fmt[2].sizeimage);
    }
    return 0;
}

static int reqbufs(int fd, unsigned int type, unsigned int count)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = count;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0)
        return -1;
    if (g_verbose)
        fprintf(stderr, "[dec] REQBUFS type=%u → count=%u\n", type, rb.count);
    return (int)rb.count;
}

static int query_mmap(int fd, unsigned int type, unsigned int index, buf_t *b)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.m.planes = planes;
    buf.length = VIDEO_MAX_PLANES;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf, "QUERYBUF") < 0)
        return -1;

    b->num_planes = buf.length;
    for (unsigned i = 0; i < buf.length; i++) {
        b->len[i] = planes[i].length;
        b->addr[i] = mmap(NULL, planes[i].length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, (off_t)planes[i].m.mem_offset);
        if (b->addr[i] == MAP_FAILED) {
            fprintf(stderr, "[dec] ✗ mmap plane %u: %s\n", i, strerror(errno));
            b->addr[i] = NULL;
            return -1;
        }
    }
    return 0;
}

static int qbuf_flags(int fd, unsigned int type, unsigned int index,
                      unsigned num_planes, unsigned bytes_used, unsigned flags)
{
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    buf.flags = flags;
    buf.m.planes = planes;
    buf.length = num_planes;
    if (num_planes > 0)
        planes[0].bytesused = bytes_used;
    if (xioctl(fd, VIDIOC_QBUF, &buf, "QBUF") < 0)
        return -1;
    return 0;
}

static int qbuf(int fd, unsigned int type, unsigned int index,
                unsigned num_planes, unsigned bytes_used)
{
    return qbuf_flags(fd, type, index, num_planes, bytes_used, 0);
}

static int dqbuf(int fd, unsigned int type, struct v4l2_buffer *buf,
                 struct v4l2_plane *planes)
{
    memset(buf, 0, sizeof(*buf));
    memset(planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
    buf->type = type;
    buf->memory = V4L2_MEMORY_MMAP;
    buf->m.planes = planes;
    buf->length = VIDEO_MAX_PLANES;
    /* EAGAIN = 暂时无帧 (非阻塞 fd 的正常返回), 不算错误, 不要打红字 */
    if (ioctl(fd, VIDIOC_DQBUF, buf) < 0)
        return -1;
    return 0;
}

static int streamon(int fd, unsigned int type)
{
    return xioctl(fd, VIDIOC_STREAMON, &type, "STREAMON");
}

static int streamoff(int fd, unsigned int type)
{
    return xioctl(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF");
}

/* ==========================================================================
 * SPS 解析 (只为拿分辨率, 不解其它字段)
 * 为什么必须: mvx 的 OUTPUT S_FMT 声明的宽高必须和码流真实尺寸一致,
 * 否则固件一个帧都不吐 (实测: 720p 流声明 1024x768/1920x1080 → 0 帧)。
 * 直播间不知道手机会推多少, 所以自己从 SPS 读。
 * ========================================================================== */
typedef struct { const uint8_t *p; size_t len, bit; } br_t;

static int br_u(br_t *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (b->bit >= b->len * 8)
            return -1;
        v = (v << 1) | ((b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1);
        b->bit++;
    }
    return v;
}

static int br_ue(br_t *b)
{
    int zeros = 0, v;
    while (1) {
        int bit = br_u(b, 1);
        if (bit < 0) return -1;
        if (bit) break;
        if (++zeros > 31) return -1;
    }
    v = br_u(b, zeros);
    if (v < 0) return -1;
    return (1 << zeros) - 1 + v;
}

/* Annex-B 去防竞争字节 (00 00 03 → 00 00) */
static size_t ebsp_strip(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o < cap; i++) {
        if (i + 2 < len && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3)
            i++;
        dst[o++] = src[i];
    }
    return o;
}

/* 从码流头里找 SPS 解析出宽高; 成功返回 0 */
/* coded/cropped 两套尺寸 (2026-10-31 iPhone 现场):
 *   coded   = (wmbs+1)*16 x (2-frame_mbs_only)*(hmbs+1)*16  —— 码流真正的编码尺寸,
 *             **MVX 的 CAPTURE S_FMT 只认这个**, 给裁剪后的尺寸会在 STREAMON 报 EINVAL。
 *   cropped = coded 减掉 SPS 的 frame_crop —— 只是「画面内容区」, 上屏几何用。
 *   iPhone 竖屏镜像就是 coded 1280x720 + 裁剪成 406x720;
 *   以前只返回 cropped, 于是「同一个流」被当成「分辨率变了」→ 重建 → 固件 RASC ABORT
 *   → 0 帧 (还会把板子卡在 STREAMON 里被看门狗复位)。
 *   现在: **尺寸判定只用 coded, cropped 只进日志**。
 *   cw/ch 传 NULL 表示不需要 coded 值。 */
static int sps_dims(const uint8_t *b, size_t len, unsigned *w, unsigned *h,
                    unsigned *cwout, unsigned *chout)
{
    static uint8_t rbsp[512];
    for (size_t i = 0; i + 5 <= len; ) {
        size_t hdr;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            hdr = i + 3;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1)
            hdr = i + 4;
        else {
            i++;
            continue;
        }
        if ((b[hdr] & 0x1f) != 7) {
            i = hdr;
            continue;
        }
        /* SPS 到下一个 start code 为止 */
        size_t end = hdr + 1;
        while (end + 3 <= len &&
               !(b[end] == 0 && b[end + 1] == 0 &&
                 (b[end + 2] == 1 || (b[end + 2] == 0 && end + 3 < len && b[end + 3] == 1))))
            end++;
        size_t n = ebsp_strip(b + hdr + 1, end - (hdr + 1), rbsp, sizeof(rbsp));
        br_t br = { rbsp, n, 0 };
        int profile = br_u(&br, 8);
        br_u(&br, 8);                      /* constraint flags */
        br_u(&br, 8);                      /* level_idc */
        if (br_ue(&br) < 0) return -1;      /* sps_id */
        if (profile == 100 || profile == 110 || profile == 122 || profile == 244 ||
            profile == 44 || profile == 83 || profile == 86 || profile == 118 ||
            profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
            profile == 135) {
            int cf = br_ue(&br);
            if (cf == 3) br_u(&br, 1);
            if (br_ue(&br) < 0) return -1;  /* bit_depth_luma */
            if (br_ue(&br) < 0) return -1;  /* bit_depth_chroma */
            br_u(&br, 1);                   /* qpprime */
            if (br_u(&br, 1) == 1) {        /* scaling matrix */
                int cnt = (cf != 3) ? 8 : 12;
                for (int k = 0; k < cnt; k++) {
                    if (br_u(&br, 1) == 1) {
                        int last = 8, next = 8;
                        for (int j = 0; j < (k < 6 ? 16 : 64); j++) {
                            if (next) next = (last + br_ue(&br)) & 0xff;
                            if (next < 0) return -1;
                            last = next ? next : last;
                        }
                    }
                }
            }
        }
        if (br_ue(&br) < 0) return -1;      /* log2_max_frame_num */
        int poc = br_ue(&br);
        if (poc == 0) {
            if (br_ue(&br) < 0) return -1;
        } else if (poc == 1) {
            br_u(&br, 1); br_ue(&br); br_ue(&br); br_ue(&br);
        }
        if (br_ue(&br) < 0) return -1;      /* max_num_ref_frames */
        br_u(&br, 1);                       /* gaps */
        int wmbs = br_ue(&br);
        int hmbs = br_ue(&br);
        int frame_mbs_only = br_u(&br, 1);
        if (wmbs < 0 || hmbs < 0 || frame_mbs_only < 0)
            return -1;
        if (!frame_mbs_only)
            br_u(&br, 1);
        br_u(&br, 1);                       /* direct_8x8 */
        int crop = br_u(&br, 1);
        unsigned cw = 0, ch = 0;
        if (crop) {
            int l = br_ue(&br), r = br_ue(&br), t = br_ue(&br), bo = br_ue(&br);
            int sub = frame_mbs_only ? 1 : 2;
            cw = (unsigned)((l + r) * 2 / sub);
            ch = (unsigned)((t + bo) * 2 * sub / sub);
            (void)sub;
        }
        *w = (unsigned)(wmbs + 1) * 16 - cw;
        *h = (unsigned)(2 - frame_mbs_only) * (hmbs + 1) * 16 - ch;
        if (cwout) *cwout = (unsigned)(wmbs + 1) * 16;
        if (chout) *chout = (unsigned)(2 - frame_mbs_only) * (hmbs + 1) * 16;
        return (*w && *h) ? 0 : -1;
    }
    return -1;
}

/* ===== SPS 归一: 让**驱动**看到的可见宽度也是 16 对齐 (2026-10-31) =====
 * 板上实测 (t406/t400/t416 三段合成流): MVX 只认 16 对齐宽 —— 400/416 出帧,
 * 406 一帧不出 (STREAMON EINVAL)。而驱动是**从 SPS 的 frame_crop 自己算**可见宽:
 * 我们把 S_FMT 改成 400, 它立刻回 NEED_REALLOC + G_FMT=406, 照它重建 ⇒ EINVAL,
 * 严重时卡死被看门狗复位。所以只改 S_FMT 没用, 必须让 SPS 自己就说 400:
 * 把 frame_crop 右侧多裁 delta 列 (delta = 可见宽 - 16 对齐宽, iPhone 是 6)。
 * 解码内容/参考帧完全不动, 只是画面右边少 6 列 (上屏由 drmplay 等比 contain)。
 * 只在 crop 存在且可见宽不是 16 对齐时触发; 其它流一个字节都不改。 */
typedef struct { uint8_t *p; size_t cap, bit; int err; } bw_t;

static void bw_u(bw_t *w, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if ((w->bit >> 3) >= w->cap) { w->err = 1; return; }
        if ((v >> i) & 1u)
            w->p[w->bit >> 3] |= (uint8_t)(0x80u >> (w->bit & 7));
        w->bit++;
    }
}

static void bw_ue(bw_t *w, unsigned v)
{
    unsigned x = v + 1;
    int len = 0;
    for (unsigned t = x; t; t >>= 1) len++;
    bw_u(w, 0, len - 1);
    bw_u(w, x, len);
}

static int br_se(br_t *b)
{
    int k = br_ue(b);
    if (k < 0) return -1000000;
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

/* Annex-B 插防竞争字节 (00 00 → 00 00 03 xx) */
static size_t ebsp_insert(const uint8_t *src, size_t len, uint8_t *dst, size_t cap)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < len && o < cap; i++) {
        if (zeros >= 2 && src[i] <= 3) { dst[o++] = 3; zeros = 0; }
        dst[o++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    return o;
}


/* 可见区矩形 (SPS frame_crop, 换成 luma 像素坐标)。解码按 coded 尺寸走, 出帧时
 * 按这个矩形裁掉填充区 (iPhone 竖屏镜像 = coded 1280x720 里中间 406x720)。
 * 没有裁剪 ⇒ g_vis_w/g_vis_h 置 0。 */
static unsigned g_vis_x, g_vis_y, g_vis_w, g_vis_h;
static int g_realloc_same;   /* 已经忽略过一次"尺寸没变的重分配" */
/* ★ 2026-10-31 (主人要求: 不许为了苹果影响已验收的安卓/PC):
 * 新逻辑只在 **DEC_CROP_FIX=1** 时启用 —— 这个变量只写进 k230-airplay 的
 * drop-in。Miracast/PC 那条路 (黄金版本验过的) 环境里没有这个变量 ⇒ 尺寸、
 * 缓冲个数、重分配策略、出帧路径**一个字节都不变**。 */
static int g_cropfix = -1;      /* -1 未初始化; 1 = 苹果分支 */
static int cropfix_on(void)
{
    if (g_cropfix < 0) g_cropfix = env_on("DEC_CROP_FIX");
    return g_cropfix;
}
static int g_has_crop;

static int sps_crop_rect(const uint8_t *b, size_t len)
{
    for (size_t i = 0; i + 5 <= len; ) {
        size_t hdr;
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) hdr = i + 3;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) hdr = i + 4;
        else { i++; continue; }
        if ((b[hdr] & 0x1f) != 7) { i = hdr; continue; }
        static uint8_t rbsp[1024];
        size_t end = hdr + 1;
        while (end + 3 <= len &&
               !(b[end] == 0 && b[end + 1] == 0 &&
                 (b[end + 2] == 1 || (b[end + 2] == 0 && end + 3 < len && b[end + 3] == 1))))
            end++;
        size_t n = ebsp_strip(b + hdr + 1, end - (hdr + 1), rbsp, sizeof rbsp);
        br_t br = { rbsp, n, 0 };
        int prof = br_u(&br, 8);
        br_u(&br, 8);
        br_u(&br, 8);
        if (br_ue(&br) < 0) return -1;
        int chroma = 1;
        if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 ||
            prof == 83 || prof == 86 || prof == 118 || prof == 128 || prof == 138 ||
            prof == 139 || prof == 134 || prof == 135) {
            chroma = br_ue(&br);
            if (chroma < 0) return -1;
            if (chroma == 3) br_u(&br, 1);
            br_ue(&br); br_ue(&br); br_u(&br, 1);
            if (br_u(&br, 1) == 1) return -1;
        }
        if (br_ue(&br) < 0) return -1;
        int poc = br_ue(&br);
        if (poc == 0) { if (br_ue(&br) < 0) return -1; }
        else if (poc == 1) { br_u(&br, 1); br_ue(&br); br_ue(&br); if (br_ue(&br) != 0) return -1; }
        else if (poc != 2) return -1;
        if (br_ue(&br) < 0) return -1;
        br_u(&br, 1);
        int wmbs = br_ue(&br), hmbs = br_ue(&br);
        int fmo = br_u(&br, 1);
        if (wmbs < 0 || hmbs < 0 || fmo < 0) return -1;
        if (!fmo) br_u(&br, 1);
        br_u(&br, 1);
        int crop = br_u(&br, 1);
        if (crop < 0) return -1;
        g_vis_w = g_vis_h = 0;
        if (!crop) { g_vis_x = g_vis_y = 0; return 0; }
        int cl = br_ue(&br), cr = br_ue(&br), ct = br_ue(&br), cb = br_ue(&br);
        if (cl < 0 || cr < 0 || ct < 0 || cb < 0) return -1;
        unsigned cux = (chroma == 1 || chroma == 2) ? 2u : 1u;
        unsigned cuy = (chroma == 1) ? 2u : 1u;
        unsigned cw = (unsigned)(wmbs + 1) * 16;
        unsigned chh = (unsigned)(2 - fmo) * (hmbs + 1) * 16;
        g_vis_x = cux * (unsigned)cl;
        g_vis_y = cuy * (unsigned)ct;
        g_vis_w = cw - cux * (unsigned)(cl + cr);
        g_vis_h = chh - cuy * (unsigned)(ct + cb);
        if (g_vis_w < 2 || g_vis_h < 2 || g_vis_x + g_vis_w > cw || g_vis_y + g_vis_h > chh) {
            g_vis_w = g_vis_h = 0;
            g_vis_x = g_vis_y = 0;
        }
        return g_vis_w ? 1 : 0;
    }
    return -1;
}

/* 返回 1 = 已改 (out/outlen 有值); 0 = 不用改; -1 = 不认识的 SPS, 放弃 */
static int sps_norm_rbsp(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap,
                         size_t *outlen, unsigned *old_vis, unsigned *new_vis)
{
    br_t b = { in, inlen, 0 };
    int prof = br_u(&b, 8), cons = br_u(&b, 8), lvl = br_u(&b, 8);
    int sps_id, chroma = 1, sep = 0, bdl = 0, bdc = 0;
    int high;
    if (prof < 0 || cons < 0 || lvl < 0) return -1;
    sps_id = br_ue(&b);
    if (sps_id < 0) return -1;
    high = (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 ||
            prof == 83 || prof == 86 || prof == 118 || prof == 128 || prof == 138 ||
            prof == 139 || prof == 134 || prof == 135);
    if (high) {
        chroma = br_ue(&b);
        if (chroma < 0) return -1;
        if (chroma == 3) { sep = br_u(&b, 1); if (sep < 0) return -1; }
        bdl = br_ue(&b);
        bdc = br_ue(&b);
        if (bdl < 0 || bdc < 0) return -1;
        br_u(&b, 1);                        /* qpprime_y_zero_transform_bypass */
        if (br_u(&b, 1) != 0) return -1;    /* 带 scaling list: 不碰 */
    }
    int log2_fn = br_ue(&b), poc = br_ue(&b);
    int log2_poc = 0, dpo = 0, onr = 0, ottb = 0;
    if (log2_fn < 0 || poc < 0) return -1;
    if (poc == 0) {
        log2_poc = br_ue(&b);
        if (log2_poc < 0) return -1;
    } else if (poc == 1) {
        dpo = br_u(&b, 1);
        onr = br_se(&b);
        ottb = br_se(&b);
        int cyc = br_ue(&b);
        if (dpo < 0 || onr == -1000000 || ottb == -1000000 || cyc < 0) return -1;
        if (cyc > 0) return -1;             /* 循环 POC (极罕见): 不碰 */
    }
    int maxref = br_ue(&b), gaps = br_u(&b, 1);
    int wmbs = br_ue(&b), hmbs = br_ue(&b);
    int fmo, mbaff = 0, d8, crop, vui, cl = 0, cr = 0, ct = 0, cb = 0;
    if (maxref < 0 || gaps < 0 || wmbs < 0 || hmbs < 0) return -1;
    fmo = br_u(&b, 1);
    if (fmo < 0) return -1;
    if (!fmo) { mbaff = br_u(&b, 1); if (mbaff < 0) return -1; }
    d8 = br_u(&b, 1);
    crop = br_u(&b, 1);
    if (d8 < 0 || crop < 0) return -1;
    if (crop) {
        cl = br_ue(&b); cr = br_ue(&b); ct = br_ue(&b); cb = br_ue(&b);
        if (cl < 0 || cr < 0 || ct < 0 || cb < 0) return -1;
    }
    vui = br_u(&b, 1);
    if (vui < 0) return -1;
    size_t tail_bit = b.bit;

    unsigned coded_w = (unsigned)(wmbs + 1) * 16;
    unsigned coded_h = (unsigned)(2 - fmo) * (hmbs + 1) * 16;
    unsigned cux = (chroma == 1 || chroma == 2) ? 2u : 1u;
    unsigned vis = crop ? coded_w - cux * (unsigned)(cl + cr) : coded_w;
    if (old_vis) *old_vis = vis;
    if (new_vis) *new_vis = coded_w;
    /* 2026-10-31 二版: 不再「缩裁剪」, 而是**整段去掉裁剪** —— 板上实测缩到
     * 16 对齐的 400 仍被固件按 416 步长写 ⇒ DMA 越界 oops。驱动的采集缓冲 =
     * 固件 alloc_param (= 可见宽), 步长却按 coded 走, 只要可见宽 < coded 就必炸。
     * 所以让固件干脆按 **coded** 分配 (1280x720/416x720 都是已知能解的尺寸),
     * 可见区由我们自己从解出的帧里裁 (emit 处按 g_vis_* 逐行拷)。
     * 只在「裁了等于没裁」时返回 0, 其它情况一律发一份 crop=0 的 SPS。 */
    if (!crop || (vis == coded_w && (ct + cb) == 0)) return 0;
    if (coded_h < 16 || coded_w < 16) return -1;

    memset(out, 0, outcap);
    bw_t w = { out, outcap, 0, 0 };
    bw_u(&w, (unsigned)prof, 8);
    bw_u(&w, (unsigned)cons, 8);
    bw_u(&w, (unsigned)lvl, 8);
    bw_ue(&w, (unsigned)sps_id);
    if (high) {
        bw_ue(&w, (unsigned)chroma);
        if (chroma == 3) bw_u(&w, (unsigned)sep, 1);
        bw_ue(&w, (unsigned)bdl);
        bw_ue(&w, (unsigned)bdc);
        bw_u(&w, 0, 1);
        bw_u(&w, 0, 1);
    }
    bw_ue(&w, (unsigned)log2_fn);
    bw_ue(&w, (unsigned)poc);
    if (poc == 0) bw_ue(&w, (unsigned)log2_poc);
    else if (poc == 1) {
        bw_u(&w, (unsigned)dpo, 1);
        bw_ue(&w, (unsigned)(onr > 0 ? onr * 2 - 1 : -onr * 2));
        bw_ue(&w, (unsigned)(ottb > 0 ? ottb * 2 - 1 : -ottb * 2));
        bw_ue(&w, 0);
    }
    bw_ue(&w, (unsigned)maxref);
    bw_u(&w, (unsigned)gaps, 1);
    bw_ue(&w, (unsigned)wmbs);
    bw_ue(&w, (unsigned)hmbs);
    bw_u(&w, (unsigned)fmo, 1);
    if (!fmo) bw_u(&w, (unsigned)mbaff, 1);
    bw_u(&w, (unsigned)d8, 1);
    bw_u(&w, 0, 1);                         /* frame_cropping_flag = 0 (见上: 可见区自己裁) */
    bw_u(&w, (unsigned)vui, 1);
    if (vui) {                              /* VUI 原样搬 (含 rbsp trailing) */
        for (size_t i = tail_bit; i < inlen * 8 && !w.err; i++)
            bw_u(&w, (in[i >> 3] >> (7 - (i & 7))) & 1u, 1);
    } else {
        bw_u(&w, 1, 1);                     /* rbsp_stop_one_bit */
    }
    if (w.err) return -1;
    *outlen = (w.bit + 7) / 8;
    return 1;
}

/* 扫一个 AU, 就地把里面的 SPS 改成 16 对齐可见宽; 长度可变时搬移后面的字节。
 * *len 会被更新 (只增删 SPS 那几个字节, 其它 NAL 原样)。 */
static void au_norm_sps(uint8_t *buf, size_t *len, size_t cap)
{
    static int logged;
    if (!len || *len < 6) return;
    size_t n = *len;
    for (size_t i = 0; i + 3 < n; ) {
        size_t sc, hdr, next;
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            sc = 3;
        else if (i + 4 < n && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
            sc = 4;
        else { i++; continue; }
        hdr = i + sc;
        if (hdr >= n) break;
        next = hdr + 1;
        while (next + 3 <= n &&
               !(buf[next] == 0 && buf[next + 1] == 0 &&
                 (buf[next + 2] == 1 ||
                  (buf[next + 2] == 0 && next + 3 < n && buf[next + 3] == 1))))
            next++;
        if ((buf[hdr] & 0x1f) == 7) {
            static uint8_t rbsp[1024], patched[1024], ebsp[1224];
            size_t rlen = ebsp_strip(buf + hdr + 1, next - (hdr + 1), rbsp, sizeof rbsp);
            unsigned ov = 0, nv = 0;
            size_t plen = 0;
            int rc = sps_norm_rbsp(rbsp, rlen, patched, sizeof patched, &plen, &ov, &nv);
            if (rc == 1) {
                size_t elen, old = next - hdr;
                ebsp[0] = buf[hdr];
                elen = 1 + ebsp_insert(patched, plen, ebsp + 1, sizeof ebsp - 1);
                if (elen <= old || n + (elen - old) <= cap) {
                    memmove(buf + hdr + elen, buf + next, n - next);
                    memcpy(buf + hdr, ebsp, elen);
                    n = n - old + elen;
                    *len = n;
                    if (!logged) {
                        logged = 1;
                        dlog("[dec] ✂ SPS 去裁剪: 可见 %u → 解码 %u (可见区由自己裁, NAL %zu → %zu B)",
                             ov, nv, old, elen);
                    }
                    next = hdr + elen;
                } else if (!logged) {
                    logged = 1;
                    dlog("[dec] ✗ SPS 归一放不下 (要 %zu B, 余量 %zu), 跳过 (驱动会要求原尺寸)",
                         elen, cap - (n - old));
                }
            }
            i = next;
            continue;
        }
        i = next;
    }
}

/* 扫新到的码流找 SPS, 记下真实分辨率 (g_live_w/h)。
 * 为什么必须有 (2026-10-26 板上实测): mvx 驱动在「采集缓冲比码流小」时
 *   **不给 NEED_REALLOC 标志**, 只把缓冲原样退回 (flags=0x20004001,
 *   bytesused=0, present=0), 同时 dmesg 刷 "Buffer plane too small";
 *   2 分钟实测 198 条, 一帧都没有 ⇒ 光靠标志永远修不好黑屏。
 *   所以自己从码流里读 SPS, 发现和当前缓冲尺寸不一致就重建。 */
static void sps_watch(void)
{
    if (g_seen_len < 6)
        return;
    size_t from = g_sps_scan > 64 ? g_sps_scan - 64 : 0;   /* 回退 64B, 防起始码/SPS 被切两半 */
    for (size_t i = from; i + 5 < g_seen_len; i++) {
        const uint8_t *p = g_seen + i;
        size_t hdr;
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            hdr = 3;
        else if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
            hdr = 4;
        else
            continue;
        if ((p[hdr] & 0x1f) == 7) {          /* NAL type 7 = SPS */
            unsigned sw, sh, cw, ch;
            g_sps_last = i;                  /* 记最后 SPS 起点: 兜底重启从这回放 */
            if (sps_dims(p, g_seen_len - i, &sw, &sh, &cw, &ch) == 0 && cw && ch) {
                /* ★ 2026-10-31 (iPhone 406x720 现场): 「换分辨率」只认 **coded** 尺寸。
                 *   iPhone 竖屏镜像 = coded 1280x720 不变、裁剪区从 1280x720 变 406x720;
                 *   以前拿裁剪后的当码流尺寸 ⇒ 误判「分辨率变了」⇒ 重建采集缓冲 ⇒
                 *   固件 V5/V7-H264DEC: RASC ABORT / STREAMON EINVAL, 严重时卡死在
                 *   ioctl 里被看门狗复位 ⇒ 屏黑。裁剪变化对缓冲毫无影响, 忽略即可,
                 *   上屏几何本来就由 drmplay 自己补边。 */
                if (g_live_cw && (cw != g_live_cw || ch != g_live_ch)) {
                    dlog("[dec] 码流分辨率变化 %ux%u → %ux%u (coded %ux%u → %ux%u)",
                         g_live_w, g_live_h, sw, sh, g_live_cw, g_live_ch, cw, ch);
                    g_live_raw_w = sw; g_live_raw_h = sh;
                    sps_crop_rect(p, g_seen_len - i);      /* 新码流的可见区 */
                    g_has_crop = cropfix_on() && (g_vis_w > 0);
                    g_live_w = g_has_crop ? sz_fit(cw) : cw;
                    g_live_h = g_has_crop ? sz_fit(ch) : ch;
                    g_live_cw = cw; g_live_ch = ch;
                } else if (g_live_cw && (sw != g_live_raw_w || sh != g_live_raw_h)) {
                    dlog("[dec] SPS 仅裁剪变化 %ux%u → %ux%u (coded 不变 %ux%u) → 不重建, 照常解码",
                         g_live_raw_w, g_live_raw_h, sw, sh, cw, ch);
                    g_live_raw_w = sw; g_live_raw_h = sh;   /* 只更新记录, 缓冲不动 */
                    sps_crop_rect(p, g_seen_len - i);      /* 裁剪变了, 出帧窗口跟着变 */
                }
            }
        }
        i += hdr;                            /* 跳过起始码, 少扫几字节 */
    }
    g_sps_scan = g_seen_len;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ===== quirk: realloc-reexec (2026-10-31 兜底, iPhone 406x720 现场) =====
 * 症状: 会话中换分辨率 (手机旋转/竖屏) 触发「采集缓冲重建」时, 固件在 STREAMON
 *   报 V5/V7-H264DEC: RASC ABORT → streamon EINVAL → 整个解码器退出 rc=3 →
 *   外壳循环重开还是重建失败 ⇒ 手机显示已连接, 屏上没画面。
 * 复现: 板上 cat t1280.h264 t406.h264 | 解码 → 100% 同款 dmesg; 纯 406 流正常。
 * 兜底: 重建失败时**不退出**, 带真实 SPS 尺寸原地重启自己 (从最后 SPS 回放),
 *   新会话按正确尺寸一次定型, 根本不进重建路径。成功路径一行不动。
 * 回放文件 = g_seen[g_sps_last..] (截到最后 SPS, 避免旧 SPS 先把尺寸定错)。 */
static void realloc_reexec(char **argv, int argc, unsigned w, unsigned h)
{
    static const char *tmp = "/tmp/dec-realloc-replay.h264";
    static int restarts;
    if (restarts >= 3) {
        dlog("[dec] ✗ realloc-reexec 已重试 3 次仍失败, 放弃 (外壳会重起管线)");
        return;
    }
    restarts++;
    size_t off = g_sps_last < g_seen_len ? g_sps_last : 0;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        if (g_seen_len > off)
            fwrite(g_seen + off, 1, g_seen_len - off, f);
        fclose(f);
    } else {
        return;
    }
    char ws[16], hs[16];
    char **nargv = calloc((size_t)argc + 8, sizeof(char *));
    int k = 0;
    if (!nargv) return;
    snprintf(ws, sizeof ws, "%u", w);
    snprintf(hs, sizeof hs, "%u", h);
    nargv[k++] = argv[0];
    nargv[k++] = "-w"; nargv[k++] = ws;
    nargv[k++] = "-h"; nargv[k++] = hs;
    nargv[k++] = "--replay"; nargv[k++] = (char *)tmp;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "-h")) { i++; continue; }
        if (!strcmp(argv[i], "--replay") && i + 1 < argc) { i++; continue; }
        nargv[k++] = argv[i];
    }
    nargv[k] = NULL;
    dlog("[dec] 🔄 兜底 realloc-reexec: 重建失败 → 带真实尺寸 %ux%u 原地重启 (第 %d 次, 回放 %zu B)",
         w, h, restarts, g_seen_len - off);
    fflush(NULL);
    execv("/proc/self/exe", nargv);
    dlog("[dec] ✗ execv: %s", strerror(errno));
}

/* 学到内容区后, 带着硬件裁剪/缩放参数**原地重启自己**: 码流从头回放 + 回到实时输入。
 * 为什么必须重启: DEC_DST_CROP / DEC_DOWNSCALE 只能在固件加载前设置 (否则 -EBUSY),
 * 而内容区只有解出帧才知道。exec 换进程最干净, 不会有残留状态。 */
static void learn_restart(char **argv, int argc,
                          int x, int y, int w, int h, int dw, int dh, const char *fit)
{
    const char *tmp = "/tmp/dec-replay.h264";
    FILE *f = fopen(tmp, "wb");
    if (f) {
        if (g_seen_len) fwrite(g_seen, 1, g_seen_len, f);
        fclose(f);
    }
    if (fit) {
        FILE *lf = fopen(fit, "w");
        if (lf) { fprintf(lf, "%d %d %d %d %d %d\n", x, y, w, h, dw, dh); fclose(lf); }
    }
    {
        char xs[16], ys[16], ws[16], hs[16], dws[16], dhs[16];
        char **nargv = calloc((size_t)argc + 16, sizeof(char *));
        int k = 0;
        if (!nargv) return;
        snprintf(xs, sizeof xs, "%d", x); snprintf(ys, sizeof ys, "%d", y);
        snprintf(ws, sizeof ws, "%d", w); snprintf(hs, sizeof hs, "%d", h);
        snprintf(dws, sizeof dws, "%d", dw); snprintf(dhs, sizeof dhs, "%d", dh);
        nargv[k++] = argv[0];
        nargv[k++] = "-c"; nargv[k++] = xs; nargv[k++] = ys; nargv[k++] = ws; nargv[k++] = hs;
        nargv[k++] = "-s"; nargv[k++] = dws; nargv[k++] = dhs;
        nargv[k++] = "--replay"; nargv[k++] = (char *)tmp;
        for (int i = 1; i < argc; i++) nargv[k++] = argv[i];
        nargv[k] = NULL;
        fprintf(stderr, "[dec] 🔄 学到内容区 %d,%d %dx%d → 硬件裁剪+缩放 %dx%d, 原地重启 (回放 %zu 字节)\n",
                x, y, w, h, dw, dh, g_seen_len);
        fflush(NULL);
        execv("/proc/self/exe", nargv);
        perror("[dec] execv");
    }
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *dev = "/dev/video0";
    unsigned w = 1280, h = 720;
    int max_frames = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) w = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-H")) g_hdr = 1;
        else if (!strcmp(argv[i], "-y")) { g_yuyv = 1; g_hdr = 1; }
        else if (!strcmp(argv[i], "-s") && i + 2 < argc) {
            g_dsl_w = (unsigned)atoi(argv[++i]); g_dsl_h = (unsigned)atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-c") && i + 4 < argc) {
            g_crop_x = (unsigned)atoi(argv[++i]); g_crop_y = (unsigned)atoi(argv[++i]);
            g_crop_w = (unsigned)atoi(argv[++i]); g_crop_h = (unsigned)atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-R")) g_rgb = 1;
        else if (!strcmp(argv[i], "-D")) g_lowdelay = 1;
        else if (!strcmp(argv[i], "-A")) g_autofit = 1;
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) g_replay_path = argv[++i];
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) g_learn = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "未知参数 %s\n", argv[i]);
            return 2;
        } else if (!in_path) in_path = argv[i];
        else if (!out_path) out_path = argv[i];
    }
    /* 显式给了 -c/-s (原地重启后的形态) ⇒ 不再学习, 否则会在已裁好的画面上又学一次 */
    if (g_crop_w > 0 || g_dsl_w > 0)
        g_fit_loaded = 1;

    /* -w/-h (默认 1280x720 或 realloc-reexec 传进来的) 也按 16 对齐, 免得给 MVX
     * 一个它不认的宽度 ⇒ 0 帧。 */
    w = sz_fit(w); h = sz_fit(h);

    if (!in_path) {
        fprintf(stderr,
            "用法: %s <in.h264> [out.yuv] [-d /dev/video0] [-w W -h H] [-n 帧数] [-H] [-v]\n"
            "  输入: Annex-B H.264 裸流; 输出: NV12 (分辨率自动从 SPS 读)。\n"
            "  -H  每帧前加 16 字节头 (magic NV12/w/h/len), 配合 drmplay -H 实现分辨率自适应\n"
            "  -y  采集口用 YUYV(4:2:2) 输出 (头 magic=YUY2, 帧长 w*h*2), 给 vgplay (VGLite) 用\n"
            "  -s W H  VPU 硬件缩放: 解码器直接输出 WxH (MVE DEC_DOWNSCALE), CPU 不参与缩放\n"
            "  -c x y w h  VPU 硬件裁剪: 只输出解码画面的这个矩形 (MVE DEC_DST_CROPPING)\n"
            "  -A  硬件降采样到目标框 (默认 1x 屏 480x800, 可用 WFD_DSL_BOX=WxH 改) 等比:\n"
            "      上屏管线用这个, 每帧过管道的数据量减到 1/16 (对 720p 源)\n"
            "  ⚠ 本板固件不支持 -c: 见下方运行期告警\n",
            argv[0]);
        return 2;
    }

    /* 2026-10-26 实测定论 (dmesg 铁证): 本固件 v5/v7 H264 解码器**不支持**
     * DEC_DST_CROPPING —— ioctl 返回 0, 但固件随后报
     *   "Firmware set option failed. index=107, msg=V5/V7-H264DEC: set-opt h264dec:
     *    Unsupported option." (mvx_firmware_v2.c:974)
     * 后果: -c 被静默忽略, 而 -s 照常生效 ⇒ 整幅 1920x1080(含左右黑边)被横向
     * 压进 480 宽, 手机内容只剩中间一条 (现场「屏上只有中间一点点」)。
     * 上屏管道因此只传 -H -D, 裁黑边+缩放交给 drmplay。 */
    if (g_crop_w > 0 || g_learn)
        fprintf(stderr, "[dec] ⚠ 本固件 H264 解码器不支持 DEC_DST_CROPPING (index=107 "
                        "'Unsupported option') → -c/-L 的裁剪会被静默忽略, 只有 -s 生效; "
                        "上屏请只用 -H -D, 裁剪/缩放交 drmplay\n");

    /* 输入: 文件或管道。管道 (stdin / 非普通文件) 用 O_NONBLOCK 读,
     *       否则 fread 会等满一个 chunk 才返回, 直播流会卡住。 */
    int infd = open(in_path, O_RDONLY);
    if (infd < 0) { fprintf(stderr, "open %s: %s\n", in_path, strerror(errno)); return 1; }
    g_infd = infd;
    if (g_replay_path) {
        g_replay_fd = open(g_replay_path, O_RDONLY);
        if (g_replay_fd >= 0)
            fprintf(stderr, "[dec] 回放 %s (重启后接着解), 然后回到实时输入\n", g_replay_path);
    }
    struct stat st;
    int is_pipe = 0;
    if (fstat(infd, &st) == 0 && !S_ISREG(st.st_mode)) {
        is_pipe = 1;
        int fl = fcntl(infd, F_GETFL, 0);
        if (fl >= 0) fcntl(infd, F_SETFL, fl | O_NONBLOCK);
    }

    FILE *fout = NULL;
    if (out_path) {
        /* ★ 2026-10-27 修「手机提示投屏成功但屏上全黑」:
         *   输出端是命名管道时, fopen("wb") 就是 open(O_WRONLY) ——
         *   **没有读者时会永久阻塞** (wchan=wait_for_partner), 连一行日志都不打。
         *   现场 (16:00) 就是常驻播放器拿着一个已被 rm 掉的旧 fifo inode,
         *   新 fifo 没人读 ⇒ 解码器开局就卡死, 屏全黑, 而日志一片安静。
         *   现在: 非阻塞 open + 重试 10 s, 每秒一行日志, 超时就明确报错退出
         *   (退出比静默挂死好: 服务/脚本能看出失败, 人也能在日志里看到原因)。 */
        struct stat ost;
        int is_out_fifo = (stat(out_path, &ost) == 0 && S_ISFIFO(ost.st_mode));
        if (is_out_fifo) {
            int ofd = -1;
            for (int try = 0; try < 100; try++) {
                ofd = open(out_path, O_WRONLY | O_NONBLOCK);
                if (ofd >= 0) break;
                if (errno != ENXIO && errno != EINTR) {
                    fprintf(stderr, "[dec] ✗ 打不开输出管道 %s: %s\n", out_path, strerror(errno));
                    return 1;
                }
                if (try == 0)
                    fprintf(stderr, "[dec] ⏳ 输出管道 %s 目前没有读者 (drmplay 没在跑/拿着旧 inode), 最多等 10 s…\n",
                            out_path);
                else if (try % 20 == 19)
                    fprintf(stderr, "[dec] ⏳ 仍在等 %s 的读者 (%d s)…\n", out_path, (try + 1) / 10);
                struct timespec ts = { 0, 100 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            if (ofd < 0) {
                fprintf(stderr, "[dec] ✗ 放弃: %s 10 s 内没有读者 —— 播放器没起或持有旧管道 inode "
                                "(查 /var/log/k230-screen.log, 或 systemctl restart k230-screen)\n", out_path);
                return 1;
            }
            int fl = fcntl(ofd, F_GETFL, 0);
            if (fl >= 0) fcntl(ofd, F_SETFL, fl & ~O_NONBLOCK);   /* 有读者后恢复阻塞写 */
            fout = fdopen(ofd, "wb");
            if (fout) fprintf(stderr, "[dec] ✅ 输出管道 %s 已接上读者\n", out_path);
        } else {
            fout = fopen(out_path, "wb");
            /* 2026-10-28: 默认 stdio 缓冲只有 4 KB ⇒ 720p NV12 一帧 1.35 MB 要发
             * ~340 次 write() 系统调用, 单核上光系统调用+分片搬运就吃掉 4~5 ms。
             * 抬到 2 MB, 配合下面「每帧结束 fflush」= 一帧一组大块写。 */
            if (fout) setvbuf(fout, NULL, _IOFBF, 2u << 20);
        }
        if (!fout) { fprintf(stderr, "open %s: %s\n", out_path, strerror(errno)); return 1; }

        /* ---- 零拷贝: 帧体改走共享内存 (WFD_SHM), 管道只发 4 B 槽号 ---- */
        {
            const char *sp = getenv("WFD_SHM");
            if (sp && *sp) {
                size_t sz = (size_t)DEC_SHM_HEAD + 2 * (size_t)DEC_SHM_SLOT;
                int sfd = open(sp, O_RDWR | O_CREAT, 0666);
                void *m = MAP_FAILED;
                if (sfd >= 0) {
                    if (ftruncate(sfd, (off_t)sz) != 0)
                        fprintf(stderr, "[dec] ⚠ 共享内存 %s 扩到 %zu 失败: %s\n",
                                sp, sz, strerror(errno));
                    m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
                    close(sfd);
                }
                if (m != MAP_FAILED) {
                    g_shm = (uint8_t *)m;
                    g_shm_w = (volatile uint32_t *)(g_shm + 0);
                    g_shm_r = (volatile uint32_t *)(g_shm + 4);
                    *g_shm_w = 0; *g_shm_r = 0;
                    g_fsig = fout;                 /* 帧头 + 槽号 */
                    for (int k = 0; k < 2; k++) {
                        g_mem[k] = fmemopen(g_shm + DEC_SHM_HEAD + (size_t)k * DEC_SHM_SLOT,
                                            DEC_SHM_SLOT, "wb");
                        if (g_mem[k]) setvbuf(g_mem[k], NULL, _IONBF, 0);
                    }
                    if (!g_mem[0] || !g_mem[1]) {
                        fprintf(stderr, "[dec] ⚠ fmemopen 失败 → 帧体仍走管道\n");
                        g_shm = NULL; g_fsig = NULL;
                    } else {
                        fprintf(stderr, "[dec] 共享内存帧输出: %s (2 槽 × %u KB, 管道只发 4 B 槽号)\n",
                                sp, DEC_SHM_SLOT / 1024);
                    }
                } else {
                    fprintf(stderr, "[dec] ⚠ 共享内存 %s 不可用 (%s) → 帧体仍走管道\n",
                            sp, strerror(errno));
                }
            }
        }
        if (!strcmp(out_path, "-") || !strcmp(out_path, "/dev/stdout")) {
            setvbuf(fout, NULL, _IONBF, 0);   /* 管道要低延迟 */
            /* 2026-10-26: pipe 默认 64 KB, 720p 一帧 1.38 MB 要来回 21 趟,
             * 单核上每趟一次上下文切换 ⇒ drmplay 端「读管道」实测 21 ms/帧。
             *
             * 2026-10-27 再修: 原来只抬到 1 MB, 而 960x540 NV12 一帧 759 KB ⇒
             *   pipe 只装得下 **1.34 帧**, dec 写第二帧就阻塞、drmplay 读时还得
             *   等 dec 补齐剩下的 0.66 帧 ⇒ 「读管道」11.7 ms 里绝大部分是**等待**,
             *   整圈 16.9 ms 贴死 16.67 ms 的源节拍 (零余量), 内容一活跃就掉到 52 fps。
             *   /proc/sys/fs/pipe-max-size 虽是 1 MB, 但 root (CAP_SYS_RESOURCE)
             *   实测能设到 4 MB ⇒ 5.4 帧, dec 可连写 5 帧不阻塞, 读端一次拿满一帧。
             * 先试 4 MB, 失败再回退 1 MB (非 root 运行时)。 */
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031        /* 老 glibc 头里没有, 内核 uapi 值固定 */
#endif
            long got = fcntl(fileno(fout), F_SETPIPE_SZ, 4194304L);
            if (got <= 0)
                got = fcntl(fileno(fout), F_SETPIPE_SZ, 1048576L);
            if (got > 0)
                fprintf(stderr, "[dec] 管道缓冲 %ld 字节 (一帧 759 KB ⇒ 可装 %d 帧)\n",
                        (long)got, (int)(got / 777600));
            else
                fprintf(stderr, "[dec] 管道缓冲设置失败 (%s), 仍是默认值\n", strerror(errno));
        }
    }


    /* ★ 必须 O_NONBLOCK: 驱动/vb2 的 DQBUF 在无帧时会阻塞, 主循环里再用 poll 控节奏。
     *   不带它 → 采集缓冲一时没帧就直接卡死在 ioctl 里 (实测挂在 STREAMON 后第一帧)。 */
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", dev, strerror(errno)); return 1; }

    /* -L: 文件里有"x y w h dw dh" 就直接上硬件裁剪+缩放; 没有就本会话学习 */
    if (g_learn && g_crop_w == 0) {
        FILE *lf = fopen(g_learn, "r");
        if (lf) {
            int x, y, w, h, dw, dh;
            if (fscanf(lf, "%d %d %d %d %d %d", &x, &y, &w, &h, &dw, &dh) == 6 && w > 0 && h > 0) {
                g_crop_x = (unsigned)x; g_crop_y = (unsigned)y;
                g_crop_w = (unsigned)w; g_crop_h = (unsigned)h;
                g_dsl_w = (unsigned)dw; g_dsl_h = (unsigned)dh;
                g_fit_loaded = 1;
                fprintf(stderr, "[dec] ✅ 用已学内容区 %d,%d %dx%d → 硬件裁剪+缩放 %dx%d\n",
                        x, y, w, h, dw, dh);
            }
            fclose(lf);
        }
    }

    if (g_lowdelay) {
        /* V4L2_CID_MVE_VIDEO_FRAME_REORDERING = V4L2_CTRL_CLASS_CODEC(0x00240000) + 0x2000 + 7
         * 关掉帧重排: 解完立刻出帧 (WFD 流无 B 帧)。自定义控制要用 EXT_CTRLS, S_CTRL 会 EINVAL。 */
        struct v4l2_ext_controls ctrls;
        struct v4l2_ext_control ctrl;
        memset(&ctrls, 0, sizeof(ctrls));
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = 0x00242007;
        ctrl.value = 0;
        ctrls.ctrl_class = 0x00240000;   /* V4L2_CTRL_CLASS_CODEC */
        ctrls.count = 1;
        ctrls.controls = &ctrl;
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
            fprintf(stderr, "[dec] (FRAME_REORDERING 不支持, 跳过)\n");
        else
            fprintf(stderr, "[dec] ✅ 低延迟: 帧重排已关 (解完即出)\n");
    }

    if (g_crop_w > 0 && g_crop_h > 0) {
        struct mvx_crop_cfg crop = { 1, (unsigned short)g_crop_x, (unsigned short)g_crop_y,
                                     (unsigned short)g_crop_w, (unsigned short)g_crop_h };
        if (ioctl(fd, VIDIOC_S_MVX_DEC_DST_CROP, &crop) < 0) {
            fprintf(stderr, "[dec] ✗ VIDIOC_S_MVX_DEC_DST_CROP %u,%u %ux%u: %s (不裁)\n",
                    g_crop_x, g_crop_y, g_crop_w, g_crop_h, strerror(errno));
            g_crop_w = g_crop_h = 0;
        } else {
            fprintf(stderr, "[dec] ✅ 硬件裁剪 DEC_DST_CROPPING → %u,%u %ux%u\n",
                    g_crop_x, g_crop_y, g_crop_w, g_crop_h);
        }
    }

    /* ---- 0. 先读一小段码流找 SPS, 拿真实分辨率 (S_FMT 必须跟码流一致) ---- */
    uint8_t *hdr_buf = malloc(512 * 1024);
    size_t hdr_len = 0;
    if (!hdr_buf) return 1;
    {
        int has_sps = 0;
        double t_wait = now_sec();
        double t_sps_log = t_wait;              /* 等 SPS 期间每 30 s 打一行, 不刷屏 */
        while (!has_sps && hdr_len < 256 * 1024) {
            ssize_t n = inp_read(hdr_buf + hdr_len, 512 * 1024 - hdr_len);
            if (n > 0) {
                hdr_len += (size_t)n;
                unsigned sw, sh, cw, ch;
                if (sps_dims(hdr_buf, hdr_len, &sw, &sh, &cw, &ch) == 0) {
                    /* 2026-10-31 (二版): S_FMT 一律用 **coded** 尺寸 (16 对齐, 固件
                     *   按它分配采集缓冲); SPS 的裁剪区 (iPhone 竖屏 = 1280x720 里
                     *   中间 406x720) 由我们自己从解出的帧里裁 —— 见 sps_crop_rect /
                     *   emit 处的 g_vis_* 分支。原因: 固件按 coded 步长写、按可见宽
                     *   分配, 只要可见 < coded 就 DMA 越界 (板上 oops 实测)。 */
                    sps_crop_rect(hdr_buf, hdr_len);
                    g_has_crop = cropfix_on() && (g_vis_w > 0);
                    if (g_has_crop) {
                        /* 带 crop 的流 (iPhone 竖屏): 按 coded 尺寸解码, 可见区自己裁 */
                        w = sz_fit(cw ? cw : sw); h = sz_fit(ch ? ch : sh);
                        dlog("[dec] 🍎 检测到 SPS 裁剪 (%ux%u 可见 / %ux%u 编码) → 走苹果分支",
                             g_vis_w, g_vis_h, w, h);
                    } else {
                        /* 无 crop: 与黄金版本一致 —— S_FMT 用 SPS 报的尺寸, 不做吸附 */
                        w = sw; h = sh;
                    }
                    g_live_raw_w = sw; g_live_raw_h = sh;
                    g_live_w = w; g_live_h = h;
                    g_live_cw = cw ? cw : sw; g_live_ch = ch ? ch : sh;
                    has_sps = 1;
                }
                continue;
            }
            if (n == 0) break;                       /* EOF */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!is_pipe && now_sec() - t_wait > 3.0) break;
                if (is_pipe) { struct pollfd p = { .fd = infd, .events = POLLIN }; poll(&p, 1, 2000); }
                /* 2026-10-26: 原来 8 s 就放弃 → 用默认 1280x720 定型。实测 K80 有
                 * 一次 GET_PARAMETER 之后拖了 **9 s** 才发 M4, 等它开始推流时我们
                 * 已经把缓冲按 720p 建好了, 1080p 码流进来只能走重定容 (那时还有
                 * 独立的 bug) ⇒ 黑屏。手机侧停顿 3~10 s 属常见现象, 等 30 s 很划算:
                 * 拿到真 SPS 就一次性定型正确, 不需要中途重分配。 */
                if (now_sec() - t_wait > 30.0) {
                    /* ★ 2026-10-31 (iPhone 竖屏现场): 直播流**必须等真 SPS 定型**。
                     *   以前 30 s 没 SPS 就按默认 1280x720 建缓冲; 手机真流一来,
                     *   它的 SPS 裁剪成 406x720, 固件随即要求重建采集缓冲 —— 而本固件
                     *   重建必挂 (RASC ABORT / STREAMON EINVAL, 严重时卡死被看门狗复位)。
                     *   解码器本来就是常驻等投屏的, 多等一会儿没有任何代价。 */
                    if (now_sec() - t_sps_log >= 30.0) {
                        t_sps_log = now_sec();
                        dlog("[dec] ⏳ 仍在等 SPS (已等 %.0f s), 不按默认 %ux%u 定型 (等真 SPS 免得中途重建)",
                             now_sec() - t_wait, w, h);
                    }
                    continue;
                }
                continue;
            }
            break;
        }
        if (g_verbose)
            fprintf(stderr, "[dec] 读头 %zu 字节, 分辨率 %ux%u%s\n",
                    hdr_len, w, h, has_sps ? " (来自 SPS)" : " (没找到 SPS, 用参数默认值)");
    }

    /* ---- 硬件缩放目标 = 等比放最佳 (contain into 480x800) ----
     * 直接按码流(或 -c 裁剪后)的宽高算, 不做内容识别: 手机给什么就等比缩什么,
     * 黑边留着, 这样不同手机来源都稳定可预期。
     * ⚠ 必须在 S_FMT 之前设置 (固件加载后再设 = -EBUSY)。 */
    {
        unsigned sw = g_crop_w > 0 ? g_crop_w : w;
        unsigned sh = g_crop_h > 0 ? g_crop_h : h;
        /* 2026-10-26 (60fps): 阈值从 1M 降到 720p 档。
         * 原来只给 1080p 缩, 理由是「720p 在预算内」。但 720p 一帧 1.38 MB 过管道,
         * drmplay 每帧光「读管道」就 4.4~5.2 ms (占整圈 45%, 60fps 预算只有 16.7 ms)。
         * 硬件缩到 960x540 后同一段降到 1.9~3.5 ms —— 实测整链 89 → 112 fps。
         * 上屏仍按 480x270 采样, 源从 1280 宽变成 960 宽看不出差别。
         * 已知代价: 竖屏内容 (约 405x720) 会变 304x540 再放大 1.58 倍, 略软;
         * 要回旧行为 (720p 不缩): WFD_DSL_MIN_PIX=1048576 或干脆去掉管线里的 -A。 */
        /* 2026-10-27 (60fps 流畅度): 门槛回到 720p 档 (900000) —— 720p(921600) 也走
         * VPU 硬件降采样。原因 (板上实测, 2026-10-27 10:20):
         *   · 720p 全尺寸过管道, drmplay 每帧 读管道 22 ms + 转换 11 ms = 整圈 33 ms
         *     ⇒ 天花板 30 fps, 60fps 根本进不来;
         *   · 转换成本 = 源内容区像素数 (竖屏 337x720≈24 万), 与屏大小无关 ⇒ 只能靠
         *     **减小送进管道的源尺寸**, 不能靠调屏参数。
         *   · 采到 2×屏 (960x1600 等比 → 720p 变 960x540) 后: 源像素砍到 1/4,
         *     管道字节砍 44%, 而 drmplay 的绘制区仍是 480x800 里 contain 后的那块,
         *     **屏上出图大小/位置/比例一点不变** (只是放大倍数变小)。
         * 想回到"720p 不降采样": WFD_DSL_MIN_PIX=1000000。 */
        long dsl_min_pix = 900000L;
        {
            const char *o = getenv("WFD_DSL_MIN_PIX");
            if (o && *o) dsl_min_pix = strtol(o, NULL, 0);
        }
        /* 2026-10-27 (流畅度): 降采样目标框从「2×屏 (960x1600)」改成可配。
         * 为什么 (板上实测 15:0x 场次): 960x540 一帧 759 KB, drmplay 每帧光「读管道」
         * 就 12 ms, 而整圈预算只有 16.67 ms ⇒ 零余量, 手机一抖就错过 vblank (2vbl)。
         * 720p 内容在 480x800 屏上只画 480x270 ⇒ 源采到 2×屏 等于每个输出像素扔一半。
         * 采到 1×屏 (480x800 框 ⇒ 720p 变 480x270, 194 KB/帧) 直接把管道字节再砍 4 倍,
         * 出图大小/位置/比例不变 (只是放大倍数变小)。
         * 可用 WFD_DSL_BOX=WxH 临时改目标框做 A/B。
         * 2026-10-27 定版: 默认改成 1×屏 (480x800) —— 实测 (真机 1080p 码流,
         * 以 native→lanczos 480x270 为参考算 PSNR) 1x 比 2x **高 1.8 dB**
         * (35.31 vs 33.49 dB), 因为 2x 后面还要在 drmplay 里按 2:1 抽行抽列,
         * 等于把硬件缩放的抗混叠成果又扔掉一次。省下的 4 倍管道字节纯赚。 */
        long box_w = 480, box_h = 800;
        {
            const char *o = getenv("WFD_DSL_BOX");
            long bw = 0, bh = 0;
            if (o && *o && sscanf(o, "%ldx%ld", &bw, &bh) == 2 && bw >= 16 && bh >= 16) {
                box_w = bw;
                box_h = bh;
            }
        }
        if (g_autofit && (g_dsl_w == 0 || g_dsl_h == 0) &&
            (long)sw * sh >= dsl_min_pix) {
            /* 2026-10-26 实测 (1080p 源, 管道延迟 avg):
             *   不缩放 (dec 出 1920x1080, 3.1MB/帧) → 32~38 ms
             *   缩到 960x540 (777KB/帧)              → 23~28 ms, 画面 381x800 @49 (正确)
             *   缩到 480x270 (155KB/帧)              → 19~27 ms, 画面 382x800 @49 (偏软)
             * 瓶颈是「每帧 3.1MB 过管道」而不是缩放本身 ⇒ 硬件降采样 2 倍 (目标 2x 屏)
             * 就把这段砍掉约 10ms, 画面尺寸/比例与不缩放时一致 (黑边由 drmplay 裁)。
             * 目标 = 目标框 (默认 2×屏 960x1600, 可用 WFD_DSL_BOX=WxH 改) 等比, 且绝不放大 (>1 时取 1)。 */
            double f = (double)box_w / sw < (double)box_h / sh
                           ? (double)box_w / sw : (double)box_h / sh;
            if (f >= 0.999) {
                /* 源本身就不比目标框大 (如竖屏 720x1280) —— 1:1 缩放没意义, 别去设 */
                fprintf(stderr, "[dec] 帧 %ux%u 不比 %ldx%ld 大, 不做硬件缩放\n", sw, sh,
                        box_w, box_h);
            } else {
                int dw = ((int)(sw * f + 0.5)) & ~1;
                int dh = ((int)(sh * f + 0.5)) & ~1;
                if (dw > box_w) dw = (int)box_w;
                if (dh > box_h) dh = (int)box_h;
                if (dw < 2) dw = 2;
                if (dh < 2) dh = 2;
                g_dsl_w = (unsigned)dw;
                g_dsl_h = (unsigned)dh;
                fprintf(stderr, "[dec] 硬件降采样: %ux%u → %ux%u (等比含进 %ldx%ld, 黑边由 drmplay 裁)\n",
                        sw, sh, g_dsl_w, g_dsl_h, box_w, box_h);
            }
        }
    }
    /* VPU 硬件缩放: 必须在固件加载 (S_FMT/STREAMON) 之前设置, 否则 -EBUSY。
     * 设了之后采集缓冲/输出尺寸都按缩放后的算 (mvx_session.c:2434)。 */
    if (g_dsl_w > 0 && g_dsl_h > 0) {
        struct mvx_dsl_frame dsl = { g_dsl_w, g_dsl_h };
        if (ioctl(fd, VIDIOC_S_MVX_DSL_FRAME, &dsl) < 0) {
            fprintf(stderr, "[dec] ✗ VIDIOC_S_MVX_DSL_FRAME %ux%u: %s (退化为不缩放)\n",
                    g_dsl_w, g_dsl_h, strerror(errno));
            g_dsl_w = g_dsl_h = 0;
        } else {
            fprintf(stderr, "[dec] ✅ 硬件缩放 DEC_DOWNSCALE → %ux%u (CPU 不做缩放)\n",
                    g_dsl_w, g_dsl_h);
        }
    }

    /* ---- 1. 两个口先定型: OUTPUT=H264(码流), CAPTURE=NV12(图像) ---- */
    struct v4l2_format ofmt, cfmt;
    if (set_fmt(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_PIX_FMT_H264,
                w, h, w * h * 2, &ofmt) < 0)
        return 1;
    if (set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                g_rgb ? V4L2_PIX_FMT_ABGR32 : (g_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_NV12),
                w, h, 0, &cfmt) < 0)
        return 1;

    unsigned cap_w = cfmt.fmt.pix_mp.width, cap_h = cfmt.fmt.pix_mp.height;
    unsigned cap_planes = cfmt.fmt.pix_mp.num_planes ? cfmt.fmt.pix_mp.num_planes : 2;
    unsigned cap_stride[VIDEO_MAX_PLANES];
    cap_stride[0] = cfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    cap_stride[1] = cfmt.fmt.pix_mp.plane_fmt[1].bytesperline;
    cap_stride[2] = cfmt.fmt.pix_mp.plane_fmt[2].bytesperline;
    if (!cap_w) cap_w = w;
    if (!cap_h) cap_h = h;
    dlog("[dec] 定型: 码流 %ux%u H264 → 图像 %ux%u NV12 (%u planes, stride=[%u,%u])",
         ofmt.fmt.pix_mp.width, ofmt.fmt.pix_mp.height, cap_w, cap_h, cap_planes,
         cap_stride[0], cap_stride[1]);

    /* ---- 2. 建缓冲 (先 CAPTURE 再 OUTPUT, 两边都要在 STREAMON 前 Q 满) ---- */
    /* ⚠ 采集缓冲 2 会让 mvx 一帧都不出 (2026-10-25 实测 frames=0), 必须 4 */
    /* ★ 2026-10-28: 4 → 10。这不是「多给点更稳」的拍脑袋, 是驱动源码里的硬条件:
     *   mvx_session.c:3060 在 DQBUF 收帧时判: output->size[i] > mvx_buffer_size(buf,i)
     *   或 buffer_allocated < buffer_min  ⇒ 打 MVX_BUFFER_FRAME_NEED_REALLOC。
     *   而 buffer_min 是**固件解析完 SPS 后回报的** (mvx_session.c:3204,
     *   p->buffer_min = msg->seq_param.planar.buffers_min)。
     *   只给 4 个时 4 < buffers_min ⇒ 每轮都要求重分配 ⇒ 我 STREAMOFF/REQBUFS/
     *   STREAMON 重建, 而此刻固件已经在解码真实帧, 直接
     *   V5/V7-H264DEC: RASC ABORT (dmesg 铁证), 重建失败 ⇒ 永远 0 帧。
     *   厂商 mvx_decoder 重建后要的正是 10 个 (LD_PRELOAD 抓到的 ioctl 序列),
     *   照它给 10 个, NEED_REALLOC 根本不会触发。 */
    /* 2026-10-31: 10 -> 16。iPhone 的 AirPlay 流 (High profile, 多参考帧) 会在第一帧后
     * 由固件回 SEQ_PARAM 给出 buffers_min; 一旦它 > 已分配, 驱动就每帧打 NEED_REALLOC,
     * 而那时重建采集缓冲必被固件 RASC ABORT (实测 STREAMON EINVAL)。多给几个缓冲
     * 从源头避免这种"事后才知道要多少"的坑。Miracast 侧 10 也照旧够用。 */
    /* 16 只给带 crop 的流 (iPhone: 固件事后报 buffers_min, 10 个不够);
     * 其它流保持黄金版本的 10。 */
    int ncap = reqbufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, g_has_crop ? 16 : 10);
    if (ncap <= 0) return 1;
    int nout = reqbufs(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 4);
    if (nout <= 0) return 1;

    buf_t cap[MAX_BUF], outp[MAX_BUF];
    memset(cap, 0, sizeof(cap));
    memset(outp, 0, sizeof(outp));
    for (int i = 0; i < ncap; i++)
        if (query_mmap(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (unsigned)i, &cap[i]) < 0)
            return 1;
    for (int i = 0; i < nout; i++)
        if (query_mmap(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (unsigned)i, &outp[i]) < 0)
            return 1;

    /* 采集缓冲全部入队 (bytesused 必须为 0) */
    for (int i = 0; i < ncap; i++)
        if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (unsigned)i, cap[i].num_planes, 0) < 0)
            return 1;

    /* 一个输入缓冲装多少码流: 实测 ≥256KB 才稳 (64KB 会切断 AU → 固件弃流) */
    size_t chunk = (size_t)ofmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (chunk == 0 || chunk > CHUNK_DEFAULT) chunk = CHUNK_DEFAULT;
    if (chunk < CHUNK_PIPE) chunk = CHUNK_PIPE;
    if (getenv("DEC_CHUNK")) chunk = (size_t)atol(getenv("DEC_CHUNK"));   /* 调试用 */
    if (getenv("DEC_FLAG"))  g_flag  = atoi(getenv("DEC_FLAG"));
    if (getenv("DEC_CFG"))   g_cfg   = atoi(getenv("DEC_CFG"));
    if (getenv("DEC_AU"))    g_perau = atoi(getenv("DEC_AU"));

    /* ---- 3. 先喂满 OUTPUT 缓冲, 再开流 (m2m 两边都有缓冲才会出帧) ----
     * ⚠️ 关键坑: **绝不能**往 OUTPUT 口 queue bytesused==0 的缓冲 ——
     *    mvx 会把它当 EOS (驱动日志 `Firmware req: Buffer ... len=[0,0,0], eos=1`),
     *    第一帧就 `Capture EOS`。厂商 sample 就是这个坑。
     *    ⇒ 文件短于缓冲时, 空缓冲干脆**不入队**, 收尾统一用 DECODER_CMD(STOP)。 */
    int eof_in = 0;
    static int out_used[MAX_BUF];
    memset(out_used, 0, sizeof(out_used));

    feeder_t feeder;
    memset(&feeder, 0, sizeof(feeder));
    feeder.infd = infd;
    feeder.fd = fd;
    feeder.outp = outp;
    feeder.nout = nout;
    feeder.out_used = out_used;
    feeder.eof = &eof_in;
    feeder.chunk = chunk;
    feeder.stage = hdr_buf;          /* 读头时已经预读的码流交给 feeder 接着喂 */
    feeder.slen = hdr_len;
    feeder.scap = 512 * 1024;

    /* 先腹满输入缓冲再开流 (m2m 两边都有缓冲才会出帧) */
    if (g_verbose)
        fprintf(stderr, "[dec] 预填输入: %d/%d 个缓冲 (chunk=%zu)\n",
                feeder_fill(&feeder), nout, chunk);
    else
        feeder_fill(&feeder);

    /* ★ 开流前必须至少有一个输入缓冲在队列里: 直播时上游可能还没开始推,
     *   若空队就 STREAMON, 驱动不会出帧 (实测帧数永远 0)。 */
    {
        int waited = 0, any = 0;
        for (int i = 0; i < nout; i++) any += out_used[i];
        while (any == 0 && !eof_in && waited < 300) {      /* 最多等 30s */
            struct pollfd p = { .fd = infd, .events = POLLIN };
            poll(&p, 1, 100);
            feeder_fill(&feeder);
            any = 0;
            for (int i = 0; i < nout; i++) any += out_used[i];
            waited++;
        }
        if (g_verbose)
            fprintf(stderr, "[dec] 开流前入队 %d/%d 个输入缓冲 (等了 %d00ms)\n",
                    any, nout, waited);
    }

    if (streamon(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0) return 1;
    if (streamon(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) return 1;

    /* ---- 4. 主循环: 喂码流 / 收帧 ---- */
    long frames = 0;
    int stop_sent = 0;
    double t0 = now_sec();

    uint8_t *canvas = NULL;
    double t_last_frame = now_sec();
    int cap_empty = 0;        /* 连续「采到空缓冲」次数 (固件没写进帧) */
    double t_empty_chk = 0;   /* 上次「空缓冲」判定时刻 (按时间节流, 不按次数) */
    double t_empty_log = 0;   /* 上次「等 source」提示时刻 */
    int req_realloc = 0;      /* 1 = 下一轮开头重建采集缓冲 */
    unsigned req_w = 0, req_h = 0;
    int realloc_tries = 0;    /* 本会话已重建几次 (防死循环重建) */
    double t_rebuild = 0;     /* 最近一次重建完成时刻 (0=没重建过) */
    long frames_at_rebuild = 0;
#define REALLOC_STALL_SEC 3.0
    /* 兜底 realloc-reexec 的"重建后停滞"观察: 实测重建 STREAMON 都能过, 但固件
     * RASC 已 abort, 之后一帧不出 (tswitch 复现: 第二段 0 帧) —— 表现就是手机
     * 横竖屏一切就"连着没画面"。重建后 3 s 帧数不涨就原地重启自愈。 */
#define REALLOC_STALL() \
    (t_rebuild > 0 && frames <= frames_at_rebuild && \
     now_sec() - t_rebuild > REALLOC_STALL_SEC)
    for (;;) {
        /* ===== 采集缓冲重建 (2026-10-26 实测修: #6 黑屏的根因) =====
         * 触发源有两个: ① 驱动给 NEED_REALLOC 标志 (老版本固件才有)
         *              ② 连续 N 次采到空缓冲 (新固件只把缓冲原样退回, 不给标志)
         * 姿势必须是完整重建 (mvx 官方流程): STREAMOFF → REQBUFS(0) → munmap →
         * 按真实尺寸 S_FMT → REQBUFS(4) → QUERYBUF/mmap → 全部入队 → STREAMON。
         * 以前只在 NEED_REALLOC 分支里重发一次 S_FMT, 缓冲一个都没换 ⇒ 固件继续
         * 往小缓冲里塞 1080p, dmesg 刷 "Buffer plane too small", 画面全黑。 */
        if (req_realloc) {
            req_realloc = 0;
            unsigned nw = req_w ? req_w : cap_w, nh = req_h ? req_h : cap_h;
            struct v4l2_format f2;
            dlog("[dec] 重建采集缓冲: %ux%u → %ux%u (STREAMOFF/REQBUFS0/munmap/S_FMT/REQBUFS/mmap/STREAMON)",
                 cap_w, cap_h, nw, nh);
            streamoff(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
            reqbufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
            for (int i = 0; i < ncap; i++) {
                for (unsigned p = 0; p < cap[i].num_planes; p++)
                    if (cap[i].addr[p]) munmap(cap[i].addr[p], cap[i].len[p]);
                memset(&cap[i], 0, sizeof(cap[i]));
            }
            /* 尺寸没变就不要再 S_FMT (照抄厂商, 见上) */
            memset(&f2, 0, sizeof(f2));
            if (nw == cap_w && nh == cap_h) {
                f2.fmt.pix_mp.width  = cap_w;
                f2.fmt.pix_mp.height = cap_h;
                f2.fmt.pix_mp.num_planes = cap_planes;
                for (unsigned p = 0; p < cap_planes && p < VIDEO_MAX_PLANES; p++)
                    f2.fmt.pix_mp.plane_fmt[p].bytesperline = cap_stride[p];
            } else if (set_fmt(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                        g_rgb ? V4L2_PIX_FMT_ABGR32 : (g_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_NV12),
                        nw, nh, 0, &f2) < 0) {
                dlog("[dec] ✗ 重建 S_FMT 失败, 退出 (外壳会重起整条管线)");
                if (quirk_on("realloc-reexec")) {
                    realloc_reexec(argv, argc, nw, nh);
                    return 7;
                }
                break;
            }
            cap_w = f2.fmt.pix_mp.width ? f2.fmt.pix_mp.width : nw;
            cap_h = f2.fmt.pix_mp.height ? f2.fmt.pix_mp.height : nh;
            cap_planes = f2.fmt.pix_mp.num_planes ? f2.fmt.pix_mp.num_planes : 2;
            cap_stride[0] = f2.fmt.pix_mp.plane_fmt[0].bytesperline;
            cap_stride[1] = f2.fmt.pix_mp.plane_fmt[1].bytesperline;
            cap_stride[2] = f2.fmt.pix_mp.plane_fmt[2].bytesperline;
            ncap = reqbufs(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 16);   /* 10 → 16, 见上方注释 */
            if (ncap <= 0) {
                dlog("[dec] ✗ 重建 REQBUFS 失败, 退出");
                if (quirk_on("realloc-reexec")) {
                    realloc_reexec(argv, argc, nw, nh);
                    return 7;
                }
                break;
            }
            int _ok = 1;
            for (int i = 0; i < ncap && _ok; i++)
                if (query_mmap(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (unsigned)i, &cap[i]) < 0)
                    _ok = 0;
            for (int i = 0; i < ncap && _ok; i++)
                if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (unsigned)i,
                         cap[i].num_planes, 0) < 0)
                    _ok = 0;
            if (!_ok || streamon(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
                dlog("[dec] ✗ 重建后重开流失败: %s (ncap=%d _ok=%d)", strerror(errno), ncap, _ok);
                /* quirk realloc-reexec: 固件 RASC ABORT 后重建救不回来, 原地重启自愈 */
                if (quirk_on("realloc-reexec")) {
                    realloc_reexec(argv, argc, nw, nh);
                    return 7;   /* execv 成功不返回; 到这 = 兜底也没救 */
                }
                break;
            }
            dlog("[dec] ✅ 采集缓冲已重建: 图像 %ux%u, 缓冲 %d 个, stride=[%u,%u] (dmesg 不该再刷 Buffer plane too small)",
                 cap_w, cap_h, ncap, cap_stride[0], cap_stride[1]);
            cap_empty = 0;
            t_rebuild = now_sec();
            frames_at_rebuild = frames;
            continue;
        }

        /* 把攒够完整 AU 的码流投给空闲输入缓冲 */
        feeder_fill(&feeder);

        /* 码流结束且缓冲/暂存都空了: 驱动不会再回 LAST 时也要能退出
         * (文件回放实测没有 V4L2_BUF_FLAG_LAST, 只能靠 1.5s 无新帧判收尾) */
        if (eof_in && feeder.slen == 0 && now_sec() - t_last_frame > 1.5) {
            if (g_verbose)
                fprintf(stderr, "[dec] 输入结束且 1.5s 无新帧, 收尾\n");
            break;
        }

        struct pollfd pfds[2] = {
            { .fd = fd,   .events = POLLIN | POLLOUT, .revents = 0 },
            { .fd = infd, .events = POLLIN,           .revents = 0 },
        };
        if (g_verbose && now_sec() - t_last_frame > 2.0 &&
            (frames == 0 || (int)(now_sec() * 10) % 20 == 0))
            fprintf(stderr, "[dec] ...等待中 frames=%ld eof=%d slen=%zu\n",
                    frames, eof_in, feeder.slen);
        if (poll(pfds, is_pipe ? 2 : 1, 3000) < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[dec] poll: %s\n", strerror(errno));
            break;
        }
        if (pfds[0].revents == 0 && pfds[1].revents == 0) {
            if (is_pipe && !eof_in) {
                /* 直播流: 没数据是正常的; 但重建后停滞 = 固件已 RASC ABORT, 兜底重启 */
                if (REALLOC_STALL() && quirk_on("realloc-reexec")) {
                    dlog("[dec] ⚠ 重建后 %.0fs 帧数不涨 (frames=%ld), 疑固件已死 → 兜底",
                         REALLOC_STALL_SEC, frames);
                    realloc_reexec(argv, argc, cap_w, cap_h);
                    return 7;
                }
                continue;
            }
            fprintf(stderr, "[dec] 超时: 3s 内既没空缓冲也没新帧 (frames=%ld)\n", frames);
            if (REALLOC_STALL() && quirk_on("realloc-reexec")) {
                dlog("[dec] ⚠ 重建后停滞 (frames=%ld), 兜底重启", frames);
                realloc_reexec(argv, argc, cap_w, cap_h);
                return 7;
            }
            break;
        }

        /* 先收帧 (可能一次多帧) */
        for (;;) {
            struct v4l2_buffer b;
            struct v4l2_plane planes[VIDEO_MAX_PLANES];
            if (dqbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &b, planes) < 0)
                break;

            unsigned used = planes[0].bytesused;
            int last = (b.flags & V4L2_BUF_FLAG_LAST) != 0;
            int present = mvx_present(b.flags);

            if (g_verbose)
                fprintf(stderr, "[dec] <- cap idx=%u flags=0x%08x bytesused=%u (present=%d last=%d) seq=%u\n",
                       b.index, b.flags, used, present, last, b.sequence);

            /* 触发源 ①: 驱动明确要求重分配 (新旧两套标志都认, 见文件头说明) */
            /* 2026-10-31: 尺寸没变却要重分配 = 固件要求换一批缓冲 (buffer_min),
             * 就地重建 (STREAMOFF/REQBUFS0/S_FMT/REQBUFS/STREAMON) 即可, 尺寸不变
             * 所以不会触发固件 RASC ABORT。 */
            if (mvx_need_realloc(b.flags)) {
                struct v4l2_format gf;
                unsigned nw = g_live_w ? g_live_w : cap_w, nh = g_live_h ? g_live_h : cap_h;
                memset(&gf, 0, sizeof(gf));
                gf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                if (ioctl(fd, VIDIOC_G_FMT, &gf) == 0) {
                    if (gf.fmt.pix_mp.width)  nw = gf.fmt.pix_mp.width;
                    if (gf.fmt.pix_mp.height) nh = gf.fmt.pix_mp.height;
                }
                /* 驱动从 SPS 算出来的裁剪宽可能是任意像素 (406), 而 MVX 只认 16 对齐
                 * ⇒ 照它重建必然 STREAMON EINVAL。这里吸附后再建 (少 6 列)。 */
                nw = sz_fit(nw); nh = sz_fit(nh);
                dlog("[dec] 驱动要求重分配 (NEED_REALLOC flags=0x%08x) → 目标 %ux%u (当前缓冲 %ux%u)",
                     b.flags, nw, nh, cap_w, cap_h);
                /* ★ 2026-10-31 (iPhone 竖屏现场): 这类流固件要求把采集缓冲换成裁剪尺寸,
                 *   而**就地重建必挂** —— V5/V7-H264DEC: RASC ABORT → STREAMON EINVAL,
                 *   严重时卡死在 ioctl 里被看门狗复位。DEC_REALLOC_REEXEC=1 时不做就地
                 *   重建, 直接带新尺寸原地重启 (新会话按正确尺寸一次定型; 实测纯 406x720
                 *   流可正常解码)。Miracast 那条路不开这个变量, 行为不变。 */
                /* 2026-10-31: 尺寸**没变**却要重分配 ⇒ 这是固件后来才说 buffers_min,
                 * 就地重建会被固件 RASC ABORT (STREAMON EINVAL, 实测)。直接忽略这个
                 * 标志、继续解码 —— 缓冲区本来就够大, 只是个数少。 */
                if (g_has_crop && nw == cap_w && nh == cap_h) {
                    if (!g_realloc_same) {
                        g_realloc_same = 1;
                        dlog("[dec] ⚠ 驱动要求重分配但尺寸没变 (%ux%u = 当前), 忽略该标志继续解码", nw, nh);
                    }
                } else if (env_on("DEC_REALLOC_REEXEC")) {
                    dlog("[dec] 🔄 DEC_REALLOC_REEXEC=1 → 不做就地重建, 直接按 %ux%u 原地重启", nw, nh);
                    realloc_reexec(argv, argc, nw, nh);
                    return 7;
                }
                realloc_tries++;
                req_w = nw; req_h = nh; req_realloc = 1;
                break;      /* 手上这个 b 马上就要被 REQBUFS(0) 作废, 别再处理 */
            }

            /* 触发源 ②: 连续采到空缓冲 —— 新固件**不给** NEED_REALLOC 标志,
             * 缓冲尺寸不对时只把缓冲原样退回 (bytesused=0, present=0),
             * dmesg 刷 "Buffer plane too small", 我们这边表现为 100% CPU 空转 + 黑屏。
             * 按时间 (0.25 s) 判一次, 用码流里 SPS 的真实宽高重建缓冲。 */
            if (used == 0 && !present) {
                /* ★ 2026-10-28: 这里原来按「次数」判 (每 60 次判一次)。本板 DQBUF
                 * 非阻塞时一个空缓冲微秒级就退回, 实测 2 s 能空转 17 万次 ⇒
                 * ① 日志刷到每秒上千行 ② 单核 CPU 被这条循环吃满, uxplay 收包线程
                 * 饿死 (AirPlay 现场: UDP 7000 积压 2.9 MB, Mac 显示已连上却一帧不来)。
                 * 现在改成按**时间**判 (0.25 s 一次), 且空转时主动让出 3 ms。 */
                cap_empty++;
                double tnow = now_sec();
                if (tnow - t_empty_chk >= 0.25) {
                    t_empty_chk = tnow;
                    unsigned nw = 0, nh = 0;
                    /* ★ 2026-10-28: 开了 VPU 硬件降采样 (-s) 时, 采集缓冲尺寸
                     * (g_dsl_w/g_dsl_h) 与 SPS 里的码流尺寸**本来就不同** —— 固件
                     * 会把 1280x720 直接缩成 480x270 写进缓冲。这里若不排除这种情况,
                     * 就会把「尺寸不同」当成「缓冲不够大」反复重建 (AirPlay 现场:
                     * 1 s 一轮 STREAMOFF/S_FMT/STREAMON 死循环, 永远出不了帧)。
                     * Miracast 路径看不出来, 因为手机端已经缩成 480x270 再送。 */
                    int dsl_expected = (g_dsl_w > 0 && g_dsl_h > 0 &&
                                        cap_w == g_dsl_w && cap_h == g_dsl_h);
                    if (!dsl_expected && g_live_w && (g_live_w != cap_w || g_live_h != cap_h)) {
                        nw = g_live_w; nh = g_live_h;
                    } else {
                        struct v4l2_format gf;
                        memset(&gf, 0, sizeof(gf));
                        gf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                        if (ioctl(fd, VIDIOC_G_FMT, &gf) == 0 &&
                            (gf.fmt.pix_mp.width != cap_w || gf.fmt.pix_mp.height != cap_h)) {
                            nw = sz_fit(gf.fmt.pix_mp.width); nh = sz_fit(gf.fmt.pix_mp.height);
                        }
                    }
                    if (nw && nh && env_on("DEC_REALLOC_REEXEC")) {
                        dlog("[dec] 🔄 空缓冲判定尺寸不符 (%ux%u → %ux%u) → DEC_REALLOC_REEXEC=1: 原地重启",
                             cap_w, cap_h, nw, nh);
                        realloc_reexec(argv, argc, nw, nh);
                        return 7;
                    }
                    if (nw && nh && realloc_tries < 3) {
                        realloc_tries++;
                        dlog("[dec] ⚠ 连续 %d 次采到空缓冲 (固件没写进帧): 码流实际 %ux%u, 缓冲只有 %ux%u → 重建 (第 %d 次)",
                             cap_empty, nw, nh, cap_w, cap_h, realloc_tries);
                        req_w = nw; req_h = nh; req_realloc = 1;
                        break;
                    }
                    if (!nw && !nh && tnow - t_empty_log >= 5.0) {
                        t_empty_log = tnow;
                        dlog("[dec] ⚠ 空缓冲 %d 次 (缓冲 %ux%u 与 SPS 一致, 不是尺寸问题) —— 流没起来, 等 source",
                             cap_empty, cap_w, cap_h);
                    }
                    /* 兜底: 重建过 + 停滞 (实测 tswitch 第二段 0 帧就是这个形态) */
                    if (REALLOC_STALL() && quirk_on("realloc-reexec")) {
                        dlog("[dec] ⚠ 重建后停滞 (空缓冲 %d 次, frames=%ld), 兜底重启",
                             cap_empty, frames);
                        realloc_reexec(argv, argc, cap_w, cap_h);
                        return 7;
                    }
                }
                /* ★ 2026-10-28 (第二处): 没帧就把缓冲放回去、**立刻回主循环读码流**。
                 *   原来这里不 break, 在这条 drain 循环里一直转 (空缓冲 µs 级退回),
                 *   「填 OUTPUT / 读输入管道」的代码永远轮不到 ⇒ uxplay 写满命名管道
                 *   后阻塞在 write, RTP 收不动 (现场: UDP 7000 接收队列积压 936 KB),
                 *   Mac 显示已连接而 K230 一帧不出。 */
                if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index,
                         cap[b.index].num_planes, 0) < 0)
                    break;
                if (last)
                    goto done;
                if (cap_empty > 4) usleep(2000);
                break;
            } else {
                cap_empty = 0;
            }

            if (used > 0 && (present || used > 1) && b.index < (unsigned)ncap) {
                frames++;
                /* 学习模式: 前 20 帧探测内容区 (黑边外框), 存下来给下一轮硬件裁剪用 */
                if (g_learn && !g_fit_loaded && !g_rgb && !g_yuyv && frames <= 20) {
                    const uint8_t *yp = (const uint8_t *)cap[b.index].addr[0];
                    unsigned st = cap_stride[0] ? cap_stride[0] : cap_w;
                    int bx, by, bw, bh;
                    if (yp && detect_bbox_nv12(yp, (int)st, (int)cap_h, &bx, &by, &bw, &bh)) {
                        int x2 = bx + bw - 1, y2 = by + bh - 1;
                        if (g_lb_n == 0) { g_lb_x = bx; g_lb_y = by; g_lb_x2 = x2; g_lb_y2 = y2; }
                        else {
                            if (bx < g_lb_x) g_lb_x = bx;
                            if (by < g_lb_y) g_lb_y = by;
                            if (x2 > g_lb_x2) g_lb_x2 = x2;
                            if (y2 > g_lb_y2) g_lb_y2 = y2;
                        }
                        g_lb_n++;
                    }
                }
                /* 学 12 帧就够 (0.25s): 有了内容区立刻带硬件裁剪+缩放原地重启 */
                if (g_learn && !g_fit_loaded && !g_restart && is_pipe && frames == 12) {
                    int x = 0, y = 0, w = (int)cap_w, h = (int)cap_h;
                    double ta = 480.0 / 800.0, k;
                    int dw, dh;
                    g_restart = 1;
                    if (g_lb_n > 0) {
                        int lw = g_lb_x2 - g_lb_x + 1, lh = g_lb_y2 - g_lb_y + 1;
                        /* 探到的框太小 (< 画面 10%) 说明整幅画面本来就暗/在过渡 (刚连上
                         * 常见: 手机先把桌面压暗再切), 这时候按它裁 = 放大一小块 →
                         * 现场"屏上只有中间一小块"。宁可整帧不裁。 */
                        if ((double)lw * lh < 0.10 * (double)cap_w * cap_h) {
                            dlog("[dec] ⚠ 探到内容区只有 %dx%d (<10%% 画面), 判定不可信 → 整帧不裁", lw, lh);
                        } else {
                            x = g_lb_x; y = g_lb_y; w = lw; h = lh;
                        }
                    }
                    x &= ~1; y &= ~1; w &= ~1; h &= ~1;
                    if (w < 2) w = 2;
                    if (h < 2) h = 2;
                    /* 目标: 裁剪框比例 = 屏比例 (480:800) 且**完整包含内容**, VPU 直接出
                     * 480x800 ⇒ drmplay 走 1:1 直贴快路径, 转换 ~1.3 ms/帧。
                     * ★ 2026-11-03 改: 原来是把框**缩进**内容里 (cover), 会切掉内容:
                     *   实测 1920x1080 帧内 608x1080 的手机画面被判成 612x1020 ⇒
                     *   上下各切 30 行 (状态栏/导航栏), 屏上是满的但少了 5%。
                     *   现在改成**向黑边方向扩框**: 内容比屏窄就加宽 (竖屏手机两侧本来
                     *   就有黑边), 扩到帧边界还不够才退化成裁内容。这样比例对、像素不丢。 */
                    if ((double)w / h < ta) {
                        /* 内容偏"高" (竖屏手机) → 加宽到 h*ta, 借两侧黑边 */
                        int nw = (int)((double)h * ta + 0.5);
                        if (nw > (int)cap_w) nw = (int)cap_w;      /* 帧不够宽: 只能裁高 */
                        if (nw > (int)cap_w) nw = (int)cap_w;
                        int nx = x - (nw - w) / 2;
                        if (nx < 0) nx = 0;
                        if (nx + nw > (int)cap_w) nx = (int)cap_w - nw;
                        x = nx; w = nw;
                    } else {
                        /* 内容偏"宽" → 加高到 w/ta, 借上下黑边 */
                        int nh = (int)((double)w / ta + 0.5);
                        if (nh > (int)cap_h) nh = (int)cap_h;
                        int ny = y - (nh - h) / 2;
                        if (ny < 0) ny = 0;
                        if (ny + nh > (int)cap_h) ny = (int)cap_h - nh;
                        y = ny; h = nh;
                    }
                    w &= ~1; h &= ~1; x &= ~1; y &= ~1;
                    if (w < 2) w = 2;
                    if (h < 2) h = 2;
                    dw = 480; dh = 800;
                    (void)k;
                    learn_restart(argv, argc, x, y, w, h, dw, dh, g_learn);
                }
                t_last_frame = now_sec();
                if (fout) {
                    if (g_shm) {
                        /* 回压: 上一帧还没被显示进程消费完就等 (最多 2 s, 免得卡死) */
                        int wait_ms = 0;
                        while ((int32_t)(*g_shm_w - *g_shm_r) >= 1) {
                            if (wait_ms >= 2000) {
                                fprintf(stderr, "[dec] ⚠ 播放器 2 s 未消费 (w=%u r=%u), 强写\n",
                                        *g_shm_w, *g_shm_r);
                                break;
                            }
                            struct timespec ws = { 0, 500 * 1000 };
                            nanosleep(&ws, NULL);
                            wait_ms++;
                        }
                        /* 换槽 + 从头写: 帧体进共享内存, 帧头/槽号走管道 */
                        g_shm_seq++;
                        g_slot = g_shm_seq & 1;
                        fout = g_mem[g_slot];
                        fseek(fout, 0, SEEK_SET);
                    }
                    /* ⚠️ 驱动给的 bytesperline 可能 > width (对齐), 必须逐行拷,
                     *    否则画面打斜、与软解对比 PSNR 极低。 */
                    const uint8_t *base = cap[b.index].addr[0];
                    unsigned st0 = cap_stride[0] ? cap_stride[0] : cap_w;
                    const uint8_t *uv = cap[b.index].num_planes > 1 ?
                        cap[b.index].addr[1] : (base + (size_t)st0 * cap_h);
                    unsigned st1 = cap_stride[1] ? cap_stride[1] : cap_w;

                    if (g_has_crop && g_vis_w > 0 && g_dsl_w == 0 && !g_rgb && !g_yuyv &&
                        (g_vis_w != cap_w || g_vis_h != cap_h)) {
                        /* 2026-10-31: 可见区 < 解码区 (iPhone 竖屏镜像 = 1280x720 里
                         * 中间 406x720) —— 只把内容区逐行拷出去, 下游按 w x h 等比
                         * 放大到屏, 左右黑边不再占位。SPS 已去掉裁剪, 固件按 coded
                         * 分配缓冲 (它无法按可见宽分配, 会 DMA 越界)。 */
                        unsigned x0 = g_vis_x & ~1u, y0 = g_vis_y & ~1u;
                        unsigned ow = g_vis_w & ~1u, oh = g_vis_h & ~1u;
                        if (x0 + ow > cap_w) ow = (cap_w - x0) & ~1u;
                        if (y0 + oh > cap_h) oh = (cap_h - y0) & ~1u;
                        if (ow < 2 || oh < 2) {
                            ow = cap_w; oh = cap_h; x0 = 0; y0 = 0;
                        }
                        emit_hdr(fout, ow, oh);
                        for (unsigned yy = 0; yy < oh; yy++)
                            fwrite(base + (size_t)(y0 + yy) * st0 + x0, 1, ow, fout);
                        /* NV12 的 UV 是**交织**的: 每行 ow 字节 (=ow/2 对), 横向偏移
                         * 也是 x0 字节, 不是 x0/2 —— 写少一半会让下游按帧长读错位。 */
                        for (unsigned yy = 0; yy < oh / 2; yy++)
                            fwrite(uv + (size_t)(y0 / 2 + yy) * st1 + x0, 1, ow, fout);
                    } else if (g_dsl_w > 0 || (cap_w == w && cap_h == h)) {
                        emit_hdr(fout, cap_w, cap_h);
                        if (g_rgb) {
                            for (unsigned y = 0; y < cap_h; y++)
                                fwrite(base + (size_t)y * st0, 1, (size_t)cap_w * 4, fout);
                        } else if (g_yuyv) {
                            for (unsigned y = 0; y < cap_h; y++)
                                fwrite(base + (size_t)y * st0, 1, (size_t)cap_w * 2, fout);
                        } else if (st0 == (unsigned)cap_w && st1 == (unsigned)cap_w) {
                            /* 2026-10-28 快路径: 驱动 stride 正好等于宽 ⇒ Y/UV 各一次
                             * 大块写。原来逐行 fwrite 720+360 次 (每次 1.28 KB),
                             * 单核光是这 1080 次 stdio+write 就 4~5 ms。 */
                            fwrite(base, 1, (size_t)cap_h * st0, fout);
                            fwrite(uv, 1, (size_t)(cap_h / 2) * st1, fout);
                        } else {
                            for (unsigned y = 0; y < cap_h; y++)
                                fwrite(base + (size_t)y * st0, 1, cap_w, fout);
                            for (unsigned y = 0; y < cap_h / 2; y++)
                                fwrite(uv + (size_t)y * st1, 1, cap_w, fout);
                        }
                    } else if (g_yuyv) {
                        /* 少见: 实际分辨率 < 声明画布。固定 WxH 的 YUYV 画布, 贴左上, 其余填
                         * Y=16 / U=V=128 (黑) —— 下游按固定帧长读管道。 */
                        if (!canvas) {
                            canvas = malloc((size_t)w * h * 2);
                            if (!canvas) goto done;
                            memset(canvas, 128, (size_t)w * h * 2);
                            for (size_t i = 0; i < (size_t)w * h * 2; i += 2)
                                canvas[i] = 16;
                        }
                        for (unsigned y = 0; y < cap_h && y < h; y++)
                            memcpy(canvas + (size_t)y * w * 2,
                                   base + (size_t)y * st0, (size_t)(cap_w < w ? cap_w : w) * 2);
                        emit_hdr(fout, w, h);
                        fwrite(canvas, 1, (size_t)w * h * 2, fout);
                    } else {
                        /* 实际分辨率 < 声明的画布 (例如手机协商成 1024x768):
                         * 固定输出 WxH 画布, 画面贴左上, 其余填黑 — 下游 (drmplay)
                         * 才能按固定帧长读管道。 */
                        if (!canvas) {
                            canvas = malloc((size_t)w * h * 3 / 2);
                            if (!canvas) goto done;
                            memset(canvas, 16, (size_t)w * h);
                            memset(canvas + (size_t)w * h, 128, (size_t)w * h / 2);
                        }
                        for (unsigned y = 0; y < cap_h && y < h; y++)
                            memcpy(canvas + (size_t)y * w,
                                   base + (size_t)y * st0, cap_w < w ? cap_w : w);
                        uint8_t *cuv = canvas + (size_t)w * h;
                        for (unsigned y = 0; y < cap_h / 2 && y < h / 2; y++)
                            memcpy(cuv + (size_t)y * w,
                                   uv + (size_t)y * st1, cap_w < w ? cap_w : w);
                        emit_hdr(fout, w, h);
                        fwrite(canvas, 1, (size_t)w * h * 3 / 2, fout);
                    }
                    if (g_shm) {
                        /* 数据已经在槽里 (_IONBF, 无用户态缓冲), 先发布写序号再发"新帧"信号 */
                        uint32_t s = g_shm_seq;
                        *g_shm_w = s;
                        __sync_synchronize();
                        if (fwrite(&s, 1, 4, g_fsig) != 4 || fflush(g_fsig) != 0) {
                            /* 管道断了 (播放器退出/换 inode): 只有退出重来一条路 */
                            fprintf(stderr, "[dec] 发槽号失败: %s\n", strerror(errno));
                            goto done;
                        }
                    } else {
                        /* 一帧写完立刻推出 (stdio 缓冲 2 MB, 不 flush 会把上一帧憋在用户态) */
                        fflush(fout);
                    }
                }
            }

            /* 放回去继续收 */
            if (qbuf(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index,
                     cap[b.index].num_planes, 0) < 0)
                break;

            if (last)
                goto done;
            if (max_frames > 0 && frames >= max_frames)
                goto done;
        }

        /* 收完帧后把耗尽的 OUTPUT 缓冲标成空闲 (填数据的活由循环顶统一做) */
        for (;;) {
            struct v4l2_buffer b;
            struct v4l2_plane planes[VIDEO_MAX_PLANES];
            if (dqbuf(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &b, planes) < 0)
                break;
            if (b.index < (unsigned)nout)
                out_used[b.index] = 0;
        }

        /* 码流读完 → 发一次 EOS, 让驱动把内部缓存的帧吐完 */
        if (eof_in && !stop_sent) {
            struct v4l2_decoder_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = V4L2_DEC_CMD_STOP;
            if (xioctl(fd, VIDIOC_DECODER_CMD, &cmd, "DECODER_CMD(STOP)") == 0) {
                stop_sent = 1;
                if (g_verbose) fprintf(stderr, "[dec] 码流喂完, 已发 EOS (DEC_CMD_STOP)\n");
            }
        }

    }

done:
    if (g_learn && !g_fit_loaded && g_lb_n > 0) {
        /* 铺满屏幕 (cover): 把内容区裁成屏幕比例 (480:800), 再整屏缩放 —— 没有黑边,
         * 代价是超出屏幕比例的边缘被裁掉。内容区外的黑边本来就不进画面。 */
        int x = g_lb_x, y = g_lb_y, w = g_lb_x2 - g_lb_x + 1, h = g_lb_y2 - g_lb_y + 1;
        double ta = 480.0 / 800.0;
        if ((double)w / h > ta) {
            int nw = (int)(h * ta + 0.5);
            x += (w - nw) / 2;
            w = nw;
        } else {
            int nh = (int)(w / ta + 0.5);
            y += (h - nh) / 2;
            h = nh;
        }
        w &= ~1; h &= ~1; x &= ~1; y &= ~1;   /* NV12 要求偶数对齐 */
        if (w < 2) w = 2;
        if (h < 2) h = 2;
        FILE *lf = fopen(g_learn, "w");
        if (lf) {
            fprintf(lf, "%d %d %d %d %d %d\n", x, y, w, h, 480, 800);
            fclose(lf);
            fprintf(stderr, "[dec] ✅ 已学习内容区 → 铺满裁剪 %d,%d %dx%d → 480x800 (写 %s)\n",
                    x, y, w, h, g_learn);
        }
    } else if (g_learn && g_fit_loaded) {
        fprintf(stderr, "[dec] (本会话直接用学习结果做硬件裁剪+缩放, 不重复学习)\n");
    } else if (g_learn) {
        fprintf(stderr, "[dec] ⚠ 学习失败 (没探到内容区), 下轮仍走 CPU 路径\n");
    }
    if (g_verbose && outp[0].num_planes >= 1) {
        fprintf(stderr, "[dec] 缓冲: capture planes=%u len=[%zu,%zu] bytesperline=[%u,%u]\n",
               cap[0].num_planes, cap[0].len[0], cap[0].len[1],
               cap_stride[0], cap_stride[1]);
    }
    {
        /* 调试用: 把最后一个收到的 UV 平面整块存盘, 便于离屏核对 stride/偏移 */
        if (getenv("DEC_DUMP_UV") && cap[0].addr[1])
            fwrite(cap[0].addr[1], 1, cap[0].len[1], fopen(getenv("DEC_DUMP_UV"), "wb"));
    }
    {
        double dt = now_sec() - t0;
        dlog("[dec] 收到 %ld 帧, %.2f s, %.1f fps, 分辨率 %ux%u NV12%s",
             frames, dt, dt > 0 ? frames / dt : 0.0, cap_w, cap_h,
             fout ? " (已落盘)" : "");
    }
    streamoff(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    streamoff(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    close(fd);
    if (fout) fclose(fout);
    close(infd);
    return frames > 0 ? 0 : 3;
}
