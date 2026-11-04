/* usb_mirror.c —— 安卓 USB 有线投屏 + 反控 (走 adb, 免 root)
 *
 * 链路:
 *   手机 --USB--> K230 USB-A (host)  == adb ==  exec:screenrecord --output-format=h264 -
 *        --> stdout (裸 H.264 Annex-B) --> v4l2_dec (VPU 硬解) --> /run/wfd-player.fifo --> drmplay
 *   反控: 板上触摸 --> adb --> 手机 /dev/input/eventN (原始 evdev 注入, 见 touch.c)
 *
 * 用法:
 *   usb_mirror [--touch-test] [--bit-rate 8000000] [--size 720x1280] [--panel 480x800] [--touch] [--once] [-v]
 * 输出: 裸 H.264 到 stdout, 日志到 stderr。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "adb.h"
#include "touch.h"

#define CH_VIDEO 1
#define CH_TOUCH 2
#define CH_PROBE 3

static adb_dev g_dev;
static volatile int g_stop;

/* 会话标记: 只有"真的在吐 H.264"时才存在。
 * k230-source-arbiter 靠它判断"有线投屏正在投", 从而把无线那几路停掉
 * (否则两个写端同时往 /run/wfd-player.fifo 灌, 屏就花)。 */
#define ACTIVE_MARK "/run/usbmirror-active"
static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
    unlink(ACTIVE_MARK);
    _exit(0);
}

static void mark_active(int on)
{
    if (on) {
        int fd = open(ACTIVE_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { if (write(fd, "1\n", 2) < 0) { /* 磁盘满也无所谓 */ } close(fd); }
    } else {
        unlink(ACTIVE_MARK);
    }
}

#define LOG(...) do { fprintf(stderr, "[mirror] " __VA_ARGS__); fflush(stderr); } while (0)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int unix_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

static int write_all(int fd, const void *buf, int len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n; len -= (int)n;
    }
    return 0;
}

/* 跑一条 shell 服务并把输出收进 buf (整段收完或超时), 返回收到字节数 */
static int run_shell(const char *service, char *buf, int cap, int timeout_ms)
{
    uint32_t remote = 0;
    int got = 0;
    buf[0] = 0;
    if (adb_open_channel(&g_dev, CH_PROBE, service, &remote, 8000) != 0) return 0;
    long t0 = now_ms();
    while (now_ms() - t0 < timeout_ms) {
        uint32_t cmd, a0, a1; uint8_t *data; int len;
        if (adb_recv_packet(&g_dev, 800, &cmd, &a0, &a1, &data, &len) < 0) break;
        if (cmd == ADB_CMD_WRTE && a1 == CH_PROBE) {
            int c = (got + len < cap - 1) ? len : cap - 1 - got;
            if (c > 0) { memcpy(buf + got, data, c); got += c; buf[got] = 0; }
            adb_send_okay(&g_dev, a1, a0);
        } else if (cmd == ADB_CMD_CLSE && a1 == CH_PROBE) {
            adb_free_packet(data);
            break;
        }
        adb_free_packet(data);
    }
    adb_send_close(&g_dev, CH_PROBE, remote);
    return got;
}

struct probe {
    int sdk;
    int phys_w, phys_h;
    int struct_size;     /* input_event 结构大小 */
};

/* 开视频通道之前做 (独占收包, 不会截断视频流) */
static void probe_device(struct probe *p, int verbose)
{
    char buf[1024];
    p->sdk = 0; p->phys_w = p->phys_h = 0; p->struct_size = 24;

    if (run_shell("shell:getprop ro.build.version.sdk; wm size; getprop ro.product.cpu.abi",
                  buf, sizeof buf, 6000) > 0) {
        p->sdk = atoi(buf);
        char *q = strstr(buf, "size:");
        if (q) { if (sscanf(q + 5, " %dx%d", &p->phys_w, &p->phys_h) != 2) p->phys_w = p->phys_h = 0; }
        if (strstr(buf, "arm64") || strstr(buf, "x86_64")) p->struct_size = 24;
        else if (strstr(buf, "armeabi") || strstr(buf, "x86")) p->struct_size = 16;
    }
    if (verbose)
        LOG("手机: SDK=%d 物理=%dx%d input_event=%d B\n", p->sdk, p->phys_w, p->phys_h, p->struct_size);
}

/* 找手机的**主触摸屏**: 有的机器不止一个 MT 设备 (实测这台: event1 =
 * nubia_synaptics_dsx 主屏 1080x1920, event2 = synaptics_btn 下面按钮条 1101x1901)。
 * 判据: ABS_MT_POSITION_X/Y 的 max+1 最接近「屏幕宽/高」的那个 —— 精确到 1 像素的
 * 只有主屏, 按钮条永远差几十。允许 X/Y 互换 (横屏驱动会把轴调过来)。 */
struct mt_dev { char path[48]; int abs_x, abs_y; };

static void detect_evdev(char *out, int outlen, int *abs_x, int *abs_y, int sw, int sh)
{
    static char buf[16384];
    out[0] = 0; *abs_x = 0; *abs_y = 0;
    if (run_shell("exec:getevent -pl", buf, sizeof buf, 5000) <= 0) return;

    struct mt_dev devs[16];
    int nd = 0;
    int cur = -1;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *q = strstr(line, "/dev/input/event");
        if (q && strstr(line, "add device")) {
            cur = -1;
            if (nd < (int)(sizeof devs / sizeof devs[0])) {
                int k = 0;
                while (q[k] && q[k] != ' ' && k < (int)sizeof devs[0].path - 1) { devs[nd].path[k] = q[k]; k++; }
                devs[nd].path[k] = 0;
                devs[nd].abs_x = devs[nd].abs_y = 0;
                cur = nd++;
            }
            continue;
        }
        if (cur < 0) continue;
        char *px = strstr(line, "ABS_MT_POSITION_X");
        if (px) {
            int mx = 0;
            if (sscanf(px, "ABS_MT_POSITION_X : value %*d, min %*d, max %d", &mx) == 1) devs[cur].abs_x = mx + 1;
            continue;
        }
        char *py = strstr(line, "ABS_MT_POSITION_Y");
        if (py) {
            int my = 0;
            if (sscanf(py, "ABS_MT_POSITION_Y : value %*d, min %*d, max %d", &my) == 1) devs[cur].abs_y = my + 1;
        }
    }

    long best_cost = -1;
    int bi = -1;
    for (int i = 0; i < nd; i++) {
        if (devs[i].abs_x <= 0 || devs[i].abs_y <= 0) continue;
        long c1 = labs((long)devs[i].abs_x - sw) + labs((long)devs[i].abs_y - sh);
        long c2 = labs((long)devs[i].abs_x - sh) + labs((long)devs[i].abs_y - sw);
        long cost = c1 < c2 ? c1 : c2;
        if (best_cost < 0 || cost < best_cost) { best_cost = cost; bi = i; }
    }
    if (bi < 0) return;
    snprintf(out, outlen, "%s", devs[bi].path);
    *abs_x = devs[bi].abs_x;
    *abs_y = devs[bi].abs_y;
}

struct opts {
    int bitrate;
    int video_w, video_h;      /* 送进屏幕的码流尺寸 */
    int panel_w, panel_h;
    int touch, once, verbose, size_given;
    int touch_test;            /* 自测: 注入一段手势后退出 */
    int hold_sec;              /* 连上后先干等 N 秒 (排查授权是否要等手机写完 adb_keys) */
    int touch_selftest;
    int motion;          /* --motion N: 自己不停在手机上划, 用来压延迟/帧率 (测速用) */
    int motion_sec;        /* 自测: 同一会话里注入手势 -> dumpsys input 取证, 只弹一次授权 */
    const char *dev;
    const char *test_unix;
};

static int open_video(adb_dev *d, uint32_t *remote, const struct opts *o)
{
    char svc[256];
    /* ★ 必须显式 --size: 手机原生 1080x1920, 不缩放一帧 NV12 要 3.1 MB,
     * 板上管道和缩放转换都吃不下 (实测掉到 9 fps)。屏只有 480x800, 720x1280 足够。 */
    if (o->video_w > 0 && o->video_h > 0)
        snprintf(svc, sizeof svc,
                 "exec:screenrecord --output-format=h264 --bit-rate %d --size %dx%d -",
                 o->bitrate, o->video_w, o->video_h);
    else
        snprintf(svc, sizeof svc,
                 "exec:screenrecord --output-format=h264 --bit-rate %d -", o->bitrate);
    return adb_open_channel(d, CH_VIDEO, svc, remote, 10000);
}

/* --motion: 自己在手机上不停划, 让画面持续变化。
 * 为什么需要它: 静止画面下手机编码器只给几帧/秒, 量不出「延迟」和「吞吐上限」;
 * 有持续动作才能看到源端到底能出多少帧、管道有没有积压。 */
static void *motion_thread(void *arg)
{
    struct opts *o = arg;
    int t = 0;
    while (!g_stop && t < o->motion_sec) {
        /* 上下划 (拉列表) —— 每屏一大片重绘, 最能压出带宽和转换开销 */
        touch_gesture(o->panel_w / 2, o->panel_h * 3 / 4,
                      o->panel_w / 2, o->panel_h / 4, 300);
        t++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    struct opts o;
    memset(&o, 0, sizeof o);
    o.bitrate = 8000000;
    o.video_w = 0; o.video_h = 0;      /* 0 = 按屏尺寸自动算 (见下方「推算视频尺寸」) */
    o.panel_w = 480; o.panel_h = 800;
    o.dev = "/dev/input/event0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bit-rate") && i + 1 < argc) o.bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &o.video_w, &o.video_h); o.size_given = 1; }
        else if (!strcmp(argv[i], "--panel") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &o.panel_w, &o.panel_h);
        else if (!strcmp(argv[i], "--touch")) o.touch = 1;
        else if (!strcmp(argv[i], "--touch-test")) { o.touch = 1; o.touch_test = 1; }
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) o.hold_sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--touch-selftest")) { o.touch = 1; o.touch_selftest = 1; }
        else if (!strcmp(argv[i], "--motion")) { o.touch = 1; o.motion = 1; o.motion_sec = (i + 1 < argc) ? atoi(argv[++i]) : 60; }
        else if (!strcmp(argv[i], "--once")) o.once = 1;
        else if (!strcmp(argv[i], "-v")) o.verbose = 1;
        else if (!strcmp(argv[i], "--test-unix") && i + 1 < argc) o.test_unix = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("用法: %s [--bit-rate 8000000] [--size 720x1280] [--panel 480x800] [--touch] [--once] [-v]\n"
                   "  输出裸 H.264 到 stdout; 典型用法: usb_mirror --touch | v4l2_dec /dev/stdin /run/wfd-player.fifo -H -D\n", argv[0]);
            return 0;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    /* systemctl stop 发 SIGTERM: 顺手把会话标记删掉, 否则仲裁器会一直以为
     * "有线还在投", 把无线那几路压着不放 (标记文件在 /run 里, 断电会没, 但正常停要清)。 */
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    int rc;
    /* ★ 2026-11-03: 手机重枚举 (USB disconnect → 新 device number) 时, 原来直接
     * goto out 退出进程 ⇒ run.sh 再起一个新进程 ⇒ **新的 adb 会话** ⇒ 这台 ROM
     * 又要发一次公钥 (授权弹窗)。改成同一进程里重连: 进程不死, 授权状态还能复用。 */
    int touch_want = o.touch;
    int first_connect = 1;
    long bytes = 0, t_bytes = 0, t0 = 0;
    int sessions = 0, reconnects = 0;
    int want_reconnect = 0;

reconnect:
    o.touch = touch_want;
    for (int attempt = 0; attempt < (first_connect ? 1 : 30); attempt++) {
        if (attempt) sleep(2);
        if (o.test_unix) {
            int fd = unix_connect(o.test_unix);
            if (fd < 0) { LOG("✗ 连不上 %s: %s\n", o.test_unix, strerror(errno)); return 10; }
            LOG("(自测模式: %s 假冒 adbd)\n", o.test_unix);
            rc = adb_connect_test(&g_dev, fd, 60000, o.verbose);
        } else {
            rc = adb_connect(&g_dev, 120000, o.verbose);
        }
        if (rc == 0) break;
        if (first_connect) break;
        LOG("… 重连失败 (%s), 2 s 后再试\n", adb_strerror(rc));
    }
    if (rc != 0) { LOG("✗ 连不上: %s\n", adb_strerror(rc)); return 10; }
    first_connect = 0;
    t0 = now_ms(); t_bytes = bytes;
    LOG("✓ adb 已连上 %04x:%04x\n", g_dev.vid, g_dev.pid);

    if (o.hold_sec > 0) { LOG("保持连接 %d s (等手机侧把授权写盘)…\n", o.hold_sec); sleep(o.hold_sec); adb_disconnect(&g_dev); return 0; }

    struct probe pr;
    probe_device(&pr, 1);

    if (!o.test_unix && !o.size_given && pr.phys_w > 0 && pr.phys_h > 0) {
        /* ★ 2026-11-03: 原来按「长边压到 1280」算 ⇒ 720x1280。实测这条链路的瓶颈
         * 不是解码/传输, 是 drmplay 的**软件 NV12→ARGB + 重采样**:
         * 720x1280 → 绘制区 450x800 时「转换」29.8 ms/帧 (整圈 42.7 ms), 只够 23 fps;
         * 手机一有动作源端就 >23 fps, 帧在管道里越积越多 ⇒ 延迟一路涨。
         * 改成按**屏的尺寸**算: 码流 = 绘制区尺寸 ⇒ drmplay 走 NV12 1:1 直贴快路径
         * (无重采样, 实测 ~4 ms/帧), 同时像素只有原来 39%, 手机编码/USB/解码全跟着省。
         * 16 对齐: screenrecord 要求宽高是 16 的倍数 (真机验证 448/800 可编)。 */
        double s = (double)o.panel_h / (double)pr.phys_h;
        if (pr.phys_w * s > o.panel_w) s = (double)o.panel_w / (double)pr.phys_w;
        o.video_w = ((int)(pr.phys_w * s)) & ~15;
        o.video_h = ((int)(pr.phys_h * s)) & ~15;
        if (o.video_w < 16) o.video_w = 16;
        if (o.video_h < 16) o.video_h = 16;
        LOG("按屏幕尺寸推算视频尺寸 %dx%d (手机物理 %dx%d, 屏 %dx%d) —— 让 drmplay 走 1:1\n",
            o.video_w, o.video_h, pr.phys_w, pr.phys_h, o.panel_w, o.panel_h);
    }

    if (o.touch) {
        char evdev[64] = { 0 };
        int abs_x = 0, abs_y = 0;
        uint32_t remote = 0;
        if (!o.test_unix) {
            /* 刚跑完别的东西 (比如 screencap) 时 adbd 还没清完, 第一把常常开不出通道,
             * 重试一次就稳 (实测: 不重试约 1/3 概率退回 fallback)。 */
            for (int attempt = 0; attempt < 2 && !evdev[0]; attempt++) {
                if (attempt) usleep(400000);
                detect_evdev(evdev, sizeof evdev, &abs_x, &abs_y,
                             pr.phys_w > 0 ? pr.phys_w : o.video_w,
                             pr.phys_h > 0 ? pr.phys_h : o.video_h);
            }
            if (o.verbose) LOG("手机触摸屏: %s (量程 %dx%d)\n", evdev[0] ? evdev : "(未识别)", abs_x, abs_y);
        }
        int ok = 0;
        if (evdev[0]) {
            char svc[128];
            snprintf(svc, sizeof svc, "exec:tee %s", evdev);
            ok = (adb_open_channel(&g_dev, CH_TOUCH, svc, &remote, 6000) == 0);
            if (!ok) LOG("⚠ 开 %s 失败, 试 input 命令兜底\n", svc);
        }
        if (!ok) {
            evdev[0] = 0;
            ok = (adb_open_channel(&g_dev, CH_TOUCH, "shell:", &remote, 6000) == 0);
        }
        if (ok) {
            struct touch_cfg tc;
            memset(&tc, 0, sizeof tc);
            tc.dev = &g_dev;
            tc.chan = CH_TOUCH;
            tc.evdev = evdev[0] ? evdev : NULL;
            tc.struct_size = pr.struct_size;
            tc.phys_w = abs_x > 0 ? abs_x : (pr.phys_w > 0 ? pr.phys_w : o.video_w);
            tc.phys_h = abs_y > 0 ? abs_y : (pr.phys_h > 0 ? pr.phys_h : o.video_h);
            tc.panel_w = o.panel_w;
            tc.panel_h = o.panel_h;
            tc.board_evdev = o.dev;
            if (touch_start(&tc, remote) != 0) { o.touch = 0; LOG("⚠ 反控没起来, 本次只投屏\n"); }
            else if (o.motion) {
                pthread_t mt;
                if (pthread_create(&mt, NULL, motion_thread, &o) == 0) {
                    pthread_detach(mt);
                    LOG("测速: 自己在手机上连划 %d 段 (每段 300 ms), 让画面持续变化\n", o.motion_sec);
                }
            }
        } else {
            o.touch = 0;
            LOG("⚠ 触摸通道打不开, 本次只投屏\n");
        }
    }

    if (o.touch_test) {
        usleep(300000);
        /* 从屏幕中上往下滑一段 (会下拉通知栏/切页), 足够看出画面变化 */
        touch_gesture(o.panel_w / 2, o.panel_h / 4,
                      o.panel_w / 2, o.panel_h * 3 / 4, 400);
        usleep(500000);
        touch_stop();
        adb_disconnect(&g_dev);
        return 0;
    }

    if (o.touch_selftest) {
        static char dbuf[24576];
        usleep(300000);
        /* 取一次注入前的 RecentQueue, 注入, 再取一次 —— 同一个 adb 会话, 只弹一次授权 */
        int n1 = run_shell("exec:dumpsys input", dbuf, sizeof dbuf, 9000);
        LOG("注入前 dumpsys: %d B\n", n1);
        usleep(400000);
        touch_gesture(o.panel_w / 2, o.panel_h / 4, o.panel_w / 2, o.panel_h * 3 / 4, 400);
        usleep(600000);
        int n2 = run_shell("exec:dumpsys input", dbuf, sizeof dbuf, 9000);
        LOG("注入后 dumpsys: %d B\n", n2);
        char *q = strstr(dbuf, "RecentQueue");
        if (q) {
            char *nl = strchr(q, '\n');
            printf("%s%.900s\n", q, nl ? nl + 1 : "");
        }
        touch_stop();
        adb_disconnect(&g_dev);
        return 0;
    }

    for (;;) {
        uint32_t remote = 0;
        rc = open_video(&g_dev, &remote, &o);
        if (rc != 0) {
            LOG("✗ 开 screenrecord 通道失败 (%d), 1 s 后重试\n", rc);
            if (o.once) return 11;
            sleep(1);
            continue;
        }
        sessions++;
        mark_active(1);
        LOG("✓ 视频通道已开 (第 %d 次), 手机开始吐 H.264\n", sessions);

        for (;;) {
            uint32_t cmd, a0, a1; uint8_t *data; int len;
            rc = adb_recv_packet(&g_dev, 3000, &cmd, &a0, &a1, &data, &len);
            if (rc == -2) continue;                     /* 3 s 没包: 正常 (静止画面不出帧) */
            if (rc < 0) {
                LOG("✗ USB 读错误 %d (手机重枚举 / 线接触不良), 本进程内重连, 不重启管线\n", rc);
                want_reconnect = 1;
                break;
            }

            if (cmd == ADB_CMD_WRTE) {
                if (a1 == CH_VIDEO && len > 0) {
                    if (write_all(STDOUT_FILENO, data, len) != 0) { adb_free_packet(data); goto out; }
                    bytes += len;
                }
                adb_send_okay(&g_dev, a1, a0);
            } else if (cmd == ADB_CMD_CLSE) {
                if (a1 == CH_VIDEO) {
                    adb_free_packet(data);
                    LOG("屏幕录制结束 (180 s 上限到 / 手机端主动停), 重开…\n");
                    break;
                }
                if (a1 == CH_TOUCH) { touch_note_closed(); LOG("⚠ 触摸通道被关\n"); }
            }
            adb_free_packet(data);

            long el = now_ms() - t0;
            if (el >= 10000) {
                LOG("速率 %.1f Mbit/s  (累计 %.1f MB)\n",
                    (bytes - t_bytes) * 8.0 / (el * 1000.0), bytes / 1048576.0);
                t_bytes = bytes; t0 = now_ms();
            }
        }
        if (o.once) break;
        if (want_reconnect) break;
        usleep(200000);
    }

    if (want_reconnect && !o.once) {
        mark_active(0);
        if (o.touch) touch_stop();
        adb_disconnect(&g_dev);
        want_reconnect = 0;
        reconnects++;
        LOG("… 第 %d 次重连 (手机重枚举后 USB 句柄已失效)\n", reconnects);
        goto reconnect;
    }

out:
    mark_active(0);
    if (o.touch) touch_stop();
    adb_disconnect(&g_dev);
    return 0;
}
