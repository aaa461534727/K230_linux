/*
 * uibc.c —— UIBC (反控): 把本板触摸屏的触摸事件回传给 source (手机)
 *
 * 背景: K230 当 Miracast Sink, 屏上触摸需要"反控"手机 —— 走 WFD 1.1 的 UIBC
 * (User Input Back Channel)。手机侧 (Android WifiDisplaySource) 会起一个
 * UIBC TCP 服务端, sink 作为 TCP 客户端连上去, 之后按 UIBC 包格式推输入事件。
 *
 * 本实现 (2026-10-27 重写通道建立部分):
 *   - 支持 GENERIC 输入类别 (SingleTouch/MultiTouch), 用单指触摸 + 归一化坐标;
 *   - **先**按 source 通告的 port 反复 connect (每 400 ms 一次, 直到会话结束);
 *     source 起服务端慢 (实测 M3 之后 10~30 s 才 listen) 也能在它起来后立刻连上;
 *   - 同时 (2 s 后) 在本端 port 监听作兜底, 少数机型是 sink 当服务端;
 *   - 每次 connect 失败都打印 errno (ECONNREFUSED = 手机没 listen / ETIMEDOUT = 被丢);
 *   - **通告端口不可信**: 2026-10-27 现场, Redmi K80 Ultra (HyperOS 3.0) 在 M3 里
 *     声明 port=7239, 但它自己 /proc/net/tcp 里压根没有 7239, 真正的 UIBC 服务端
 *     监听在 0.0.0.0:4321 (板端直连 4321 立即通过)。所以按
 *     [上次成功的口, 通告的口, 4321, 7239] 依次试, 第一个通的就固定下来;
 *   - 通道断了自动重连 (手机切前后台会重建)。
 *
 * 触摸事件按 SYN_REPORT 成帧处理: 先收齐"按下 + 坐标"再发一包 —— 老实现
 * 在 ABS_MT_TRACKING_ID 时就用上一帧坐标(常常是 0,0)发按下, 首包坐标可能是错的。
 *
 * UIBC GENERIC 包 (WFD 1.1 §4.11 Table 15, 2026-10-28 真机反汇编+实测定论):
 *   公共头 4 B (两个 16 位大端字):
 *     word0 = version(0..2) | T(3) | reserved(4..11) | category(12..15)  ⇒ [0]=[1]=0x00
 *     word1 = [2][3] = 整包字节数 (含这 4 B 头, 大端)
 *   正文: [4] type(0=Down 1=Up 2=Move) | [5][6] 正文字节数 (大端) | [7] 指针数
 *         | {id(1) x(2 大端) y(2 大端)}*
 *
 * ⚠ 坐标 = **会话视频帧像素** (0..1279 / 0..719), 不是 0..65535 归一化。
 *   手机侧 (libwfdnative) 先 xin = x/1280、yin = y/720, 再 adjustForAspectRatio。
 *   发 0..65535 会被当成像素 32768 ⇒ 手机报 Invalid number of inputs 全丢。
 *
 * ⚠ 竖屏手机还有第二次变换: 帧是 1280x720 (16:9), 手机屏是 9:16, 手机认为内容
 *   只占帧中间 1/f 的横向范围 (f = resAspect/screenAspect = 3.1605), 于是
 *     xout = 0.5 + (xin - 0.5) * f
 *   ⇒ 发给手机的 x 必须落在内容区 (帧像素 437..842) 里, 否则 xout 出 [0,1] 被
 *     "Not injecting rogue touches" 丢弃 (默认不打日志, 表现为点了没反应)。
 *   本文件用「屏上矩形 → 手机屏归一化 → 帧内内容区 → 帧像素」三步换算, 见
 *   uibc_map_xy(); 两个矩形都能用环境变量覆盖 (见 WFD_UIBC_AREA / WFD_UIBC_SPAN)。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/input.h>

#include "uibc.h"

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif
#ifndef ABS_MT_TRACKING_ID
#define ABS_MT_TRACKING_ID 0x39
#endif
#ifndef ABS_MT_POSITION_X
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#endif

#define UIBC_DEF_PORT 7239
#define UIBC_VER      1

static int  g_port  = UIBC_DEF_PORT;
static int  g_fd    = -1;          /* 到 source 的 UIBC 连接 */
static int  g_live  = 0;
static volatile int g_run = 0;
static pthread_t g_th;
static int  g_th_started = 0;
static int  g_th_done = 0;
static char g_peer[64] = { 0 };
static int  g_peer_port = 0;

/* 候选端口机制: source 通告的 port 常常不是它真正 listen 的那个 (见文件头)。
 * g_learned = 上次连成功的口, 下次优先用它, 断开重连不再走一遍苦试。 */
#define UIBC_ALT_PORT   4321    /* Android/MIUI UIBC 服务端默认口 (实测) */
#define UIBC_MAX_CAND   4
static int g_learned = 0;

/* 生成候选口: [learned, declared, 4321, 7239], 去重; 返回个数 */
static int build_candidates(int declared, int *out)
{
    int src[UIBC_MAX_CAND];
    int n = 0, cnt = 0;

    src[n++] = g_learned;
    src[n++] = declared;
    src[n++] = UIBC_ALT_PORT;
    src[n++] = UIBC_DEF_PORT;
    for (int i = 0; i < n; i++) {
        int p = src[i];
        if (p <= 0 || p > 65535)
            continue;
        int dup = 0;
        for (int k = 0; k < cnt; k++)
            if (out[k] == p) { dup = 1; break; }
        if (!dup)
            out[cnt++] = p;
    }
    return cnt;
}

/* 触摸设备坐标范围 (EVIOCGABS 读出来), 用来换算到 480x800 屏像素 */
static int g_xmin = 0, g_xmax = 0, g_ymin = 0, g_ymax = 0;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int uibc_port(void) { return g_port; }
int uibc_active(void) { return g_live; }

/* 拼 M3 里的 wfd_uibc_capability 值 (sink 侧声明自己能收的单指/多指 + 本端监听端口) */
const char *uibc_capability(void)
{
    static char buf[192];
    /* WFD_UIBC_CAP: 整串覆盖 (须自带 port=), 用于现场试不同能力串而不用重编译 */
    const char *ov = getenv("WFD_UIBC_CAP");

    if (ov && *ov)
        return ov;
    /* 2026-10-28 K80 实测: 逗号后面**不能有空格**。原来写 "SingleTouch, MultiTouch",
     * 小米/MTK 侧把 " MultiTouch" 当未知类型 ⇒ 打 "devType:1 is not supported" 并且
     * UIBC 通道压根没通 (Connection refused), 反控彻底失效。去掉空格后立刻正常。
     * 努比亚对两种写法都能解析, 所以去空格是纯兼容性提升。 */
    snprintf(buf, sizeof(buf),
             "input_category_list=GENERIC;generic_cap_list=SingleTouch,MultiTouch;"
             "hidc_cap_list=none;port=%d", g_port);
    return buf;
}

static int write_all(int fd, const char *p, int n)
{
    int off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, (size_t)(n - off), MSG_NOSIGNAL);
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        return -1;
    }
    return 0;
}

/* 发一帧 UIBC GENERIC 触摸 (WFD 1.1 §4.11 / Table 15, 2026-10-28 按规范重写)
 *
 * 公共头 4 字节 (两个 16 位字, 均为网络序/大端):
 *   word0: b0..2 version(=0) | b3 T(=0) | b4..11 reserved | b12..15 input category(0=GENERIC)
 *          ⇒ 字节 [0]=0x00, [1]=0x00
 *   word1: [2][3] = Length (整个包长, 含这 4 字节头, 单位字节, 大端)
 * GENERIC 正文字段:
 *   [4]     Input Type ID  (0=TouchDown 1=TouchUp 2=TouchMove)
 *   [5][6]  Generic Length (正文字节数, 大端)
 *   [7]     Describe: 指针个数
 *   [8..]   每个指针: id(1) + x(2 大端) + y(2 大端), x/y 为**会话帧像素**
 *
 * 旧实现 (ver=1 + 4 字节小端时间戳 + cat/nptr 拼在一起) 与规范完全不符, 手机侧
 * 解析器根本认不出包 —— 这就是"通道连上了但触摸没反应"的原因。 */
static int uibc_send_touch(int down, int x, int y)
{
    unsigned char b[13];
    int n;

    /* 抬手包必须**也带坐标** (2026-10-28 K80 实测):
     *   只带 id 的 9 字节抬手包, MTK 的 UibcServerHandler 只走到
     *   "TOUCH_UP: pointerID:0 x:0 y:0" 就不再往下派发 (还会打一行
     *   devType:1 is not supported), 手机侧触摸状态**一直停在按下**。
     *   后果就是"只有连上后的第一下能生效, 之后怎么点都没反应"。
     *   抬手改成和按下同布局 (13 B, 带最后坐标) 后, 手机日志变成
     *   "two TOUCH_UP" + strUibcJson action=1 → 点按真正落到 UI 上。
     * 想退回老格式 (现场对比用): WFD_UIBC_UP=short */
    static int lastx, lasty;
    const char *upm = getenv("WFD_UIBC_UP");
    int up_full = !(upm && *upm && strcmp(upm, "short") == 0);

    if (g_fd < 0)
        return -1;
    memset(b, 0, sizeof(b));
    if (down) {
        lastx = x;
        lasty = y;
        n = 13;                                  /* 4 头 + 1 type + 2 len + 6 describe */
        b[3] = (unsigned char)n;
        b[4] = 0x00;                             /* TouchDown */
        b[6] = 0x06;                             /* describe 长度 */
        b[7] = 0x01;                             /* 1 个指针 */
        b[8] = 0x00;                             /* pointer id */
        b[9]  = (unsigned char)((x >> 8) & 0xff);
        b[10] = (unsigned char)(x & 0xff);
        b[11] = (unsigned char)((y >> 8) & 0xff);
        b[12] = (unsigned char)(y & 0xff);
    } else if (up_full) {
        n = 13;                                  /* 抬手也带坐标 (K80 需要) */
        b[3] = (unsigned char)n;
        b[4] = 0x01;                             /* TouchUp */
        b[6] = 0x06;
        b[7] = 0x01;
        b[8] = 0x00;
        b[9]  = (unsigned char)((lastx >> 8) & 0xff);
        b[10] = (unsigned char)(lastx & 0xff);
        b[11] = (unsigned char)((lasty >> 8) & 0xff);
        b[12] = (unsigned char)(lasty & 0xff);
    } else {
        n = 9;                                   /* 4 头 + 1 type + 2 len + 2 describe */
        b[3] = (unsigned char)n;
        b[4] = 0x01;                             /* TouchUp */
        b[6] = 0x02;
        b[7] = 0x01;                             /* 1 个指针 */
        b[8] = 0x00;                             /* pointer id */
    }
    return write_all(g_fd, (const char *)b, n);
}

/* 触摸移动: 与 down 同布局, 只是 Input Type ID = 2 */
static int uibc_send_move(int x, int y)
{
    unsigned char b[13];
    int n = 13;

    if (g_fd < 0)
        return -1;
    memset(b, 0, sizeof(b));
    b[3] = (unsigned char)n;
    b[4] = 0x02;                                 /* TouchMove */
    b[6] = 0x06;
    b[7] = 0x01;
    b[8] = 0x00;
    b[9]  = (unsigned char)((x >> 8) & 0xff);
    b[10] = (unsigned char)(x & 0xff);
    b[11] = (unsigned char)((y >> 8) & 0xff);
    b[12] = (unsigned char)(y & 0xff);
    return write_all(g_fd, (const char *)b, n);
}

/* ===================== 触摸坐标映射 =====================
 * 三步换算 (2026-10-28 努比亚 Z17 miniS 真机标定, 详见文件头注释):
 *   ① 触摸设备原始坐标 → K230 屏像素 (480x800)
 *   ② K230 屏像素 → 手机屏归一化 0..1  (g_area = 屏上手机画面所占矩形)
 *   ③ 手机屏归一化 → 会话帧内内容区 → 帧像素 (g_span = 帧内内容区归一化范围)
 * ② 的矩形: K230 480x800 上手机画面在 x 19..469、y 0..799 (drmdump 实测)
 * ③ 的矩形: 竖屏机 1280x720 帧里内容只占 x 0.3418..0.6582 (405 px), y 全高
 * 两个都可用环境变量覆盖, 换手机/换出图几何不用重编译:
 *   WFD_UIBC_AREA="19,469,0,799"     ② 的矩形 (K230 屏像素)
 *   WFD_UIBC_SPAN="0.3418,0.6582,0,1" ③ 的矩形 (帧内归一化)
 * 另: 若存在 /run/wfd-video-geom (4 个归一化数, x0 x1 y0 y1), 优先用它当 ③ —— 给
 * "换任意手机自动适配"留的接口 (由解码/上屏侧实测黑边后写入)。
 */
#define DISP_W 480
#define DISP_H 800
#define FRAME_W 1280
#define FRAME_H 720

static double g_area[4] = { 19, 469, 0, 799 };
static double g_span[4] = { 0.341797, 0.658203, 0, 1 };
static int g_geom_ready = 0;

static void parse4(const char *s, double *o)
{
    double a, b, c, d;
    if (s && sscanf(s, "%lf,%lf,%lf,%lf", &a, &b, &c, &d) == 4 &&
        b > a && d > c) {
        o[0] = a; o[1] = b; o[2] = c; o[3] = d;
    }
}

static void geom_init(void)
{
    if (g_geom_ready)
        return;
    g_geom_ready = 1;
    parse4(getenv("WFD_UIBC_AREA"), g_area);
    parse4(getenv("WFD_UIBC_SPAN"), g_span);
    {
        FILE *fp = fopen("/run/wfd-video-geom", "r");
        if (fp) {
            double g[4];
            if (fscanf(fp, "%lf %lf %lf %lf", &g[0], &g[1], &g[2], &g[3]) == 4 &&
                g[1] > g[0] && g[3] > g[2])
                memcpy(g_span, g, sizeof(g));
            fclose(fp);
        }
    }
    fprintf(stderr, "[UIBC] 坐标映射: 屏上画面 x %.0f..%.0f y %.0f..%.0f | "
            "帧内内容区 x %.4f..%.4f y %.4f..%.4f\n",
            g_area[0], g_area[1], g_area[2], g_area[3],
            g_span[0], g_span[1], g_span[2], g_span[3]);
}

/* ---- 上屏几何 (由 drmplay 发布到 /run/wfd-uibc-geom) ----
 * 有它就用它: 1:1 直贴时画面贴在屏中央, 用写死的屏幕矩形会让触点整体偏移
 * (实测屏上画面 x 36..440 y 40..759, 老值 19..469 / 0..799 ⇒ 边缘差 40 px)。
 * 读不到文件就退回 g_area/g_span 环境变量那套老算法, 行为完全不变。 */
static int uibc_screen_geom(double *ox, double *oy, double *dw, double *dh,
                            double *cx, double *cy, double *cw, double *ch)
{
    double v[8];
    int ok;
    FILE *fp = fopen("/run/wfd-uibc-geom", "r");

    if (!fp)
        return 0;
    ok = (fscanf(fp, "%lf %lf %lf %lf %lf %lf %lf %lf",
                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8);
    fclose(fp);
    if (!ok || v[2] <= 0 || v[3] <= 0 || v[6] <= 0 || v[7] <= 0)
        return 0;
    *ox = v[0]; *oy = v[1]; *dw = v[2]; *dh = v[3];
    *cx = v[4]; *cy = v[5]; *cw = v[6]; *ch = v[7];
    return 1;
}

/* 触摸原始坐标 → 发送用的帧像素 */
static void uibc_map_xy(int rx, int ry, int *opx, int *opy)
{
    int xmax = (g_xmax > g_xmin) ? g_xmax : DISP_W - 1;
    int ymax = (g_ymax > g_ymin) ? g_ymax : DISP_H - 1;
    double dx, dy, xs, ys, xin, yin;

    geom_init();
    dx = (double)(rx - g_xmin) * DISP_W / (double)(xmax - g_xmin + 1);
    dy = (double)(ry - g_ymin) * DISP_H / (double)(ymax - g_ymin + 1);

    /* 屏上真实几何优先 (drmplay 每帧算出来的, 见 /run/wfd-uibc-geom) */
    {
        double gox, goy, gdw, gdh, gcx, gcy, gcw, gch;
        if (uibc_screen_geom(&gox, &goy, &gdw, &gdh, &gcx, &gcy, &gcw, &gch)) {
            double fx = gcx + (dx - gox) * gcw / gdw;
            double fy = gcy + (dy - goy) * gch / gdh;
            if (fx < gcx) fx = gcx;
            if (fx > gcx + gcw - 1) fx = gcx + gcw - 1;
            if (fy < gcy) fy = gcy;
            if (fy > gcy + gch - 1) fy = gcy + gch - 1;
            *opx = (int)(fx + 0.5);
            *opy = (int)(fy + 0.5);
            return;
        }
    }

    xs = (dx - g_area[0]) / (g_area[1] - g_area[0]);
    ys = (dy - g_area[2]) / (g_area[3] - g_area[2]);
    if (xs < 0) xs = 0;
    if (xs > 1) xs = 1;
    if (ys < 0) ys = 0;
    if (ys > 1) ys = 1;

    xin = g_span[0] + xs * (g_span[1] - g_span[0]);
    yin = g_span[2] + ys * (g_span[3] - g_span[2]);

    *opx = (int)(xin * FRAME_W + 0.5);
    *opy = (int)(yin * FRAME_H + 0.5);
    if (*opx < 0) *opx = 0;
    if (*opx > FRAME_W - 1) *opx = FRAME_W - 1;
    if (*opy < 0) *opy = 0;
    if (*opy > FRAME_H - 1) *opy = FRAME_H - 1;
}

/* 测试注入通道 (WFD_UIBC_TESTIN=1, **仅调试**): 在生产输入循环里顺手看一眼
 * /run/uibc-touch, 这样"不用手按屏"也能验证包有没有被手机受理。
 * 格式: "tx ty" 点一下 | "D tx ty" 按下 | "M tx ty" 拖动 | "U" 抬手
 *      (tx/ty = K230 屏像素, 走与真实触摸完全相同的 uibc_map_xy)。
 *
 * ⚠ 为什么不复用 WFD_UIBC_MAPTAP: 那个模式是**替换**掉 uibc_input_loop 的
 *   (只轮询文件、根本不读 /dev/input/event0) —— 只要它被设上, 手指按屏就
 *   一个包都不发。2026-10-28 现场「触摸 K230 屏不能控制手机」就是被这个
 *   残留的自测开关挡住的, 不是协议也不手机的问题。 */
static void testin_poll(void)
{
    char line[256];
    char op = 'T';
    int tx = 0, ty = 0, px = 0, py = 0;
    FILE *fp = fopen("/run/uibc-touch", "r");
    if (!fp)
        return;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);
    truncate("/run/uibc-touch", 0);
    if (line[0] == 'D' || line[0] == 'M' || line[0] == 'U')
        op = line[0];
    if (op == 'U') {
        uibc_send_touch(0, 0, 0);
        fprintf(stderr, "[UIBC] 🧪 注入 抬手\n");
        return;
    }
    if (sscanf(op == 'T' ? line : line + 1, "%d %d", &tx, &ty) != 2)
        return;
    uibc_map_xy(tx, ty, &px, &py);
    if (op == 'D') {
        uibc_send_touch(1, px, py);
        fprintf(stderr, "[UIBC] 🧪 注入 按下 (%d,%d)→(%d,%d)\n", tx, ty, px, py);
    } else if (op == 'M') {
        uibc_send_move(px, py);
        fprintf(stderr, "[UIBC] 🧪 注入 拖动 (%d,%d)→(%d,%d)\n", tx, ty, px, py);
    } else {
        uibc_send_touch(1, px, py);
        usleep(60 * 1000);
        uibc_send_touch(0, 0, 0);
        fprintf(stderr, "[UIBC] 🧪 注入 点击 (%d,%d)→(%d,%d)\n", tx, ty, px, py);
    }
}

/* 读触摸事件并回传, 直到连接断开或 g_run 清零
 *
 * 2026-10-27 改: 按 SYN_REPORT 成帧。ft5x06 是 type-B 协议, 一帧里先给
 * ABS_MT_TRACKING_ID 再给 X/Y —— 老实现看到 TRACKING_ID 就用"上一帧"的坐标
 * 发按下 (首帧 lx/ly = -1 → 发成 (0,0)), 而且紧接的 X/Y 又被 8 ms 节流挡掉,
 * 结果每一下点都落在屏幕左上角。现在把一帧收全再发。 */
static void uibc_input_loop(int ufd, int lfd)
{
    const char *dev = getenv("WFD_UIBC_DEV");
    const char *dbg = getenv("WFD_UIBC_DEBUG");
    const char *tin = getenv("WFD_UIBC_TESTIN");
    int testin = (tin && *tin && strcmp(tin, "0") != 0) ? 1 : 0;
    int debug_events = (dbg && *dbg && strcmp(dbg, "0") != 0) ? 60 : 0;
    int ifd;
    int cur_down = 0;          /* 本帧按下的指针 (TRACKING_ID >= 0) */
    int sent_down = 0;         /* 已经告诉手机的按下状态 */
    int lx = -1, ly = -1;      /* 最新原始坐标 (屏幕像素) */
    int sx = -1, sy = -1;      /* 最近一次发出去的归一化坐标 */
    long long last_ms = 0;
    long sent = 0;

    if (!dev || !*dev)
        dev = "/dev/input/event0";
    ifd = open(dev, O_RDONLY | O_NONBLOCK);
    (void)g_xmin; (void)g_ymin;
    if (ifd < 0) {
        fprintf(stderr, "[UIBC] 打不开触摸设备 %s: %s\n", dev, strerror(errno));
        return;
    }
    {
        struct input_absinfo ai;
        if (ioctl(ifd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) {
            g_xmin = ai.minimum; g_xmax = ai.maximum;
        }
        if (ioctl(ifd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) {
            g_ymin = ai.minimum; g_ymax = ai.maximum;
        }
    }
    fprintf(stderr, "[UIBC] ✅ 触摸源就绪: %s (x %d..%d, y %d..%d)\n",
            dev, g_xmin, g_xmax, g_ymin, g_ymax);

    while (g_run && g_fd >= 0) {
        struct pollfd p[2];
        struct input_event ev;
        ssize_t r;

        p[0].fd = ifd;  p[0].events = POLLIN;  p[0].revents = 0;
        p[1].fd = ufd;  p[1].events = POLLIN;  p[1].revents = 0;
        int pr = poll(p, 2, 300);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (testin)
            testin_poll();
        if (p[1].revents & (POLLHUP | POLLERR | POLLIN)) {
            char junk[64];
            ssize_t n = recv(ufd, junk, sizeof(junk), MSG_DONTWAIT);
            /* n == 0 = 对端关连接; n < 0 + EAGAIN = 误报, 继续 */
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                break;
        }
        /* 监听兜底口的连入由外层线程 accept, 这里只盯着当前这条连接 */
        if (!(p[0].revents & POLLIN))
            continue;

        while ((r = read(ifd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (debug_events > 0 && ev.type != EV_SYN) {
                debug_events--;
                fprintf(stderr, "[UIBC] ev type=%d code=%d value=%d\n",
                        ev.type, ev.code, ev.value);
            }
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_TRACKING_ID) {
                    cur_down = (ev.value >= 0);
                } else if (ev.code == ABS_MT_POSITION_X) {
                    lx = ev.value;
                } else if (ev.code == ABS_MT_POSITION_Y) {
                    ly = ev.value;
                } else if (ev.code == ABS_X && g_xmax <= 0) {
                    lx = ev.value;
                } else if (ev.code == ABS_Y && g_ymax <= 0) {
                    ly = ev.value;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                cur_down = (ev.value != 0);
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                /* 一帧收齐 → 才决定发什么 */
                if (cur_down) {
                    int nx, ny;
                    uibc_map_xy(lx < 0 ? 0 : lx, ly < 0 ? 0 : ly, &nx, &ny);
                    if (!sent_down) {
                        if (uibc_send_touch(1, nx, ny) == 0) {
                            sent++;
                            fprintf(stderr, "[UIBC] ↓ 按下 触摸(%d,%d) → 帧像素(%d,%d)\n",
                                    lx, ly, nx, ny);
                            sent_down = 1;
                            sx = nx; sy = ny;
                            last_ms = now_ms();
                        }
                    } else if ((nx != sx || ny != sy) && now_ms() - last_ms >= 8) {
                        /* 拖动必须发 TouchMove(2), 不能重复发 TouchDown(1):
                         * 重复 DOWN 在手机侧只被当作同一点重新按下, 滑动轨迹丢失 */
                        if (uibc_send_move(nx, ny) == 0) {
                            sent++;
                            sx = nx; sy = ny;
                            last_ms = now_ms();
                        }
                    }
                } else if (sent_down) {
                    if (uibc_send_touch(0, 0, 0) == 0) {
                        sent++;
                        fprintf(stderr, "[UIBC] ↑ 抬手 (本次会话已发 %ld 个 UIBC 包)\n",
                                sent);
                    }
                    sent_down = 0;
                }
            }
        }
    }
    if (sent_down)
        uibc_send_touch(0, 0, 0);
    if (sent)
        fprintf(stderr, "[UIBC] 通道结束, 本次共发 %ld 个 UIBC 包\n", sent);
    close(ifd);
}

/* 非阻塞 connect 到指定端口, 最多等 300 ms; 失败时把 errno 交给调用者打印 */
static int connect_port(int port, int *err_out)
{
    struct sockaddr_in sa;
    int fd, one = 1;
    int err = 0;

    if (!g_peer[0] || port <= 0) {
        if (err_out) *err_out = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err_out) *err_out = errno;
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, g_peer, &sa.sin_addr) != 1) {
        if (err_out) *err_out = EINVAL;
        close(fd);
        return -1;
    }
    {
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        err = errno;
        if (err == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
            int pr = poll(&p, 1, 300);
            if (pr == 1) {
                int so = 0;
                socklen_t sl = sizeof(so);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
                if (so == 0)
                    err = 0;
                else
                    err = so;
            } else {
                err = ETIMEDOUT;
            }
        }
        if (err != 0) {
            if (err_out) *err_out = err;
            close(fd);
            return -1;
        }
    }
    {
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* 依次试候选口; 第一个连通的固定下来 (记住 g_learned, 重连优先); 全失败返回 -1。
 * tried[] 回填每个口的结果, 给调用者打日志用。 */
static int connect_once(int *err_out, int *won_port)
{
    int cand[UIBC_MAX_CAND];
    int n = build_candidates(g_peer_port, cand);
    int last = ECONNREFUSED;

    for (int i = 0; i < n; i++) {
        int err = 0;
        int fd = connect_port(cand[i], &err);
        if (fd >= 0) {
            if (cand[i] != g_peer_port && g_learned != cand[i])
                fprintf(stderr, "[UIBC] ⚠ source 通告 %d 连不上, 实际在 %d —— 改用 %d\n",
                        g_peer_port, cand[i], cand[i]);
            g_learned = cand[i];
            if (won_port) *won_port = cand[i];
            return fd;
        }
        last = err ? err : ECONNREFUSED;
    }
    if (err_out) *err_out = last;
    return -1;
}

/* 本端监听兜底 (非阻塞 accept), 返回已 listen 的 fd */
static int listen_open(void)
{
    struct sockaddr_in sa;
    int lfd, one = 1;

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        return -1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)g_port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[UIBC] bind %d 失败: %s (只做主动连接)\n", g_port, strerror(errno));
        close(lfd);
        return -1;
    }
    listen(lfd, 1);
    fprintf(stderr, "[UIBC] 同时在本端 %d 监听兜底 (source 反向连入也能用)\n", g_port);
    return lfd;
}

/* 自测钩子 (WFD_UIBC_RAW=1): 轮询 /run/uibc-test, 把文件里的十六进制字节原样发出去。
 * 行格式: 段与段之间用逗号分隔, 段内是连续十六进制 (如 "090000000000,00,010000..."),
 * 每段之间停 60 ms。用途: 手机 UIBC 服务端同时只接受一条连接 (第二条会被 RST),
 * 所以校准包格式必须借用 wfd-sink 自己这条连接 —— 这个钩子把"要发的字节"从文件读进来,
 * 改一个字节不用重编重投。生产不设这个变量, 行为与原来完全一致。 */
static void uibc_raw_test(int fd)
{
    fprintf(stderr, "[UIBC] 🧪 RAW 自测模式: 轮询 /run/uibc-test\n");
    while (g_run && g_fd >= 0) {
        char line[1024];
        FILE *fp;
        size_t n;
        if (!(fp = fopen("/run/uibc-test", "r"))) { usleep(100000); continue; }
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); usleep(100000); continue; }
        fclose(fp);
        truncate("/run/uibc-test", 0);
        n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) { usleep(50000); continue; }
        {
            char *seg = line;
            while (seg && *seg) {
                unsigned char buf[256];
                int blen = 0;
                char *comma = strchr(seg, ',');
                if (comma) *comma = 0;
                for (char *p = seg; p[0] && p[1]; p += 2) {
                    char h[3] = { p[0], p[1], 0 };
                    buf[blen++] = (unsigned char)strtoul(h, NULL, 16);
                    if (blen >= (int)sizeof(buf)) break;
                }
                if (blen > 0) {
                    int r = write_all(fd, (const char *)buf, blen);
                    fprintf(stderr, "[UIBC] 🧪 RAW 发 %d 字节 r=%d t=%lld\n", blen, r, now_ms());
                }
                seg = comma ? comma + 1 : NULL;
                if (seg) usleep(60 * 1000);
            }
        }
        usleep(20 * 1000);
    }
    fprintf(stderr, "[UIBC] 🧪 RAW 自测结束\n");
}

/* 自测钩子 (WFD_UIBC_MAPTAP=1): 轮询 /run/uibc-touch, 每行一条指令 ——
 *   "tx ty"    → 点一下 (按下 + 60 ms 后抬手)
 *   "D tx ty"  → 只按下 (拖动起点)
 *   "M tx ty"  → 拖动中
 *   "U"        → 抬手
 * tx/ty 是 **K230 屏像素** (0..479 / 0..799), 走的是和真实触摸完全相同的
 * uibc_map_xy() ⇒ 拿它验证坐标映射链路 (板子没有可用人手/无 /dev/uinput)。
 * 生产不设这个变量, 行为与原来一致。 */
static void uibc_map_test(int fd)
{
    fprintf(stderr, "[UIBC] 🧪 MAPTAP 自测模式: 轮询 /run/uibc-touch ( \"tx ty\" | \"D tx ty\" | \"M tx ty\" | \"U\" )\n");
    while (g_run && g_fd >= 0) {
        /* 4096: 一行 R 命令是多段 hex 拼接 (拖一次 12 段 ≈ 330 字符), 256 会把
         * 尾巴截掉 → 发半个包 → 手机侧 TCP 解析流错位, 之后全部包失效
         * (2026-10-28 踩过, 表现为手机的 Invalid number of inputs) */
        char line[4096];
        char op = 'T';
        int tx = 0, ty = 0, px = 0, py = 0;
        FILE *fp;
        if (!(fp = fopen("/run/uibc-touch", "r"))) { usleep(100000); continue; }
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); usleep(100000); continue; }
        fclose(fp);
        truncate("/run/uibc-touch", 0);
        if (strlen(line) >= sizeof(line) - 1)
            fprintf(stderr, "[UIBC] ⚠ 命令被截断 (超 %zu 字节)\n", sizeof(line) - 2);
        if (line[0] == 'R') {
            op = 'R';                      /* 原样十六进制 */
        } else if (line[0] == 'B') {
            op = 'B';                      /* 连发滑动, 参数在下面的专分支里解析 */
        } else if (line[0] == 'D' || line[0] == 'M' || line[0] == 'U') {
            op = line[0];
            if (op != 'U' && sscanf(line + 1, "%d %d", &tx, &ty) != 2)
                continue;
        } else if (sscanf(line, "%d %d", &tx, &ty) != 2) {
            usleep(50000);
            continue;
        }
        if (op == 'U') {
            uibc_send_touch(0, 0, 0);
            fprintf(stderr, "[UIBC] 🧪 MAPTAP 抬手\n");
            continue;
        }
        if (op == 'R') {   /* 原样发一行十六进制 (逗号分段, 段间 60 ms) —— 试包格式用 */
            char *seg = line + 1;
            while (*seg == ' ') seg++;
            while (seg && *seg) {
                unsigned char buf[256];
                int blen = 0;
                char *comma = strchr(seg, ',');
                char *p;
                if (comma) *comma = 0;
                for (p = seg; p[0] && p[1]; p += 2) {
                    char h[3] = { p[0], p[1], 0 };
                    buf[blen++] = (unsigned char)strtoul(h, NULL, 16);
                    if (blen >= (int)sizeof(buf)) break;
                }
                if (blen > 0) {
                    int rr = write_all(fd, (const char *)buf, blen);
                    fprintf(stderr, "[UIBC] 🧪 RAW 发 %d 字节 r=%d\n", blen, rr);
                }
                seg = comma ? comma + 1 : NULL;
                if (seg) usleep(60 * 1000);
            }
            continue;
        }
        if (op == 'B') {   /* 连发滑动: 一次指令生成整条拖动, 用于测延迟
                            * 格式 "B x1 y1 x2 y2 steps rate_hz" (K230 屏像素) +
                            * 可选第 7 个字段 = 拖动前的起手停顿 ms */
            int x1, y1, x2, y2, steps, rate, hold = 0, i;
            long long t0, t1;
            struct timespec ts0, ts;
            if (sscanf(line + 1, "%d %d %d %d %d %d %d",
                       &x1, &y1, &x2, &y2, &steps, &rate, &hold) < 6)
                continue;
            if (steps < 2) steps = 2;
            if (steps > 400) steps = 400;
            if (rate < 1) rate = 60;
            if (rate > 240) rate = 240;
            if (hold < 0) hold = 0;
            uibc_map_xy(x1, y1, &px, &py);
            clock_gettime(CLOCK_MONOTONIC, &ts0);
            t0 = now_ms();
            uibc_send_touch(1, px, py);
            if (hold) usleep((useconds_t)hold * 1000);
            for (i = 1; i <= steps; i++) {
                int mx = x1 + (x2 - x1) * i / steps;
                int my = y1 + (y2 - y1) * i / steps;
                long long due = (long long)i * 1000000 / rate;
                uibc_map_xy(mx, my, &px, &py);
                uibc_send_move(px, py);
                clock_gettime(CLOCK_MONOTONIC, &ts);
                {
                    long long el = ((long long)(ts.tv_sec - ts0.tv_sec)) * 1000000 +
                                   (ts.tv_nsec - ts0.tv_nsec) / 1000;
                    while (el < due) {
                        long long k = due - el;
                        usleep((useconds_t)(k > 2000 ? 2000 : k));
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        el = ((long long)(ts.tv_sec - ts0.tv_sec)) * 1000000 +
                             (ts.tv_nsec - ts0.tv_nsec) / 1000;
                    }
                }
            }
            uibc_send_touch(0, 0, 0);
            t1 = now_ms();
            fprintf(stderr, "[UIBC] 🧪 SWIPE (%d,%d)->(%d,%d) steps=%d %dHz 用时 %lld ms t0=%lld t1=%lld\n",
                    x1, y1, x2, y2, steps, rate, t1 - t0, t0, t1);
            continue;
        }
        uibc_map_xy(tx, ty, &px, &py);
        if (op == 'T') {
            /* t0 = 发第 1 个 down 包的时刻 (CLOCK_MONOTONIC ms, 与 drmplay [FRAME] t= 同源)
             * ⇒ 离线算「点击 → 屏上首帧变化」的端到端延迟, 不像滑动手势那样要先过
             *   安卓的拖动阈值, 是真正干净的一拍。 */
            long long t0 = now_ms();
            uibc_send_touch(1, px, py);
            fprintf(stderr, "[UIBC] 🧪 MAPTAP 触摸(%d,%d) → 帧像素(%d,%d) 点击 t0=%lld\n",
                    tx, ty, px, py, t0);
            usleep(60 * 1000);
            uibc_send_touch(0, 0, 0);
        } else {
            fprintf(stderr, "[UIBC] 🧪 MAPTAP %s 触摸(%d,%d) → 帧像素(%d,%d)\n",
                    op == 'D' ? "按下" : "拖动", tx, ty, px, py);
            if (op == 'D') uibc_send_touch(1, px, py);
            else            uibc_send_move(px, py);
        }
    }
    fprintf(stderr, "[UIBC] 🧪 MAPTAP 结束\n");
}

/* 通道建立: 跑输入循环, 返回后由外层重连 */
static void serve(int fd, int lfd)
{
    fprintf(stderr, "[UIBC] ✅ UIBC 通道已建立, 开始送触摸\n");
    g_fd = fd;
    g_live = 1;
    /* 自测钩子: WFD_UIBC_TAP="nx,ny" 时通道建立 2 s 后补发一次"按下+抬手" (帧像素)。
     * 用途: 板子没有 /dev/uinput, 又没人能用手按屏 ⇒ 靠它验证"包真的到了手机"。
     * 生产不用 (不设环境变量就没有任何变化)。 */
    {
        const char *tp = getenv("WFD_UIBC_TAP");
        if (tp && *tp) {
            int nx = 0, ny = 0;
            if (sscanf(tp, "%d,%d", &nx, &ny) == 2) {
                sleep(2);
                int r1 = uibc_send_touch(1, nx, ny);
                usleep(80 * 1000);
                int r2 = uibc_send_touch(0, 0, 0);
                fprintf(stderr, "[UIBC] 🧪 自测补发一次点击 (norm %d,%d) down=%d up=%d\n",
                        nx, ny, r1, r2);
            }
        }
    }
    if (getenv("WFD_UIBC_RAW") && strcmp(getenv("WFD_UIBC_RAW"), "0") != 0)
        uibc_raw_test(fd);
    else if (getenv("WFD_UIBC_MAPTAP") && strcmp(getenv("WFD_UIBC_MAPTAP"), "0") != 0)
        uibc_map_test(fd);
    else
        uibc_input_loop(fd, lfd);
    g_live = 0;
    g_fd = -1;
    fprintf(stderr, "[UIBC] 通道关闭\n");
}

static void *uibc_thread(void *arg)
{
    (void)arg;
    int lfd = -1;
    long long t0 = now_ms();
    long long last_report = 0;
    int got_in = 0;                     /* 是否已经有 source 连进来过 */
    const char *outs = getenv("WFD_UIBC_OUT");
    int allow_out = (outs && strcmp(outs, "0") == 0) ? 0 : 1;

    while (g_run) {
        int err = 0, won = 0;
        int fd = -1;

        /* WFD 1.1 §6.9: sink 在自己声明的端口上 listen, 由 **source 主动连进来**。
         * 老代码只在"外连失败"时才开监听 —— 而外连 4321 永远"成功"(那是手机的
         * media/DRM 共享 uid 的服务, 根本不是 UIBC) ⇒ 监听永远不开 ⇒ 手机连进来被拒。
         * 现在: 先 listen, 再等 accept; 外连只在 8 s 没连入时作为个别平台的兜底。 */
        if (lfd < 0) {
            lfd = listen_open();
            if (lfd < 0)
                usleep(500 * 1000);
        }
        if (allow_out && !got_in && now_ms() - t0 > 8000)
            fd = connect_once(&err, &won);
        if (fd >= 0) {
            fprintf(stderr, "[UIBC] ✅ 已连上 source 的 UIBC 端口 %s:%d\n", g_peer, won);
            serve(fd, lfd);
            close(fd);
            continue;                          /* 断了就重连 (手机切前后台会重建) */
        }
        if (allow_out && !got_in && now_ms() - last_report >= 5000) {
            last_report = now_ms();
            fprintf(stderr, "[UIBC] 连 %s 失败 (试过通告 %d / %d / %d): %s —— 继续重试\n",
                    g_peer, g_peer_port, UIBC_ALT_PORT, UIBC_DEF_PORT,
                    strerror(err ? err : ECONNREFUSED));
        }
        if (lfd >= 0) {
            struct pollfd p = { .fd = lfd, .events = POLLIN, .revents = 0 };
            int pr = poll(&p, 1, 400);
            if (pr > 0 && (p.revents & POLLIN)) {
                struct sockaddr_in ra;
                socklen_t rl = sizeof(ra);
                int cfd = accept(lfd, (struct sockaddr *)&ra, &rl);
                if (cfd >= 0) {
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    fprintf(stderr, "[UIBC] ✅ source 连入 %s:%d\n",
                            inet_ntoa(ra.sin_addr), ntohs(ra.sin_port));
                    got_in = 1;
                    serve(cfd, lfd);
                    close(cfd);
                }
            }
        } else {
            usleep(400 * 1000);
        }
    }
    if (lfd >= 0)
        close(lfd);
    g_th_done = 1;
    return NULL;
}

int uibc_start(const char *peer_ip, int peer_port, int listen_port)
{
    /* stderr 重定向到日志文件时默认是全缓冲 —— 现场排查"通道到底连没连上"时日志会
     * 滞后 4 KB 才落盘, 让人误判成"没有动作"。改成行缓冲, 一行一条即时可见。 */
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (listen_port > 0)
        g_port = listen_port;
    g_peer_port = peer_port;
    if (peer_ip)
        snprintf(g_peer, sizeof(g_peer), "%s", peer_ip);
    if (getenv("WFD_UIBC") && !strcmp(getenv("WFD_UIBC"), "0")) {
        fprintf(stderr, "[UIBC] WFD_UIBC=0, 不启用反控\n");
        return 0;
    }
    if (g_th_started) {
        if (!g_th_done)
            return 0;               /* 上一轮的线程还活着, 别重复起 */
        pthread_join(g_th, NULL);   /* 上一轮已结束: 收尸后重新起 */
        g_th_started = 0;
    }
    g_th_done = 0;
    g_run = 1;
    if (pthread_create(&g_th, NULL, uibc_thread, NULL) != 0) {
        fprintf(stderr, "[UIBC] 线程创建失败: %s\n", strerror(errno));
        g_run = 0;
        return -1;
    }
    g_th_started = 1;
    fprintf(stderr, "[UIBC] 反控启动: peer=%s 通告口=%d (候选也含 %d/%d) listen=%d\n",
            g_peer, g_peer_port, UIBC_ALT_PORT, UIBC_DEF_PORT, g_port);
    return 0;
}

void uibc_stop(void)
{
    if (!g_th_started)
        return;
    g_run = 0;
    if (g_fd >= 0) {
        shutdown(g_fd, SHUT_RDWR);
    }
    pthread_join(g_th, NULL);
    g_th_started = 0;
    g_live = 0;
}
