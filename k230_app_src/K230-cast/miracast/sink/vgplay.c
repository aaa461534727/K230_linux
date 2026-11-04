/*
 * vgplay.c — K230 投屏硬件缩放播放器 (VGLite 2.5D GPU + DRM 裸 ioctl)
 *
 * 为什么写它:
 *   - drmplay.c 的 NV12 路径是 **CPU 逐像素** YUV→RGB + 双线性缩放 (480x800 屏,
 *     1080p 源), 单核被吃满, 且画质只能靠点采样.
 *   - K230 的 AI2D (/dev/video4) **没有缩放器** (驱动只编程 src size, 无 dst size),
 *     往小缓冲写大图 = 越界, 2026-10-25 实测直接把板子带死 ⇒ 此路不通.
 *   - VO 层有 HSCALE/VSCALE 寄存器, 但 canaan_vo.c 没实现, 要改内核重烧.
 *   - **VGLite (gc8000ul) 内核已开、用户态有 libvg_lite.so**: vg_lite_blit_rect 支持
 *     NV12 源 + 任意缩放 + 双线性滤波 + YUV→RGB 一次完成, 还能把 DRM dumb buffer
 *     直接 vg_lite_map 成渲染目标 ⇒ CPU 只负责一次 memcpy.
 *
 * 输入 (与 drmplay -H 完全一致, 可直接替换管道末段):
 *   v4l2_dec -H 的输出: 每帧 16 字节头 ("NV12", w(u16 LE), h(u16 LE), len(u32 LE), 4B pad)
 *   + w*h*3/2 字节 NV12 (Y 平面 + UV 交织)
 *
 * 用法:
 *   vgplay [-d /dev/dri/card0] [-w 480] [-h 800] [-i W H] [-f] [-C] [-r fps] [-n 帧] [-B] [-v]
 *   vgplay --selftest        # 只测 GPU (不用屏, 不打断空闲黑屏), ok 返回 0
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "vg_lite.h"

/* ---------- drm 裸定义 (与 drmplay.c 保持一致) ---------- */
#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) \
    ((3u << 30) | (((uint32_t)sizeof(type) & 0x3ffu) << 16) | \
     ((uint32_t)DRM_IOCTL_BASE << 8) | (nr))

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_get_resources {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

#define DRM_IOCTL_MODE_GETRESOURCES  DRM_IOWR(0xA0, struct drm_mode_get_resources)
#define DRM_IOCTL_MODE_SETCRTC       DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER    DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_CREATE_DUMB   DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB      DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_ADDFB2        DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)

#define DRM_MODE_CONNECTED 1
#define FORMAT_AR24 0x34325241u   /* 'AR24' */

#define LOG(fmt, ...) fprintf(stderr, "[vgplay] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "[vgplay] ✗ " fmt "\n", ##__VA_ARGS__)

static int g_verbose;

static struct timespec *pace_next(struct timespec *next, double fps)
{
    if (fps <= 0)
        return NULL;
    next->tv_nsec += (long)(1e9 / fps);
    while (next->tv_nsec >= 1000000000L) { next->tv_nsec -= 1000000000L; next->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
    return next;
}

/* GPU 渲染完 → CPU 拷进 DRM scanout 缓冲。
 * 为什么不让 GPU 直接写 DRM 内存: 2026-10-25 实测 vg_lite_map(DMABUF) 绑 dumb buffer,
 * 第一帧 64x64 就把整机卡死 (board_hwscale_test 第 2 步)。GPU 只写自己 allocate 的内存就没这问题;
 * 480x800x4 = 1.5 MB/帧, 拷一次 ~2 ms, 相比原来逐像素 YUV→RGB 仍是数量级的节省。 */
static void present_to_drm(vg_lite_buffer_t *src, uint8_t *map, int pitch, int W, int H)
{
    const uint8_t *s = (const uint8_t *)src->memory;
    for (int y = 0; y < H; y++)
        memcpy(map + (size_t)y * pitch, s + (size_t)y * src->stride, (size_t)W * 4);
}

/* ---------- 屏上几何 (与 drmplay 同规则, 保证观感一致) ---------- */
static void geom_fit(int W, int H, int cover,
                     int cx, int cy, int cw, int ch,
                     int *dw, int *dh, int *ox, int *oy, int *scaled)
{
    double fw = (double)W / cw;
    double fh = (double)H / ch;
    double f = cover ? (fw > fh ? fw : fh) : (fw < fh ? fw : fh);
    int w = (int)(cw * f + 0.5);
    int h = (int)(ch * f + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (!cover) {
        if (w > W) w = W;
        if (h > H) h = H;
    }
    *dw = w;
    *dh = h;
    *ox = (W - w) / 2;
    *oy = (H - h) / 2;
    *scaled = (w != cw || h != ch || cx != 0 || cy != 0);
}

/* 自动裁黑边: 扫 YUYV 的 Y 分量 (每像素 2 字节, Y 在偶数字节) 找非黑 bbox (同 drmplay) */
static int detect_bbox_yuyv(const uint8_t *yp, int sw, int sh,
                            int *bx, int *by, int *bw, int *bh)
{
    const int TH = 24;
    int minx = sw, maxx = -1, miny = sh, maxy = -1;

    for (int y = 0; y < sh; y += 2) {
        const uint8_t *row = yp + (size_t)y * sw * 2;
        int first = -1, last = -1;
        for (int x = 0; x < sw; x += 4) {
            if (row[x * 2] > TH) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first >= 0) {
            if (first < minx) minx = first;
            if (last > maxx) maxx = last;
            if (y < miny) miny = y;
            maxy = y;
        }
    }
    if (maxx < 0 || maxy < 0)
        return 0;

    minx = minx - 4 > 0 ? minx - 4 : 0;
    maxx = maxx + 4 < sw ? maxx + 4 : sw - 1;
    miny = miny - 2 > 0 ? miny - 2 : 0;
    maxy = maxy + 2 < sh ? maxy + 2 : sh - 1;

    if (maxx - minx < sw / 5 || maxy - miny < sh / 5)
        return 0;
    if (maxx - minx > sw * 9 / 10 && maxy - miny > sh * 9 / 10)
        return 0;

    *bx = minx;
    *by = miny;
    *bw = maxx - minx + 1;
    *bh = maxy - miny + 1;
    return 1;
}

/* ---------- DRM 初始化 ---------- */
static int find_connector(int fd, uint32_t *out_id, uint32_t *out_crtc)
{
    struct drm_mode_get_resources res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        ERR("GETRESOURCES: %s", strerror(errno));
        return -1;
    }
    uint32_t *fbs  = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(*fbs));
    uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(*crtcs));
    uint32_t *ids = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(*ids));
    uint32_t *encs = calloc(res.count_encoders ? res.count_encoders : 1, sizeof(*encs));
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)ids;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        ERR("GETRESOURCES(2): %s", strerror(errno));
        free(fbs); free(crtcs); free(ids); free(encs);
        return -1;
    }

    int found = -1;
    for (uint32_t i = 0; i < res.count_connectors && found != 0; i++) {
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof(c));
        c.connector_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        if (c.connection != DRM_MODE_CONNECTED)
            continue;

        uint32_t *enc_list = calloc(c.count_encoders ? c.count_encoders : 1, sizeof(*enc_list));
        struct drm_mode_modeinfo *cmodes = calloc(c.count_modes ? c.count_modes : 1, sizeof(*cmodes));
        void *cprops = calloc(c.count_props ? c.count_props : 1, sizeof(uint32_t));
        uint64_t *cpvals = calloc(c.count_props ? c.count_props : 1, sizeof(uint64_t));
        c.encoders_ptr = (uint64_t)(uintptr_t)enc_list;
        c.modes_ptr = (uint64_t)(uintptr_t)cmodes;
        c.props_ptr = (uint64_t)(uintptr_t)cprops;
        c.prop_values_ptr = (uint64_t)(uintptr_t)cpvals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            free(enc_list); free(cmodes); free(cprops); free(cpvals);
            continue;
        }

        uint32_t cand[16];
        uint32_t ncand = 0;
        if (c.encoder_id)
            cand[ncand++] = c.encoder_id;
        for (uint32_t k = 0; k < c.count_encoders && ncand < 16; k++)
            if (enc_list[k] && enc_list[k] != c.encoder_id)
                cand[ncand++] = enc_list[k];
        free(enc_list); free(cmodes); free(cprops); free(cpvals);

        for (uint32_t k = 0; k < ncand; k++) {
            struct drm_mode_get_encoder e;
            memset(&e, 0, sizeof(e));
            e.encoder_id = cand[k];
            if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0)
                continue;
            uint32_t crtc_id = e.crtc_id;
            if (!crtc_id) {
                for (uint32_t b = 0; b < res.count_crtcs && b < 32; b++) {
                    if (e.possible_crtcs & (1u << b)) { crtc_id = crtcs[b]; break; }
                }
            }
            if (!crtc_id)
                continue;
            *out_id = ids[i];
            *out_crtc = crtc_id;
            found = 0;
            if (g_verbose)
                LOG("connector %u (encoder %u, crtc %u)", *out_id, e.encoder_id, *out_crtc);
            break;
        }
    }
    free(fbs); free(crtcs); free(ids); free(encs);
    if (found != 0)
        ERR("没找到 connected connector");
    return found;
}

/* ---------- GPU 自检: 不碰屏, 只验证 libvg_lite + /dev/vg_lite 能用 ---------- */
static int gpu_selftest(void)
{
    vg_lite_buffer_t src, dst;
    vg_lite_matrix_t m;
    vg_lite_error_t e;
    int rc = 1;

    e = vg_lite_init(64, 64);
    if (e != VG_LITE_SUCCESS) { ERR("vg_lite_init: %d", (int)e); return 1; }

    memset(&src, 0, sizeof(src));
    src.width = 64; src.height = 64; src.format = VG_LITE_YUYV; src.stride = 64 * 2;
    e = vg_lite_allocate(&src);
    if (e != VG_LITE_SUCCESS) { ERR("allocate YUYV: %d", (int)e); goto out_close; }

    memset(&dst, 0, sizeof(dst));
    dst.width = 64; dst.height = 64; dst.format = VG_LITE_BGRA8888;
    e = vg_lite_allocate(&dst);
    if (e != VG_LITE_SUCCESS) { ERR("allocate BGRA: %d", (int)e); goto out_src; }

    /* 白场 YUYV: 每像素对 = Y0 U Y1 V = 235,128,235,128 */
    {
        uint8_t *p = (uint8_t *)src.memory;
        memset(p, 128, (size_t)src.stride * 64);
        for (size_t i = 0; i < (size_t)src.stride * 64; i += 2)
            p[i] = 235;
    }
    vg_lite_flush_mapped_buffer(&src);

    vg_lite_clear(&dst, NULL, 0xFF000000u);
    vg_lite_identity(&m);
    e = vg_lite_blit(&dst, &src, &m, VG_LITE_BLEND_NONE, 0, VG_LITE_FILTER_BI_LINEAR);
    if (e != VG_LITE_SUCCESS) { ERR("blit: %d", (int)e); goto out_dst; }
    e = vg_lite_finish();
    if (e != VG_LITE_SUCCESS) { ERR("finish: %d", (int)e); goto out_dst; }

    {
        const uint8_t *p = (const uint8_t *)dst.memory + (size_t)(32 * dst.stride) + 32 * 4;
        unsigned b = p[0], g = p[1], r = p[2], a = p[3];
        LOG("selftest 像素 BGRA = %u,%u,%u,%u", b, g, r, a);
        if (r > 200 && g > 200 && b > 200)
            rc = 0;
        else
            ERR("selftest 像素不是白 (GPU CSC/写回异常)");
    }

out_dst:
    vg_lite_free(&dst);
out_src:
    vg_lite_free(&src);
out_close:
    vg_lite_close();
    return rc;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/dri/card0";
    int W = 480, H = 800;
    int iw = 0, ih = 0;
    int fill_mode = 0;      /* -f: cover */
    int crop_mode = 1;      /* -C: 关自动裁黑边 */
    int blank_only = 0;     /* -B */
    int hdr_mode = 0;       /* -H */
    int max_frames = 0;
    int pattern_w = 0, pattern_h = 0;   /* --pattern W H: 不上管道, 自己造 N 帧 YUYV 测 GPU+DRM */
    double fps = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 2 < argc) { iw = atoi(argv[++i]); ih = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f")) fill_mode = 1;
        else if (!strcmp(argv[i], "-C")) crop_mode = 0;
        else if (!strcmp(argv[i], "-H")) hdr_mode = 1;
        else if (!strcmp(argv[i], "-B") || !strcmp(argv[i], "--blank")) blank_only = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--selftest")) return gpu_selftest();
        else if (!strcmp(argv[i], "--pattern") && i + 2 < argc) {
            pattern_w = atoi(argv[++i]); pattern_h = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-?")) {
            fprintf(stderr,
                "用法: %s [-d /dev/dri/card0] [-w 480] [-h 800] [-i W H] [-f] [-C]\n"
                "          [-H] [-r fps] [-n 帧] [-B] [-v] | --selftest\n"
                "  -H   stdin 每帧 16 字节头 (v4l2_dec -H), NV12\n"
                "  -f   等比 cover 铺满 (默认 contain 留黑边)\n"
                "  -C   关闭自动裁黑边\n"
                "  -B   只刷纯黑占屏 (防紫) 并常驻\n"
                "  --pattern W H   自己造帧测 GPU+DRM (不看 stdin, 用 -n 控帧数)\n", argv[0]);
            return 0;
        }
        else { fprintf(stderr, "未知参数: %s\n", argv[i]); return 2; }
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) { ERR("open %s: %s", dev, strerror(errno)); return 1; }

    uint32_t cid = 0, crtc = 0;
    if (find_connector(fd, &cid, &crtc) != 0) { close(fd); return 1; }

    /* dumb buffer (AR24) —— 既是 DRM scanout, 也是 VGLite 渲染目标 */
    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof(cd));
    cd.width = (uint32_t)W; cd.height = (uint32_t)H; cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        ERR("CREATE_DUMB %dx%d: %s", W, H, strerror(errno));
        close(fd); return 1;
    }
    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof(fb));
    fb.width = (uint32_t)W; fb.height = (uint32_t)H; fb.pixel_format = FORMAT_AR24;
    fb.handles[0] = cd.handle; fb.pitches[0] = cd.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        ERR("ADDFB2(AR24) %dx%d: %s", W, H, strerror(errno));
        close(fd); return 1;
    }
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof(md));
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        ERR("MAP_DUMB: %s", strerror(errno));
        close(fd); return 1;
    }
    uint8_t *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (map == MAP_FAILED) { ERR("mmap: %s", strerror(errno)); close(fd); return 1; }
    /* 先填不透明黑再点屏 (与 drmplay 一致): 内核给的是全 0, alpha=0 在 AR24 下等于全透明,
     * SETCRTC 到首次绘制之间那一小段会扫到未定义内容 = 紫色闪一下 */
    { uint32_t *_q = (uint32_t *)map; for (size_t _i = 0; _i < cd.size / 4; _i++) _q[_i] = 0xFF000000u; }

    /* 点屏 */
    struct drm_mode_get_connector q;
    memset(&q, 0, sizeof(q));
    q.connector_id = cid;
    struct drm_mode_modeinfo mode;
    memset(&mode, 0, sizeof(mode));
    int have_mode = 0;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) == 0 && q.count_modes > 0) {
        struct drm_mode_modeinfo *modes = calloc(q.count_modes, sizeof(*modes));
        uint32_t *qencs = calloc(q.count_encoders ? q.count_encoders : 1, sizeof(*qencs));
        void *props = calloc(q.count_props ? q.count_props : 1, sizeof(uint32_t));
        uint64_t *pvals = calloc(q.count_props ? q.count_props : 1, sizeof(uint64_t));
        q.encoders_ptr = (uint64_t)(uintptr_t)qencs;
        q.modes_ptr = (uint64_t)(uintptr_t)modes;
        q.props_ptr = (uint64_t)(uintptr_t)props;
        q.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) == 0) {
            for (uint32_t i = 0; i < q.count_modes; i++) {
                if (modes[i].hdisplay == (uint16_t)W && modes[i].vdisplay == (uint16_t)H) {
                    mode = modes[i]; have_mode = 1; break;
                }
            }
            if (!have_mode) { mode = modes[0]; have_mode = 1; }
        }
        free(modes); free(qencs); free(props); free(pvals);
    }
    if (!have_mode) {
        mode.hdisplay = (uint16_t)W; mode.vdisplay = (uint16_t)H; mode.vrefresh = 60;
        snprintf(mode.name, sizeof(mode.name), "%dx%d", W, H);
    }

    struct drm_mode_crtc sc;
    memset(&sc, 0, sizeof(sc));
    uint32_t one = cid;
    sc.set_connectors_ptr = (uint64_t)(uintptr_t)&one;
    sc.count_connectors = 1;
    sc.crtc_id = crtc;
    sc.fb_id = fb.fb_id;
    sc.mode = mode;
    sc.mode_valid = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
        ERR("SETCRTC: %s", strerror(errno));
        close(fd); return 1;
    }

    /* ---- VGLite ---- */
    vg_lite_error_t ve = vg_lite_init(W, H);
    if (ve != VG_LITE_SUCCESS) {
        ERR("vg_lite_init(%dx%d): %d", W, H, (int)ve);
        close(fd); return 1;
    }

    vg_lite_buffer_t target;
    memset(&target, 0, sizeof(target));
    target.width = W;
    target.height = H;
    target.format = VG_LITE_BGRA8888;   /* AR24 内存序 = B,G,R,A */
    ve = vg_lite_allocate(&target);
    if (ve != VG_LITE_SUCCESS) {
        ERR("vg_lite_allocate target %dx%d: %d", W, H, (int)ve);
        vg_lite_close(); close(fd); return 1;
    }
    /* 先铺不透明黑 (alpha=0 会让面板扫到未定义内容 = 紫) */
    vg_lite_clear(&target, NULL, 0xFF000000u);
    vg_lite_finish();
    present_to_drm(&target, map, (int)cd.pitch, W, H);
    LOG("✅ 上屏: connector %u crtc %u %ux%u AR24 (GPU 渲染 → CPU 拷贝上屏)", cid, crtc, W, H);

    if (blank_only) {
        LOG("  -B 黑屏常驻 (VGLite)");
        for (;;) pause();
    }

    if (pattern_w > 0 && pattern_h > 0) {
        /* 只测 GPU+DRM: 自己造 YUYV 渐变帧, 不走管道 (隔离"解码器问题"与"GPU 问题") */
        int sw = pattern_w, sh = pattern_h;
        int n = max_frames > 0 ? max_frames : 30;
        vg_lite_buffer_t ps;
        int dw, dh, ox, oy, scaled;

        memset(&ps, 0, sizeof(ps));
        ps.width = sw; ps.height = sh; ps.format = VG_LITE_YUYV; ps.stride = sw * 2;
        ve = vg_lite_allocate(&ps);
        if (ve != VG_LITE_SUCCESS) { ERR("pattern allocate %dx%d: %d", sw, sh, (int)ve); return 1; }

        geom_fit(W, H, fill_mode, 0, 0, sw, sh, &dw, &dh, &ox, &oy, &scaled);
        LOG("pattern: %dx%d YUYV → %dx%d @ %d,%d, %d 帧", sw, sh, dw, dh, ox, oy, n);
        vg_lite_clear(&target, NULL, 0xFF000000u);
        vg_lite_finish();

        for (int f = 0; f < n; f++) {
            uint8_t *p = (uint8_t *)ps.memory;
            for (int y = 0; y < sh; y++) {
                uint8_t *row = p + (size_t)y * ps.stride;
                for (int x = 0; x < sw; x += 2) {
                    row[x * 2 + 0] = (uint8_t)(((x + f * 4) % sw) * 255 / sw);
                    row[x * 2 + 1] = 128;
                    row[x * 2 + 2] = (uint8_t)(((x + 2 + f * 4) % sw) * 255 / sw);
                    row[x * 2 + 3] = 128;
                }
            }
            vg_lite_flush_mapped_buffer(&ps);
            {
                vg_lite_matrix_t m;
                vg_lite_identity(&m);
                vg_lite_translate((vg_lite_float_t)ox, (vg_lite_float_t)oy, &m);
                vg_lite_scale((vg_lite_float_t)dw / sw, (vg_lite_float_t)dh / sh, &m);
                ve = vg_lite_blit(&target, &ps, &m, VG_LITE_BLEND_NONE, 0, VG_LITE_FILTER_BI_LINEAR);
                if (ve != VG_LITE_SUCCESS) { ERR("pattern blit: %d", (int)ve); vg_lite_free(&ps); return 1; }
            }
            ve = vg_lite_finish();
            if (ve != VG_LITE_SUCCESS) { ERR("pattern finish: %d", (int)ve); vg_lite_free(&ps); return 1; }
            present_to_drm(&target, map, (int)cd.pitch, W, H);
            fprintf(stderr, "[vgplay] pattern frame %d ok\n", f + 1);
        }
        vg_lite_free(&ps);
        vg_lite_free(&target);
        vg_lite_close();
        close(fd);
        return 0;
    }

    /* ---- 帧循环 ---- */
    int sw = iw > 0 ? iw : W;
    int sh = ih > 0 ? ih : H;
    int cx = 0, cy = 0, cw = sw, ch = sh;
    int dw, dh, ox, oy, scaled;
    geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);

    vg_lite_buffer_t src;
    memset(&src, 0, sizeof(src));
    src.width = sw; src.height = sh; src.format = VG_LITE_YUYV; src.stride = sw * 2;
    ve = vg_lite_allocate(&src);
    if (ve != VG_LITE_SUCCESS) {
        ERR("vg_lite_allocate YUYV %dx%d: %d", sw, sh, (int)ve);
        vg_lite_close(); close(fd); return 1;
    }

    size_t frame = (size_t)sw * sh * 2;   /* YUYV 4:2:2 */
    uint8_t *buf = malloc(frame);
    if (!buf) { ERR("malloc 失败"); vg_lite_free(&src); vg_lite_close(); close(fd); return 1; }
    size_t buf_cap = frame;

    int need_clear = 1, redo_crop = 1;
    int pvx = -1, pvy = -1, pvw = -1, pvh = -1;   /* 待确认的裁剪候选 */
    long n = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    LOG("输入: NV12 %dx%d → 屏 %dx%d (绘制区 %dx%d @ %d,%d)", sw, sh, W, H, dw, dh, ox, oy);

    while (1) {
        if (hdr_mode) {
            unsigned char hd[16];
            size_t g = 0;
            while (g < sizeof(hd)) {
                ssize_t r = read(STDIN_FILENO, hd + g, sizeof(hd) - g);
                if (r == 0) goto done;
                if (r < 0) { if (errno == EINTR) continue; ERR("read hdr: %s", strerror(errno)); goto done; }
                g += (size_t)r;
            }
            if (memcmp(hd, "YUY2", 4) != 0) {
                ERR("帧头 magic=%c%c%c%c, 需要 YUY2 (解码器要带 -y; VGLite 这块芯片只吃 YUYV, 不吃 NV12)",
                    hd[0], hd[1], hd[2], hd[3]);
                goto done;
            }
            int nw = hd[4] | (hd[5] << 8), nh = hd[6] | (hd[7] << 8);
            uint32_t nl = (uint32_t)hd[8] | ((uint32_t)hd[9] << 8) |
                          ((uint32_t)hd[10] << 16) | ((uint32_t)hd[11] << 24);
            if (nw > 0 && nh > 0 && (nw != sw || nh != sh)) {
                sw = nw; sh = nh;
                cx = 0; cy = 0; cw = sw; ch = sh;
                geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                need_clear = 1; redo_crop = 1;
                pvx = -1; pvy = -1; pvw = -1; pvh = -1;
                vg_lite_free(&src);
                memset(&src, 0, sizeof(src));
                src.width = sw; src.height = sh; src.format = VG_LITE_YUYV; src.stride = sw * 2;
                ve = vg_lite_allocate(&src);
                if (ve != VG_LITE_SUCCESS) { ERR("re-allocate YUYV %dx%d: %d", sw, sh, (int)ve); goto done; }
                frame = (size_t)sw * sh * 2;
                if (frame > buf_cap) {
                    uint8_t *nb = realloc(buf, frame);
                    if (!nb) { ERR("realloc 失败"); goto done; }
                    buf = nb; buf_cap = frame;
                }
                LOG("输入变为 %dx%d → 绘制区 %dx%d @ %d,%d", sw, sh, dw, dh, ox, oy);
            }
            if (nl > 0) {
                frame = nl;
                if (frame > buf_cap) {
                    uint8_t *nb = realloc(buf, frame);
                    if (!nb) { ERR("realloc 失败"); goto done; }
                    buf = nb; buf_cap = frame;
                }
            }
        }

        size_t got = 0;
        while (got < frame) {
            ssize_t r = read(STDIN_FILENO, buf + got, frame - got);
            if (r == 0) goto done;
            if (r < 0) { if (errno == EINTR) continue; ERR("read: %s", strerror(errno)); goto done; }
            got += (size_t)r;
        }

        /* 裁黑边 (与 drmplay 同一套判据, 保证观感不跳变) */
        if (crop_mode && (n == 0 || redo_crop || n % (n < 150 ? 5 : 60) == 0)) {
            int bx, by, bw, bh;
            redo_crop = 0;
            if (detect_bbox_yuyv(buf, sw, sh, &bx, &by, &bw, &bh)) {
                int ddx = abs(bx - cx), ddy = abs(by - cy);
                int ddw = abs(bw - cw), ddh = abs(bh - ch);
                if (n == 0 || ddx > 16 || ddy > 16 || ddw > 16 || ddh > 16) {
                    int zoom_in = (bw <= cw && bh <= ch);
                    int confirmed = (pvx >= 0 && abs(bx - pvx) <= 24 && abs(by - pvy) <= 24 &&
                                     abs(bw - pvw) <= 24 && abs(bh - pvh) <= 24);
                    if (n == 0 || !zoom_in || confirmed) {
                        cx = bx; cy = by; cw = bw; ch = bh;
                        pvx = -1; pvy = -1; pvw = -1; pvh = -1;
                        geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                        need_clear = 1;
                        LOG("自动裁黑边: 源 %dx%d @ %d,%d (整帧 %dx%d) → 绘制区 %dx%d @ %d,%d",
                            cw, ch, cx, cy, sw, sh, dw, dh, ox, oy);
                    } else {
                        pvx = bx; pvy = by; pvw = bw; pvh = bh;
                    }
                }
            } else if (n == 0 && (cx || cy || cw != sw || ch != sh)) {
                cx = 0; cy = 0; cw = sw; ch = sh;
                geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                need_clear = 1;
            }
        }

        /* 拷进 GPU 缓冲 (stride 不一致时按行拷) */
        if (src.stride == sw * 2) {
            memcpy(src.memory, buf, frame);
        } else {
            for (int y = 0; y < sh; y++)
                memcpy((uint8_t *)src.memory + (size_t)y * src.stride,
                       buf + (size_t)y * sw * 2, (size_t)sw * 2);
        }
        vg_lite_flush_mapped_buffer(&src);

        if (need_clear) {
            /* 只在几何变化时清黑底: 每帧都清会闪 (踩过的坑) */
            need_clear = 0;
            vg_lite_clear(&target, NULL, 0xFF000000u);
        }

        {
            vg_lite_matrix_t m;
            vg_lite_rectangle_t rect = { (vg_lite_int32_t)cx, (vg_lite_int32_t)cy,
                                         (vg_lite_int32_t)cw, (vg_lite_int32_t)ch };
            double sx = (double)dw / cw;
            double sy = (double)dh / ch;
            vg_lite_identity(&m);
            vg_lite_translate((vg_lite_float_t)ox, (vg_lite_float_t)oy, &m);
            vg_lite_scale((vg_lite_float_t)sx, (vg_lite_float_t)sy, &m);
            vg_lite_translate((vg_lite_float_t)(-cx), (vg_lite_float_t)(-cy), &m);
            ve = vg_lite_blit_rect(&target, &src, &rect, &m, VG_LITE_BLEND_NONE, 0,
                                   VG_LITE_FILTER_BI_LINEAR);
            if (ve != VG_LITE_SUCCESS) { ERR("blit_rect: %d", (int)ve); goto done; }
        }
        ve = vg_lite_finish();
        if (ve != VG_LITE_SUCCESS) { ERR("finish: %d", (int)ve); goto done; }
        present_to_drm(&target, map, (int)cd.pitch, W, H);

        n++;
        if (max_frames > 0 && n >= max_frames) break;
        pace_next(&next, fps);
    }

done:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "[vgplay] %ld 帧, %.2f s, %.1f fps (GPU 缩放)\n",
                n, dt, dt > 0 ? n / dt : 0.0);
    }
    vg_lite_free(&src);
    vg_lite_free(&target);
    vg_lite_close();
    free(buf);
    munmap(map, cd.size);
    close(fd);
    return 0;
}
