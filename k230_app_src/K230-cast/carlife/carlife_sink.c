/*
 * carlife_sink.c —— K230 CarLife 车机端 (有线 USB AOA) 主程序
 *
 * 链路: 手机 --USB--> AOA accessory --CarLife 私有帧--> 本进程
 *         ├─ VIDEO(2)/VIDEO_DATA  : H.264 Annex-B → /run/carlife.h264 → v4l2_dec → drmplay
 *         ├─ MEDIA(3)/MEDIA_DATA  : PCM S16LE        → /run/wfd-audio.pcm → pcmplay
 *         └─ TOUCH(6)/TOUCH_ACTION: /dev/input/event0 → 手机
 *
 * 用法: carlife_sink [--width 1280] [--height 720] [--fps 30] [--verbose]
 */
#include "carlife.h"
#include "carlife_io.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define DEFAULT_VIDEO_FIFO "/run/carlife.h264"
#define DEFAULT_PLAYER_FIFO "/run/wfd-player.fifo"
#define DEFAULT_AUDIO_FIFO "/run/wfd-audio.pcm"
#define DEFAULT_LOG "/var/log/carlife.log"
#define TOUCH_DEV "/dev/input/event0"

static int   g_log = -1;
static int   g_vfd = -1;
static int   g_afd = -1;
static int   g_verbose = 0;
static volatile int g_run = 1;
static int g_raw_dumps = 0;
static int   g_vw = 1280, g_vh = 720, g_vfps = 30;
static int   g_touch_fd = -1;
static int   g_tcp_port = 0;          /* >0 = 无线 (单 TCP 调试模式, 监听) */
static int   g_wifi = 0;              /* 1 = 无线 (手机是 server, 我们每通道连一条 TCP) */
static cl_io  *g_io = NULL;           /* 有线: AOA; 无线: Cmd 通道 */
static cl_io  *g_touch_io = NULL;     /* 无线: Touch 通道 (单独一条 TCP) */
static pthread_mutex_t g_tx = PTHREAD_MUTEX_INITIALIZER;

static void logmsg(const char *fmt, ...)
{
    char line[1024];
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03ld] ",
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(g_log, line, (size_t)n); (void)w; }
}

static int send_packet(uint16_t ch, uint32_t msg, uint32_t ts,
                       const uint8_t *pl, uint32_t pl_len)
{
    uint8_t buf[12 + 4096];
    if (pl_len > 4096) return -1;
    size_t n = cl_frame(buf, sizeof buf, ch, msg, ts, pl, pl_len);
    if (!n) return -1;

    /* 无线的每个通道是**独立的一条 TCP**, 上面只有 8 字节内层头, 没有
     * USB/AOA 的 8 字节外层头 (channel|len) —— 所以这里要跳过 buf 前 8 字节。 */
    cl_io *tio = (ch == CH_TOUCH && g_touch_io) ? g_touch_io : g_io;
    int r = -1;
    pthread_mutex_lock(&g_tx);
    if (tio && tio->wr) {
        if (g_wifi) {
            r = tio->wr(tio->ctx, buf + 8, n - 8);
        } else if (n > 8) {
            /* ★ 2026-10-30 真机实证: 有线必须**先发 8 字节外层头, 再发内层**。
             *   手机 AOAConnectManager 用 bulkTransferIn(buf, 8) 只收 8 字节头,
             *   一次发 20 字节会报 bulkTransferIn error 3 (OVERFLOW) → 250 ms 断链。 */
            r = tio->wr(tio->ctx, buf, 8);
            if (r >= 0) r = tio->wr(tio->ctx, buf + 8, n - 8);
        } else {
            r = tio->wr(tio->ctx, buf, n);
        }
    }
    pthread_mutex_unlock(&g_tx);
    if (r < 0) {
        if (g_wifi) logmsg("[tx] ch=%u msg=0x%05x 发送失败 (无线)\n", ch, msg);
        else logmsg("[tx] ch=%u msg=0x%05x 发送失败 (%s)\n", ch, msg, cl_usb_last_error());
    } else if (g_verbose) logmsg("[tx] ch=%u msg=0x%05x %u B ok\n", ch, msg, (unsigned)n);
    return r;
}

/* ---------- 视频: 起硬解管线 (与 Miracast/AirPlay 共用 v4l2_dec + drmplay) ---------- */
static void start_decoder(const char *video_fifo)
{
    /* fifo 不存在就建 (k230-screen 已独占 drmplay, 我们只喂码流) */
    unlink(video_fifo);
    if (mkfifo(video_fifo, 0666) != 0 && errno != EEXIST)
        logmsg("[vid] mkfifo %s 失败: %s\n", video_fifo, strerror(errno));

    /* 每场会话都 fork 一个 v4l2_dec; 上一场那个在 fifo 被 unlink 后自己 exit, 但没人
     * waitpid ⇒ 变僵尸。反复重连时 (手机换网/退 App) 僵尸会一场一个堆起来 (实测 55 s
     * 内 4 个), 顺手在起新解码器前回收干净。 */
    while (waitpid(-1, NULL, WNOHANG) > 0) { }

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
        execl("/usr/bin/v4l2_dec", "v4l2_dec", video_fifo,
              DEFAULT_PLAYER_FIFO, "-H", "-D", (char *)NULL);
        _exit(127);
    }
    logmsg("[vid] 解码器已起 pid=%d (输入 %s → %s)\n", (int)pid, video_fifo, DEFAULT_PLAYER_FIFO);
    /* 等解码器把管道读端打开, 否则我们的 write 会卡 */
    for (int i = 0; i < 50 && g_vfd < 0; i++) {
        g_vfd = open(video_fifo, O_WRONLY | O_NONBLOCK);
        if (g_vfd < 0) usleep(100 * 1000);
    }
    if (g_vfd < 0) logmsg("[vid] 打开 %s 失败: %s\n", video_fifo, strerror(errno));
    else {
        int fl = fcntl(g_vfd, F_GETFL, 0);
        fcntl(g_vfd, F_SETFL, fl & ~O_NONBLOCK);
    }
}

/* ---------- 触摸上行 ---------- */
/* 屏上真实几何 (drmplay 每帧写 /run/wfd-uibc-geom):
 *   ox oy dw dh = 画面在 480x800 屏上的绘制矩形
 *   cx cy cw ch = 该矩形对应的"源"坐标范围 (旋转后尺寸)
 * ★ 2026-10-30: CarLife 是 832x480 横屏源, drmplay 会顺时针转 90 度铺满 480x800
 *   (转置公式见 miracast/sink/drmplay.c rot90_cw_u8: dst(x = h-1-sy, y = sx))。
 *   触摸必须走相反方向的逆变换, 否则落点整体偏 90 度 —— 原来只会线性缩放,
 *   按"电话"会点到别处。 */
static int read_geom(double *ox, double *oy, double *dw, double *dh,
                     double *cx, double *cy, double *cw, double *ch)
{
    FILE *fp = fopen("/run/wfd-uibc-geom", "r");
    double v[8];
    int ok;
    if (!fp) return 0;
    ok = (fscanf(fp, "%lf %lf %lf %lf %lf %lf %lf %lf",
                 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8);
    fclose(fp);
    if (!ok || v[2] <= 0 || v[3] <= 0 || v[6] <= 0 || v[7] <= 0) return 0;
    *ox = v[0]; *oy = v[1]; *dw = v[2]; *dh = v[3];
    *cx = v[4]; *cy = v[5]; *cw = v[6]; *ch = v[7];
    return 1;
}

static void touch_send(int action, int sx, int sy)
{
    if (g_vw <= 0 || g_vh <= 0) return;
    /* 原始触摸 (0..479 / 0..799) → 屏像素 */
    double dx = sx, dy = sy;
    if (dx < 0) dx = 0;
    if (dx > 479) dx = 479;
    if (dy < 0) dy = 0;
    if (dy > 799) dy = 799;

    double ox, oy, dw, dh, cx, cy, cw, ch;
    int x, y;
    if (read_geom(&ox, &oy, &dw, &dh, &cx, &cy, &cw, &ch)) {
        double fx = cx + (dx - ox) * cw / dw;
        double fy = cy + (dy - oy) * ch / dh;
        if (fx < cx) fx = cx;
        if (fx > cx + cw - 1) fx = cx + cw - 1;
        if (fy < cy) fy = cy;
        if (fy > cy + ch - 1) fy = cy + ch - 1;
        if (cw == g_vh && ch == g_vw && g_vw != g_vh) {
            /* 转 90 度过: 逆变换回源坐标系 (832x480) */
            x = (int)(fy + 0.5);
            y = (int)((g_vh - 1) - fx + 0.5);
        } else {
            x = (int)(fx + 0.5);
            y = (int)(fy + 0.5);
        }
    } else {
        x = (int)(dx * g_vw / 480.0 + 0.5);
        y = (int)(dy * g_vh / 800.0 + 0.5);
    }
    if (x < 0) x = 0;
    if (x >= g_vw) x = g_vw - 1;
    if (y < 0) y = 0;
    if (y >= g_vh) y = g_vh - 1;

    uint8_t pl[32];
    size_t n = 0;
    n += cl_proto_int(pl + n, 1, (uint64_t)action);
    n += cl_proto_int(pl + n, 2, (uint64_t)x);
    n += cl_proto_int(pl + n, 3, (uint64_t)y);
    send_packet(CH_TOUCH, MSG_TOUCH_ACTION, 0, pl, (uint32_t)n);
    if (g_verbose) logmsg("[touch] action=%d 屏(%d,%d) → 视频(%d,%d)\n", action, sx, sy, x, y);
}

static void *touch_thread(void *arg)
{
    (void)arg;
    g_touch_fd = open(TOUCH_DEV, O_RDONLY);
    if (g_touch_fd < 0) { logmsg("[touch] 打不开 %s: %s\n", TOUCH_DEV, strerror(errno)); return NULL; }
    logmsg("[touch] 触摸源就绪: %s\n", TOUCH_DEV);

    int x = 0, y = 0, down = 0;
    struct input_event ev;
    while (g_run) {
        ssize_t r = read(g_touch_fd, &ev, sizeof ev);
        if (r != (ssize_t)sizeof ev) { usleep(20 * 1000); continue; }
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) x = ev.value;
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) y = ev.value;
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value) { down = 1; touch_send(0, x, y); }
            else { touch_send(1, x, y); down = 0; }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && down) {
            touch_send(2, x, y);
        }
    }
    return NULL;
}

/* ---------- 会话 ---------- */
/* ★ 2026-10-30 反编译 com.baidu.carlife 6.0.0 定死: 手机收到 HU_VERSION(0x18001) /
 *   HU_INFO(0x18003) 等报文后起一个**一次性 120 s 计时器** (util/an.java d=120000),
 *   到点弹「车机版本未经CarLife授权，即将断开连接！」并把按钮/取消都接到 n/c.Q() →
 *   connect/d.a().a(false) 主动断链。CarlifeActivity$a.handleMessage 里 msg=0x18027
 *   分支**第一句**就是 util/an.a() = 停表 (Timer.cancel + TimerTask.cancel), 之后才
 *   parseFrom(CarlifeStatisticsInfo) 打点。所以车机端回这条 0x18027 就能把弹窗掐掉。
 *   字段号取自 CarlifeStatisticsInfo.writeTo 的 write*(N, ...): 1 cuid / 2 versionName /
 *   3 versionCode / 4 channel / 5 connectCount / 6 connectSuccessCount / 7 connectTime /
 *   8 crashLog。真机实测 **channel 和 connectTime 是 required** (缺了手机打
 *   InvalidProtocolBufferException: Message missing required fields), 必须带。
 *   channel 必须是**手机 App 内置枚举里真实存在的整车厂渠道号** (core/f$a 里 62 个, 都是
 *   2\d{7})。★ 踩过的坑: 一开始填 "20000001" —— 那正是枚举里的
 *   VEHICLE_CHANNEL_PREINSTALLMARKET (= f$a.aj), 而 util/av.a(ctx, channel, e) 的判断是
 *   「channel 非空 && 匹配 2\d{7} && (== f$a.ai[20000000 AFTERMARKET] || == f$a.aj)」⇒ 弹窗。
 *   所以填这两个保留值等于自报「我是个没授权的水货车机」, 每次上报都弹。
 *   现在填 20101100 = VEHICLE_CHANNEL_HARMAN (合法后装车机厂商), 即「冒充一台授权车机」。 */
static time_t g_stat_ts;   /* 上次发 HU_STATISTICS 的时刻, 0 = 本会话还没发过 */
/* 车机渠道号 (冒充哪台授权车机)。默认 20101100 = VEHICLE_CHANNEL_HARMAN;
 * CARLIFE_HU_CHANNEL 可覆盖, 方便现场换一台"车"做 A/B。 */
static const char *g_hu_channel = "20101100";

static int stat_disabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("CARLIFE_NO_STAT"); v = (e && *e && *e != '0'); }
    return v;
}

static void init_hu_channel(void)
{
    const char *e = getenv("CARLIFE_HU_CHANNEL");
    if (e && strlen(e) == 8) g_hu_channel = e;
}

static void send_hu_statistics(void)
{
    if (stat_disabled()) return;   /* 现场 A/B 开关: 排查「统计上报是否影响出画」 */
    uint8_t pl[256]; size_t n = 0;
    n += cl_proto_str(pl + n, 1, "K230HU0001");
    n += cl_proto_str(pl + n, 2, "1.0.0");
    n += cl_proto_int(pl + n, 3, 1);
    n += cl_proto_str(pl + n, 4, g_hu_channel);  /* required; 必须是真实整车厂渠道号 */
    n += cl_proto_int(pl + n, 5, 1);
    n += cl_proto_int(pl + n, 6, 1);
    n += cl_proto_int(pl + n, 7, 0);            /* required: 本次连接时长 (s) */
    if (send_packet(CH_CMD, MSG_HU_STATISTICS, 0, pl, (uint32_t)n) >= 0) {
        g_stat_ts = time(NULL);
        logmsg("[sess] 已发 HU_STATISTICS (0x18027) → 手机端 120 s 授权计时器应停\n");
    }
}

static void handle_packet(cl_packet *p)
{
    switch (p->message) {
    case MSG_VERSION_MATCH: {
        uint64_t v = 0;
        if (cl_proto_get(p->payload, p->payload_len, 1, &v, NULL, NULL) != 0 || v != 1) {
            logmsg("[sess] 手机拒绝版本 (f1=%llu)\n", (unsigned long long)v);
            return;
        }
        uint8_t pl[512]; size_t n = 0;
        n += cl_proto_str(pl + n, 1, "K230");
        n += cl_proto_str(pl + n, 4, "K230 CarLife Sink");
        n += cl_proto_str(pl + n, 14, "K230 HU");
        n += cl_proto_str(pl + n, 19, "1");
        send_packet(CH_CMD, MSG_HU_INFO, 0, pl, (uint32_t)n);

        n = 0;
        n += cl_proto_int(pl + n, 1, (uint64_t)g_vw);
        n += cl_proto_int(pl + n, 2, (uint64_t)g_vh);
        n += cl_proto_int(pl + n, 3, (uint64_t)g_vfps);
        send_packet(CH_CMD, MSG_VIDEO_INIT, 0, pl, (uint32_t)n);
        logmsg("[sess] 版本匹配 → 已发 HU_INFO + VIDEO_INIT (%dx%d@%d)\n", g_vw, g_vh, g_vfps);
        send_hu_statistics();   /* 赶在 120 s 计时器到点前把授权弹窗掐掉 */
        break;
    }
    case MSG_MD_INFO: {
        logmsg("[sess] 收到 MD_INFO (%u B)\n", p->payload_len);
        break;
    }
    case MSG_VIDEO_INIT_DONE: {
        uint64_t w = 0, h = 0, f = 0;
        if (cl_proto_get(p->payload, p->payload_len, 1, &w, NULL, NULL) == 0 &&
            cl_proto_get(p->payload, p->payload_len, 2, &h, NULL, NULL) == 0 &&
            cl_proto_get(p->payload, p->payload_len, 3, &f, NULL, NULL) == 0) {
            g_vw = (int)w; g_vh = (int)h; g_vfps = (int)f;
        }
        logmsg("[sess] VIDEO_INIT_DONE: %dx%d@%d → 发 VIDEO_START\n", g_vw, g_vh, g_vfps);
        send_packet(CH_CMD, MSG_VIDEO_START, 0, NULL, 0);
        break;
    }
    case MSG_MEDIA_INIT: {
        uint64_t rate = 0, ch = 0, bits = 0;
        cl_proto_get(p->payload, p->payload_len, 1, &rate, NULL, NULL);
        cl_proto_get(p->payload, p->payload_len, 2, &ch, NULL, NULL);
        cl_proto_get(p->payload, p->payload_len, 3, &bits, NULL, NULL);
        logmsg("[aud] MEDIA_INIT: %llu Hz / %llu ch / %llu bit\n",
               (unsigned long long)rate, (unsigned long long)ch, (unsigned long long)bits);
        if (g_afd < 0) g_afd = open(DEFAULT_AUDIO_FIFO, O_WRONLY | O_NONBLOCK);
        break;
    }
    case MSG_MEDIA_DATA: {
        if (g_afd < 0) g_afd = open(DEFAULT_AUDIO_FIFO, O_WRONLY | O_NONBLOCK);
        if (g_afd >= 0 && p->payload_len) {
            ssize_t w = write(g_afd, p->payload, p->payload_len);
            (void)w;
        }
        break;
    }
    case MSG_MEDIA_STOP:
        logmsg("[aud] MEDIA_STOP\n");
        break;
    case MSG_VIDEO_DATA: {
        if (g_vfd >= 0 && p->payload_len) {
            ssize_t w = write(g_vfd, p->payload, p->payload_len);
            if (w < 0) logmsg("[vid] 写解码管道失败: %s\n", strerror(errno));
        }
        break;
    }
    case MSG_VIDEO_HEARTBEAT:
        break;
    default:
        if (p->message == 0x18048 || p->message == 0x10049 || p->message == 0x1804A ||
            p->message == 0x1004B || p->message == 0x1806B || p->message == 0x18083) {
            logmsg("[sess] ⚠ 手机要求 OEM 鉴权 (msg=0x%05x) —— 本实现无鉴权, 会话可能被拒\n", p->message);
        } else if (g_verbose) {
            logmsg("[rx] ch=%u msg=0x%05x len=%u\n", p->channel, p->message, p->payload_len);
        }
        break;
    }
}

static void run_session(cl_io *io)
{
    g_io = io;
    g_stat_ts = 0;                  /* 新会话: 统计上报从头算 */
    static cl_parser parser;
    cl_parser_reset(&parser);

    /* 2026-11-06: 会话期间置 /run/carlife-active, drmplay 认这个标记后无限保持
     * 最后一帧 (与 AirPlay 同策略)。原因: CarLife 是**屏幕镜像** —— 手机画面静止时
     * App 完全不产帧 (实测编码器 Stats 50 s: encode:0), 不保持就会静默 2.4 s 刷黑
     * (主人现场「投着投着突然黑屏」)。服务停止时由 unit 的 ExecStopPost 兜底清掉。 */
    int _mk = open("/run/carlife-active", O_CREAT | O_WRONLY, 0644);
    if (_mk >= 0) close(_mk);

    uint8_t pl[64]; size_t n = 0;
    n += cl_proto_int(pl + n, 1, 1);      /* 协议主版本 */
    n += cl_proto_int(pl + n, 2, 0);
    /* ★ 2026-10-30: AOA 切完手机要重新枚举 + 系统拉起 CarLife App 打开 accessory fd,
     *   这中间有几百毫秒到几秒没读者, 写 bulk OUT 会 TIMEOUT。原来只发一次, 一旦撞上
     *   这段窗口就永远等不到回包。改成重发直到写成功 (最多 20 s)。 */
    int sent_ok = 0;
    for (int i = 0; i < 40; i++) {
        if (send_packet(CH_CMD, MSG_HU_VERSION, 0, pl, (uint32_t)n) >= 0) {
            if (i) logmsg("[sess] HU_VERSION 第 %d 次写入成功 (手机 App 已接管 USB)\n", i + 1);
            sent_ok = 1;
            break;
        }
        if (i == 0) logmsg("[sess] 手机还没读 (App 未接管 accessory), 每 500 ms 重试…\n");
        if (!strcmp(cl_usb_last_error(), "LIBUSB_ERROR_NO_DEVICE")) {
            logmsg("[sess] USB 已断开 (拔线), 退出本次会话\n");
            break;
        }
        usleep(500 * 1000);
    }
    if (!sent_ok) {
        logmsg("[sess] ⚠ HU_VERSION 20 s 内始终写不进 ⇒ 手机还粘在 accessory 模式但没有进程读它。\n");
        logmsg("[sess]    App 已退出或被系统杀掉时, 只能拔插一次 USB 才能恢复 (AOA 没有退出命令)\n");
    }
    logmsg("[sess] 已发 HU_VERSION\n");

    uint8_t buf[16384];
    while (g_run) {
        /* 手机端 0x1800F / 0x18010 / 0x1801D~0x18025 这些报文会**重启** 120 s 计时器,
         * 所以不能只发一次: 每 50 s 补一条 0x18027 把表按停, 计时器永远走不到 120 s。 */
        if (g_stat_ts && time(NULL) - g_stat_ts >= 50) send_hu_statistics();
        int r = io->rd(io->ctx, buf, sizeof buf, 1000);
        if (r < 0) { logmsg(g_wifi ? "[wifi] Cmd 通道读失败 (手机换网/退 App/keepalive 探活失败), 结束会话\n"
                             : "[usb] 读失败, 断链\n"); break; }
        if (r == 0) continue;
        if (g_verbose && g_raw_dumps < 30) {   /* 头 30 包打原始十六进制, 用来对协议 */
            char hex[3 * 48 + 4]; int hn = 0;
            int lim = r < 48 ? r : 48;
            for (int i = 0; i < lim; i++) hn += snprintf(hex + hn, sizeof hex - hn, "%02x ", buf[i]);
            logmsg("[raw] rx %d B: %s\n", r, hex);
            g_raw_dumps++;
        }
        cl_packet pkt;
        int pr = g_wifi ? cl_inner_parser_feed(&parser, buf, (size_t)r, &pkt)
                        : cl_parser_feed(&parser, buf, (size_t)r, &pkt);
        if (pr < 0) { logmsg("[proto] 帧解析错误, 丢弃缓冲\n"); cl_parser_reset(&parser); continue; }
        if (pr > 0) handle_packet(&pkt);
    }
    unlink("/run/carlife-active");
    g_io = NULL;
}

/* ---------- 无线: Video / Media 通道各一条 TCP, 只收裸字节 ---------- */
static void *video_reader(void *arg)
{
    cl_io *io = arg;
    uint8_t buf[16384];
    long total = 0;
    while (g_run) {
        int r = io->rd(io->ctx, buf, sizeof buf, 1000);
        if (r < 0) { logmsg("[wifi] Video 通道断链 (共 %ld B)\n", total); break; }
        if (r == 0) continue;
        total += r;
        if (g_vfd >= 0) {
            ssize_t w = write(g_vfd, buf, (size_t)r);
            (void)w;
        }
    }
    return NULL;
}

static void *media_reader(void *arg)
{
    cl_io *io = arg;
    uint8_t buf[8192];
    int logged = 0;
    while (g_run) {
        int r = io->rd(io->ctx, buf, sizeof buf, 1000);
        if (r < 0) break;
        if (r == 0) continue;
        if (!logged) { logmsg("[wifi] Media 通道开始有数据 (%d B)\n", r); logged = 1; }
        if (g_afd < 0) g_afd = open(DEFAULT_AUDIO_FIFO, O_WRONLY | O_NONBLOCK);
        if (g_afd >= 0) { ssize_t w = write(g_afd, buf, (size_t)r); (void)w; }
    }
    return NULL;
}

/* 端口表来自手机端 ConnectManager (com.baidu.carlife.core.f jB..jH):
 *   7240 Cmd / 8240 Video / 9240 Media / 9241 TTS / 9242 VR / 9340 Touch / 9440 Data
 * 手机侧把除 Data 外的 6 条都连上才算"已连接", 所以必须全连。 */
static const struct { const char *name; int port; } WIFI_CH[] = {
    { "Cmd",   7240 }, { "Video", 8240 }, { "Media", 9240 }, { "TTS", 9241 },
    { "VR",    9242 }, { "Touch", 9340 }, { "Data",  9440 },
};

static int run_wifi_session(const char *ip, const char *video_fifo)
{
    enum { NCH = (int)(sizeof WIFI_CH / sizeof WIFI_CH[0]) };
    int fd[NCH];
    int ok6 = 1;

    for (int i = 0; i < NCH; i++) {
        fd[i] = cl_wifi_connect(ip, WIFI_CH[i].port, 3, g_log);
        int critical = strcmp(WIFI_CH[i].name, "Data") != 0;
        if (fd[i] < 0 && critical) ok6 = 0;
        if (fd[i] >= 0) logmsg("[wifi] %s 通道 %d 已连\n", WIFI_CH[i].name, WIFI_CH[i].port);
    }
    if (!ok6) {
        logmsg("[wifi] ⚠ 关键通道没全连上, 断开重来\n");
        for (int i = 0; i < NCH; i++) if (fd[i] >= 0) close(fd[i]);
        return -1;
    }

    char peer[64];
    snprintf(peer, sizeof peer, "%s:7240", ip);
    cl_io *cmd = cl_tcp_wrap_fd(fd[0], peer);
    cl_io *video = cl_tcp_wrap_fd(fd[1], "video");
    cl_io *media = cl_tcp_wrap_fd(fd[2], "media");
    cl_io *touch = cl_tcp_wrap_fd(fd[5], "touch");
    /* TTS/VR/Data 只建链不读 (手机侧只看连接数), 保留 fd 直到会话结束 */

    g_wifi = 1;
    g_io = cmd;
    g_touch_io = touch;
    start_decoder(video_fifo);

    pthread_t tv = 0, tm = 0;
    if (video) pthread_create(&tv, NULL, video_reader, video);
    if (media) pthread_create(&tm, NULL, media_reader, media);

    run_session(cmd);

    g_run = 0;                 /* 让 reader 线程退出 */
    if (tv) pthread_join(tv, NULL);
    if (tm) pthread_join(tm, NULL);
    g_run = 1;

    cl_tcp_close(cmd); cl_tcp_close(video); cl_tcp_close(media); cl_tcp_close(touch);
    for (int i = 3; i <= 6; i++) if (i != 5 && fd[i] >= 0) close(fd[i]);
    g_io = NULL; g_touch_io = NULL; g_wifi = 0;
    if (g_vfd >= 0) { close(g_vfd); g_vfd = -1; }
    logmsg("[wifi] 无线会话结束\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *video_fifo = DEFAULT_VIDEO_FIFO;
    const char *logpath = DEFAULT_LOG;
    init_hu_channel();   /* 先取环境变量, 下面的 --channel 可覆盖它 */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) g_vw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) g_vh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_vfps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(argv[i], "--tcp") && i + 1 < argc) g_tcp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wifi")) g_wifi = -1;   /* 稍后归一成 1 */
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else if (!strcmp(argv[i], "--channel") && i + 1 < argc) g_hu_channel = argv[++i];
    }
    g_log = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_log < 0) g_log = 2;

    signal(SIGPIPE, SIG_IGN);
    logmsg("=== carlife_sink 启动 (请求 %dx%d@%d, 车机渠道号 %s) ===\n",
           g_vw, g_vh, g_vfps, g_hu_channel);

    if (libusb_init(NULL) != 0) { logmsg("libusb_init 失败\n"); return 1; }

    pthread_t th;
    pthread_create(&th, NULL, touch_thread, NULL);

    int backoff = 2;

    if (g_wifi != 0) {
        /* -------- 无线: 听 UDP 7999 "CarlifeHost" → 连手机 7 条通道 -------- */
        g_wifi = 0;
        logmsg("=== 无线模式: 等手机广播 CarlifeHost (UDP 7999) ===\n");
        while (g_run) {
            char ip[64];
            if (cl_wifi_wait_hello(ip, sizeof ip, 86400, g_log) != 0) continue;
            logmsg("[wifi] 手机 %s, 开始建通道\n", ip);
            if (run_wifi_session(ip, video_fifo) != 0) sleep(3);
        }
    } else if (g_tcp_port > 0) {
        /* ---------------- 无线: WiFi 下的一条 TCP ---------------- */
        logmsg("=== 无线模式: 监听 TCP :%d (手机连 K230 的 AP 后接入) ===\n", g_tcp_port);
        while (g_run) {
            cl_io *io = cl_tcp_listen_accept(g_tcp_port, g_log, 86400);
            if (!io) { sleep(2); continue; }
            start_decoder(video_fifo);
            run_session(io);
            cl_tcp_close(io);
            if (g_vfd >= 0) { close(g_vfd); g_vfd = -1; }
            logmsg("[wifi] 连接结束, 重新监听 :%d\n", g_tcp_port);
        }
    } else {
        /* ---------------- 有线: USB AOA ---------------- */
        while (g_run) {
            logmsg("[usb] 等待安卓手机 (AOA)…\n");
            /* 手机可能已经在 accessory 模式 (上一次会话没退), 先直接找 */
            cl_usb *u = cl_usb_wait_accessory(800, g_log);
            if (!u && cl_usb_open_and_switch(g_log) == 0)
                u = cl_usb_wait_accessory(15000, g_log);
            if (u) {
                cl_io *io = cl_usb_as_io(u);
                start_decoder(video_fifo);
                if (io) run_session(io);
                cl_io_free(io);
                cl_usb_close(u);
                if (g_vfd >= 0) { close(g_vfd); g_vfd = -1; }
                logmsg("[sess] 会话结束, %d s 后重试\n", backoff);
            }
            sleep((unsigned)backoff);
        }
    }
    libusb_exit(NULL);
    return 0;
}
