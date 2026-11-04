#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/*
 * drmplay.c — 最小 DRM 播放器 (K230 DSI 480x800, 裸 ioctl, 零依赖)
 *
 * 为什么写它:
 *   1. K230 的 canaan-drm 驱动**不支持** XRGB8888 dumb buffer 的 AddFB
 *      (`failed to add fb: Invalid argument`), 只认 AR24/AR12/AR15/RG24/...,
 *      所以 SDL2 的 kmsdrm 后端 (固定 XRGB8888) 起不来, ffmpeg 的 fbdev 输出
 *      也没有 /dev/fb0 可用。
 *   2. 板上没有 libdrm 头文件 (只有 libdrm.so.2 运行时), 而 Buildroot 将来也不会有。
 *      ⇒ 本文件**手写 drm ioctl**, 不 include 任何 libdrm 头, 交叉编/本机编都一样。
 *
 * 用途 (Miracast sink 出画面):
 *   ffmpeg -i <收到的 h264 流> -vf scale=480:800 -pix_fmt rgba -f rawvideo - \
 *     | ./drmplay -w 480 -h 800
 *   (每帧 w*h*4 字节 RGBA, 从 stdin 顺序读, 读到 EOF 退出)
 *
 * 用法:
 *   drmplay [-d /dev/dri/card0] [-w 480] [-h 800] [-c <connector_id>] [-v]
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
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>

/* ---------- drm 裸定义 (抄自 include/uapi/drm/drm*.h, 只留用到的) ---------- */
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

/* ---- 双缓冲 + PAGE_FLIP (2026-10-27 流畅度) ----
 * 为什么必须: 原来只有一块 dumb buffer 直接 mmap 直写。屏是 60 Hz DSI 持续扫描
 *   显存, 而 CPU 画一帧要 ~7 ms (360 KB 转换 + 1.5 MB 写)。写的过程中扫描线正在
 *   同一块内存上跑 ⇒ 每帧都扫到「上半新 / 下半旧」的混搭 (tearing), 而且写与扫的
 *   相位随机 ⇒ 主观就是「看着最多 25fps + 忽快忽慢不稳定」, 尽管 sink/dec/整圈
 *   计量都是 60fps。改成两块 buffer 轮流画 + flip, 屏永远扫到完整的一帧。 */
struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};
struct drm_event_hdr {
    uint32_t type;
    uint32_t length;
};
struct drm_event_vblank {
    struct drm_event_hdr base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_EVENT_FLIP_COMPLETE  0x02
/* 2026-10-27 修正: 上一版误写成 0xBC (那是 ATOMIC), 内核没有匹配的 handler
 * 直接返回 EINVAL, 于是被误判成"驱动不支持 flip"。实测 0xB0 是好的:
 *   ioctl 0.05 ms, vblank 事件 16.45 ms 后到, 与 60 Hz 完全对齐。 */
#define DRM_IOCTL_MODE_CRTC_PAGE_FLIP DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
enum { NBUF_MAX = 3 };

#define DRM_IOCTL_MODE_GETRESOURCES  DRM_IOWR(0xA0, struct drm_mode_get_resources)
#define DRM_IOCTL_MODE_SETCRTC       DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER    DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_CREATE_DUMB   DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB      DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_ADDFB2        DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)

#define DRM_MODE_CONNECTED 1

/* fourcc('A','R','2','4') —— ARGB8888; canaan-drm 支持, XRGB8888 不支持 */
#define FORMAT_AR24 0x34325241u
/* fourcc('N','V','1','2') */
#define FORMAT_NV12 0x3231564eu

/* ---------- 方案A (2026-11-04): NV12 直上 VO 视频层 ----------
 * 为什么: 每帧 CPU 3.1 ms 全花在「旋转 + NV12→AR24」和写 1.43 MB 上, 而硬件都能做:
 *   · VO 视频层原生收 NV12 (canaan_vo.c:709 video_plane_formats)
 *   · VO 的 yuv2rgb 已开 (canaan_vo.c:561 VO_DISP_YUV2RGB_CTL=1)
 *   · 转色后 CPU 还要写 1.43 MB/帧, 走视频层是 0 写
 * 做法: dumb buffer (bpp=8, 高=屏高*3/2) 建 NV12 fb → memcpy 帧体 → atomic commit
 *   到 video_3; 同时关掉 OSD4(primary) —— OSD 在视频层之上, 不关会把视频盖住。
 * 边界(v1): 只在「NV12 + 不缩放 + 不旋转 + 满屏」启用, 其它情况原路返回,
 *   黑边/旋转行为一个像素都不变。开关: WFD_VO=0 强制关, WFD_VO_DEBUG=1 打细节。 */
#define DRM_IOW(nr, type) \
    ((1u << 30) | (((uint32_t)sizeof(type) & 0x3ffu) << 16) | \
     ((uint32_t)DRM_IOCTL_BASE << 8) | (nr))

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};
struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};
struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};
struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};
struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
};
struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

#define DRM_IOCTL_MODE_GETPROPERTY       DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE          DRM_IOWR(0xB6, struct drm_mode_get_plane)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_ATOMIC            DRM_IOWR(0xBC, struct drm_mode_atomic)
#define DRM_IOCTL_SET_CLIENT_CAP         DRM_IOW(0x0d, struct drm_set_client_cap)

#define DRM_MODE_OBJECT_PLANE     0xeeeeeeeeu
#define DRM_MODE_ATOMIC_NONBLOCK  0x0200u
#define DRM_CLIENT_CAP_ATOMIC     3
#define DRM_PLANE_TYPE_PRIMARY    1
#define DRM_PLANE_TYPE_OVERLAY    0

struct vo_nv12 {
    int ok;             /* 平面+fb 都建成了 */
    int on;             /* 视频层当前在显示 (OSD4 已关) */
    int force_off;      /* WFD_VO=0 */
    int rot_ok;         /* WFD_VO_ROT=0 时禁掉「旋转也走视频层」, 退回旧 CPU 路径 */
    uint32_t crtc;
    int W, H;
    int nbuf;
    uint32_t ovl, prim;              /* 平面 id: video_3 / OSD4 */
    uint32_t o_fb, o_crtc, o_srcx, o_srcy, o_srcw, o_srch;
    uint32_t o_crtcx, o_crtcy, o_crtcw, o_crtch;
    uint32_t p_fb, p_crtc;
    uint32_t fb[NBUF_MAX];
    uint32_t handle[NBUF_MAX];
    uint8_t *map[NBUF_MAX];
    uint32_t pitch;
    int g_w, g_h, g_x, g_y;          /* 上次贴上去的几何 (变了就重刷黑边) */
    int logged;                      /* 第一次真正走视频层时打一行 */
    int logged2;                     /* 第一次走「缩放后直上视频层」时打一行 */
};

/* 把一帧 NV12 按 1:1 贴到视频层 (可带黑边), 返回 0 成功 / -1 这个几何不支持。
 * 只做「逐行拷贝 + 换几何时刷一次黑」: 不转色 (VO 的 yuv2rgb 做), 不缩放。
 * 黑边必须自己刷 —— OSD4 (primary) 在视频层之上, 出视频时必须关掉它,
 * 而 VO 背景色来自 DT 的 0xffffff (白), 不刷黑就漏白边。 */
static int vo_blit_nv12(struct vo_nv12 *v, int nxt, const uint8_t *buf,
                        int sw, int sh, int ox, int oy)
{
    const int W = v->W, H = v->H;
    uint8_t *m;
    const uint8_t *srcY, *srcUV;
    uint8_t *dstUV;
    size_t plane = (size_t)W * H;

    if (sw <= 0 || sh <= 0 || sw > W || sh > H)
        return -1;
    if (ox < 0 || oy < 0 || ox + sw > W || oy + sh > H)
        return -1;
    /* NV12 的 UV 平面按 2x2 采样: 偏移取偶数 (最多挪 1 像素, 肉眼不可见) */
    ox &= ~1;
    oy &= ~1;
    if (ox + sw > W || oy + sh > H)
        return -1;

    if (v->g_w != sw || v->g_h != sh || v->g_x != ox || v->g_y != oy) {
        /* 几何变了 (换会话/换方向): 三块缓冲都刷成限范围黑 (Y=16 / UV=128),
         * 否则上一段内容的残影会从黑边里漏出来 */
        for (int b = 0; b < v->nbuf; b++) {
            memset(v->map[b], 16, plane);
            memset(v->map[b] + plane, 128, plane / 2);
        }
        v->g_w = sw; v->g_h = sh; v->g_x = ox; v->g_y = oy;
    }

    m = v->map[nxt];
    srcY = buf;
    for (int y = 0; y < sh; y++)
        memcpy(m + (size_t)(oy + y) * W + ox, srcY + (size_t)y * sw, (size_t)sw);
    srcUV = buf + (size_t)sw * sh;
    dstUV = m + plane;
    for (int y = 0; y < sh / 2; y++)
        memcpy(dstUV + (size_t)(oy / 2 + y) * W + ox,
               srcUV + (size_t)y * sw, (size_t)sw);
    return 0;
}

/* 缩放后直上视频层 (2026-11-04): CPU 只做 **NV12→NV12 缩放**, 转色交 VO 的 yuv2rgb。
 *
 * 为什么需要: Miracast 真机的源永远是横档 (1280x720 → 竖屏内容裁出 409x720),
 * 屏是 480x800 ⇒ 必然要放大 1.17x/1.11x, 永远不满足「1:1 直贴」。
 * 旧路把「NV12→AR24 转色 + 最近邻缩放」一起做, 每帧 12 ms;
 * 这里只保留缩放 (输出 1.5 B/px, 旧路输出 4 B/px), 转色白送给 VO 硬件。
 *
 * 采样算法与旧路完全一致 (最近邻 + xmap 查表), 画质不变。
 * 返回 0 = 已贴好; -1 = 这个几何不支持 (调用方自动回旧路)。 */
static int vo_blit_nv12_scaled(struct vo_nv12 *v, int nxt, const uint8_t *buf,
                               int sw, int sh, int cx, int cy, int cw, int ch,
                               int ox, int oy, int dw, int dh)
{
    static int *sxmap = NULL;
    static int sxcap = 0;
    const int W = v->W, H = v->H;
    const size_t plane = (size_t)W * H;

    dw &= ~1; dh &= ~1;                       /* NV12 的 UV 按 2x2 组, 必须偶数 */
    if (dw <= 0 || dh <= 0 || cw <= 0 || ch <= 0)
        return -1;
    if (cx < 0 || cy < 0 || cx + cw > sw || cy + ch > sh || sw <= 0 || sh <= 0)
        return -1;
    ox &= ~1; oy &= ~1;
    if (ox < 0 || oy < 0 || ox + dw > W || oy + dh > H)
        return -1;

    if (v->g_w != dw || v->g_h != dh || v->g_x != ox || v->g_y != oy) {
        /* 几何变了: 三块缓冲都刷限范围黑 (Y=16 / UV=128), 免得残影从黑边漏出来 */
        for (int b = 0; b < v->nbuf; b++) {
            memset(v->map[b], 16, plane);
            memset(v->map[b] + plane, 128, plane / 2);
        }
        v->g_w = dw; v->g_h = dh; v->g_x = ox; v->g_y = oy;
    }

    if (dw > sxcap) {
        int *nm = realloc(sxmap, (size_t)dw * sizeof(int));
        if (!nm)
            return -1;
        sxmap = nm; sxcap = dw;
    }
    for (int dx = 0; dx < dw; dx++)
        sxmap[dx] = cx + (int)((long)dx * cw / dw);

    uint8_t *m = v->map[nxt];
    const uint8_t *yp = buf;
    const uint8_t *uvp = buf + (size_t)sw * sh;

    /* Y 平面: 逐目标行取源行, 逐点查表搬字节 */
    for (int dy = 0; dy < dh; dy++) {
        int sry = cy + (int)((long)dy * ch / dh);
        const uint8_t *srow = yp + (size_t)sry * sw;
        uint8_t *drow = m + (size_t)(oy + dy) * W + ox;
        for (int dx = 0; dx < dw; dx++)
            drow[dx] = srow[sxmap[dx]];
    }
    /* UV 平面: 每 2x2 目标块共享一对 UV (源行号 / 2, 源列对齐到偶数) */
    for (int dy = 0; dy < dh / 2; dy++) {
        int sry = cy + (int)((long)(dy * 2) * ch / dh);
        const uint8_t *srow = uvp + (size_t)(sry / 2) * sw;
        uint8_t *drow = m + plane + (size_t)(oy / 2 + dy) * W + ox;
        for (int dx = 0; dx < dw; dx += 2) {
            int sx = sxmap[dx] & ~1;
            drow[dx] = srow[sx];
            drow[dx + 1] = srow[sx + 1];
        }
    }
    return 0;
}

/* 日志统一带墙钟 (2026-10-26 加): play.log 以前没有时间戳 ⇒ K80 黑屏那一轮
 * 无法与 RTSP / dec 日志对齐到秒。现在三条日志同一条时间轴, 出错能一眼对轮次。 */
static void plog(const char *mark, const char *fmt, va_list ap)
{
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[drmplay]%s %02d:%02d:%02d ", mark,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
static void plog_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); plog("", fmt, ap); va_end(ap);
}
static void plog_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); plog(" ✗", fmt, ap); va_end(ap);
}

#define LOG(fmt, ...) plog_info(fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) plog_err(fmt, ##__VA_ARGS__)

/* ============ 方案A 的 DRM 辅助 (裸 ioctl, 不依赖 libdrm) ============ */
static int g_verbose;

/* 取对象的某个属性 id; 找到时 *out_val 带回当前值 */
static uint32_t vo_prop_get(int fd, uint32_t obj, uint32_t type,
                            const char *want, uint64_t *out_val)
{
    struct drm_mode_obj_get_properties gp;
    uint32_t *ids = NULL;
    uint64_t *vals = NULL;
    uint32_t found = 0;

    memset(&gp, 0, sizeof(gp));
    gp.obj_id = obj;
    gp.obj_type = type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0)
        return 0;
    if (gp.count_props == 0 || gp.count_props > 256)
        return 0;
    ids = calloc(gp.count_props, sizeof(*ids));
    vals = calloc(gp.count_props, sizeof(*vals));
    if (!ids || !vals) { free(ids); free(vals); return 0; }
    gp.props_ptr = (uint64_t)(uintptr_t)ids;
    gp.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &gp) < 0) {
        free(ids); free(vals);
        return 0;
    }
    for (uint32_t i = 0; i < gp.count_props; i++) {
        struct drm_mode_get_property p;
        memset(&p, 0, sizeof(p));
        p.prop_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) < 0)
            continue;
        if (strcmp(p.name, want) == 0) {
            found = ids[i];
            if (out_val) *out_val = vals[i];
            break;
        }
    }
    free(ids); free(vals);
    return found;
}

/* 找 video_3 (overlay + 支持 NV12) 和 OSD4 (primary) */
static int vo_probe_planes(int fd, struct vo_nv12 *v)
{
    struct drm_mode_get_plane_res pr;
    uint32_t *ids = NULL;
    int ret = -1;

    memset(&pr, 0, sizeof(pr));
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        ERR("GETPLANERESOURCES: %s", strerror(errno));
        return -1;
    }
    if (pr.count_planes == 0 || pr.count_planes > 64)
        return -1;
    ids = calloc(pr.count_planes, sizeof(*ids));
    if (!ids) return -1;
    pr.plane_id_ptr = (uint64_t)(uintptr_t)ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0) {
        ERR("GETPLANERESOURCES(2): %s", strerror(errno));
        free(ids);
        return -1;
    }

    for (uint32_t i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane pl;
        uint32_t *fm;
        uint64_t ptype = 0xff;
        int has_nv12 = 0;

        memset(&pl, 0, sizeof(pl));
        pl.plane_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) < 0)
            continue;
        fm = calloc(pl.count_format_types ? pl.count_format_types : 1, sizeof(*fm));
        if (!fm) break;
        pl.format_type_ptr = (uint64_t)(uintptr_t)fm;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &pl) == 0) {
            for (uint32_t k = 0; k < pl.count_format_types; k++)
                if (fm[k] == FORMAT_NV12) { has_nv12 = 1; break; }
        }
        free(fm);
        if (!vo_prop_get(fd, pl.plane_id, DRM_MODE_OBJECT_PLANE, "type", &ptype))
            continue;
        if (g_verbose)
            LOG("plane %u: type=%llu nv12=%d crtc=%u fb=%u", pl.plane_id,
                (unsigned long long)ptype, has_nv12, pl.crtc_id, pl.fb_id);
        if (ptype == DRM_PLANE_TYPE_PRIMARY && !v->prim) {
            v->prim = pl.plane_id;
        } else if (ptype == DRM_PLANE_TYPE_OVERLAY && has_nv12 && !v->ovl) {
            v->ovl = pl.plane_id;
        }
    }
    free(ids);

    if (!v->ovl || !v->prim)
        return -1;

    v->o_fb   = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    v->o_crtc = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    v->o_srcx = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
    v->o_srcy = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
    v->o_srcw = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
    v->o_srch = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
    v->o_crtcx = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
    v->o_crtcy = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
    v->o_crtcw = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
    v->o_crtch = vo_prop_get(fd, v->ovl, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
    v->p_fb   = vo_prop_get(fd, v->prim, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    v->p_crtc = vo_prop_get(fd, v->prim, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);

    if (!v->o_fb || !v->o_crtc || !v->o_srcx || !v->o_srcy || !v->o_srcw ||
        !v->o_srch || !v->o_crtcx || !v->o_crtcy || !v->o_crtcw || !v->o_crtch ||
        !v->p_fb || !v->p_crtc) {
        return -1;
    }
    ret = 0;
    return ret;
}

/* 建 nbuf 块 NV12 dumb buffer 并 AddFB2 (单 handle + offset 1 = pitch*H) */
static int vo_make_fbs(int fd, struct vo_nv12 *v, int W, int H, int nbuf)
{
    for (int b = 0; b < nbuf; b++) {
        struct drm_mode_create_dumb cd;
        struct drm_mode_map_dumb md;
        struct drm_mode_fb_cmd2 fb;
        uint8_t *m;

        memset(&cd, 0, sizeof(cd));
        cd.width = (uint32_t)W;
        cd.height = (uint32_t)H * 3 / 2;
        cd.bpp = 8;
        cd.flags = 0;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
            ERR("CREATE_DUMB(NV12 %dx%d): %s", W, H, strerror(errno));
            return -1;
        }
        if ((int)cd.pitch != W) {
            ERR("NV12 pitch %u != 宽 %d ⇒ 放弃硬件上屏", cd.pitch, W);
            return -1;
        }
        memset(&fb, 0, sizeof(fb));
        fb.width = (uint32_t)W;
        fb.height = (uint32_t)H;
        fb.pixel_format = FORMAT_NV12;
        fb.handles[0] = cd.handle;
        fb.handles[1] = cd.handle;
        fb.pitches[0] = cd.pitch;
        fb.pitches[1] = cd.pitch;
        fb.offsets[0] = 0;
        fb.offsets[1] = cd.pitch * (uint32_t)H;
        if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
            ERR("ADDFB2(NV12): %s", strerror(errno));
            return -1;
        }
        memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
            ERR("MAP_DUMB(NV12): %s", strerror(errno));
            return -1;
        }
        m = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
        if (m == MAP_FAILED) {
            ERR("mmap(NV12): %s", strerror(errno));
            return -1;
        }
        v->fb[b] = fb.fb_id;
        v->handle[b] = cd.handle;
        v->map[b] = m;
    }
    v->pitch = (uint32_t)W;
    return 0;
}

/* show=1: 视频层出 NV12, OSD4 关; show=0: 视频层关, OSD4 出 prim_fb */
static int vo_commit(int fd, struct vo_nv12 *v, int show,
                     uint32_t ovl_fb, uint32_t prim_fb, int want_event)
{
    uint32_t objs[2] = { v->ovl, v->prim };
    uint32_t counts[2] = { 10, 2 };
    uint32_t props[12];
    uint64_t vals[12];
    struct drm_mode_atomic at;
    int n = 0;

    props[n] = v->o_fb;    vals[n++] = ovl_fb;
    props[n] = v->o_crtc;  vals[n++] = show ? v->crtc : 0;
    props[n] = v->o_srcx;  vals[n++] = 0;
    props[n] = v->o_srcy;  vals[n++] = 0;
    props[n] = v->o_srcw;  vals[n++] = (uint64_t)v->W << 16;
    props[n] = v->o_srch;  vals[n++] = (uint64_t)v->H << 16;
    props[n] = v->o_crtcx; vals[n++] = 0;
    props[n] = v->o_crtcy; vals[n++] = 0;
    props[n] = v->o_crtcw; vals[n++] = (uint64_t)v->W;
    props[n] = v->o_crtch; vals[n++] = (uint64_t)v->H;
    /* OSD4 在视频层之上: 出视频时必须把它关掉 (CRTC_ID=0) */
    props[n] = v->p_fb;    vals[n++] = prim_fb;
    props[n] = v->p_crtc;  vals[n++] = prim_fb ? v->crtc : 0;

    memset(&at, 0, sizeof(at));
    at.flags = want_event ? (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK) : 0;
    at.count_objs = 2;
    at.objs_ptr = (uint64_t)(uintptr_t)objs;
    at.count_props_ptr = (uint64_t)(uintptr_t)counts;
    at.props_ptr = (uint64_t)(uintptr_t)props;
    at.prop_values_ptr = (uint64_t)(uintptr_t)vals;
    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &at) < 0) {
        ERR("MODE_ATOMIC(show=%d): %s", show, strerror(errno));
        return -1;
    }
    return 0;
}

static void vo_init(int fd, struct vo_nv12 *v, uint32_t crtc, int W, int H,
                    int nbuf, uint32_t prim_fb)
{
    struct drm_set_client_cap cap;
    int rc;

    memset(v, 0, sizeof(*v));
    v->crtc = crtc;
    v->W = W;
    v->H = H;
    v->nbuf = nbuf;
    {
        const char *e = getenv("WFD_VO");
        if (e && e[0] == '0') { v->force_off = 1; }
        /* 2026-11-04: 旋转帧也走视频层 (转置在 CPU 已按块做完, 见 rot90_cw_nv12)。
         * WFD_VO_ROT=0 可单独退回「旋转帧走旧 CPU 路径」, 用于 A/B 或出问题时止损。 */
        e = getenv("WFD_VO_ROT");
        v->rot_ok = !(e && e[0] == '0');
    }
    if (v->force_off) {
        LOG("⚠ WFD_VO=0: 强制走【已弃用】CPU 转 NV12→AR24 (只用于 A/B 对照)");
        return;
    }
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) < 0) {
        LOG("⚠ VO 不可用: 无 atomic 能力 (%s) → 退回【已弃用】CPU 路 (应急)", strerror(errno));
        return;
    }
    cap.capability = 2;   /* DRM_CLIENT_CAP_UNIVERSAL_PLANES */
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);

    if (vo_probe_planes(fd, v) < 0) {
        LOG("⚠ VO 不可用: 找不到 (overlay+NV12 / primary) 平面 → 退回【已弃用】CPU 路 (应急)");
        return;
    }
    if (vo_make_fbs(fd, v, W, H, nbuf) < 0) {
        LOG("⚠ VO 不可用: NV12 fb 建不出来 → 退回【已弃用】CPU 路 (应急)");
        return;
    }
    /* 空跑一次: 关视频层 + 出黑 OSD4, 验证 atomic 通 */
    rc = vo_commit(fd, v, 0, 0, prim_fb, 0);
    if (rc < 0) {
        LOG("⚠ VO 不可用: atomic commit 不通 → 退回【已弃用】CPU 路 (应急)");
        return;
    }
    v->ok = 1;
    LOG("✅ 方案A 就绪: video_3=%u(NV12) OSD4=%u 已关, %d 块 NV12 fb (pitch %u) — CPU 不再碰像素",
        v->ovl, v->prim, nbuf, v->pitch);
}


/* ---------- 卡住等待一帧的间隔 (调试用): 通过环境变量控制 ---------- */
static int g_verbose;

/* 按目标帧率节流 (0 = 不限速, 有多少画多快) */
static void pace(struct timespec *next, double fps)
{
    if (fps <= 0)
        return;
    next->tv_nsec += (long)(1e9 / fps);
    while (next->tv_nsec >= 1000000000L) { next->tv_nsec -= 1000000000L; next->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

/* 找一块 connected connector; 指定 cid 时优先用它 */
static int find_connector(int fd, int want_id, uint32_t *out_id,
                          uint32_t *out_encoder, uint32_t *out_crtc)
{
    struct drm_mode_get_resources res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        ERR("GETRESOURCES: %s", strerror(errno));
        return -1;
    }
    /* 注意: kernel 第二趟会往**所有** count>0 的指针里写 (哪怕你不关心),
     * 留 NULL 就 EFAULT → 所以 fb/crtc/connector/encoder 四组都要给真数组 */
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
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c;
        memset(&c, 0, sizeof(c));
        c.connector_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
            continue;
        if (g_verbose)
            LOG("connector %u: connection=%u modes=%u encoder=%u encoders=%u", ids[i],
                c.connection, c.count_modes, c.encoder_id, c.count_encoders);
        if (c.connection != DRM_MODE_CONNECTED)
            continue;
        if (want_id > 0 && (int)ids[i] != want_id)
            continue;

        /* 拿 connector 的候选 encoder 列表 (本驱动 connector->encoder_id 常为 0,
         * 此时必须从 possible encoders 里挑, 光看 encoder_id 会失败) */
        uint32_t *enc_list = calloc(c.count_encoders ? c.count_encoders : 1,
                                    sizeof(*enc_list));
        struct drm_mode_modeinfo *cmodes =
            calloc(c.count_modes ? c.count_modes : 1, sizeof(*cmodes));
        void *cprops = calloc(c.count_props ? c.count_props : 1, sizeof(uint32_t));
        uint64_t *cpvals = calloc(c.count_props ? c.count_props : 1, sizeof(uint64_t));
        c.encoders_ptr = (uint64_t)(uintptr_t)enc_list;
        c.modes_ptr = (uint64_t)(uintptr_t)cmodes;
        c.props_ptr = (uint64_t)(uintptr_t)cprops;
        c.prop_values_ptr = (uint64_t)(uintptr_t)cpvals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0) {
            ERR("GETCONNECTOR(%u): %s", ids[i], strerror(errno));
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

        int ok = -1;
        for (uint32_t k = 0; k < ncand; k++) {
            struct drm_mode_get_encoder e;
            memset(&e, 0, sizeof(e));
            e.encoder_id = cand[k];
            if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0) {
                ERR("GETENCODER(%u): %s", cand[k], strerror(errno));
                continue;
            }
            uint32_t crtc_id = e.crtc_id;
            if (!crtc_id) {
                /* 未绑定 CRTC: 从 possible_crtcs 位图里挑第一个可用的 */
                for (uint32_t b = 0; b < res.count_crtcs && b < 32; b++) {
                    if (e.possible_crtcs & (1u << b)) { crtc_id = crtcs[b]; break; }
                }
            }
            if (!crtc_id) {
                ERR("encoder %u 无可用 CRTC", cand[k]);
                continue;
            }
            *out_id = ids[i];
            *out_encoder = e.encoder_id;
            *out_crtc = crtc_id;
            ok = 0;
            if (g_verbose)
                LOG("选用 connector %u (encoder %u, crtc %u)", *out_id, *out_encoder, *out_crtc);
            break;
        }
        if (ok == 0) {
            found = 0;
            break;
        }
    }
    free(fbs);
    free(crtcs);
    free(ids);
    free(encs);
    if (found != 0)
        ERR("没找到 connected connector (want=%d)", want_id);
    return found;
}

/* 1:1 原始像素直贴开关 (定义见下方 WFD_1X); 必须在 geom_fit 之前声明 */
static int g_1x = 1;
static int g_nostore = 0;   /* 诊断: 转换仍做, 但不写屏缓冲 (量 store 占多少) */

/* ---- 共享内存帧缓冲 (2026-10-28 零拷贝) ----
 * 为什么: 解码出图改回原始尺寸 (1280x720 NV12 = 1.35 MB) 后, 帧要经管道从 v4l2_dec
 *   搬到 drmplay。实测这一趟「读管道」4.4~6.0 ms, 加上转换要从缓冲里再读 0.45 MB、
 *   往屏上写 1.18 MB, 整圈 19~20 ms ⇒ 单核只跑到 50 fps。
 * 做法: 解码端把帧体直接写进 /run/wfd-frame 的两个槽 (mmap 文件, tmpfs 内存),
 *   管道里只留 16 字节帧头 + 4 字节序号当「有新帧」的信号 (保证 idle/断线语义不变)。
 *   帧头的 len 字段最高位置 1 表示"帧体在共享内存"; 没置位就是老协议 (帧体在管道里),
 *   所以两端任意一端没配 WFD_SHM 都能自动退化成老路径。 */
#define SHM_HEAD 64
#define SHM_SLOT (4u << 20)
static uint8_t *g_shm_base = NULL;   /* 映射基址 (NULL = 走管道) */
static size_t   g_shm_slot = SHM_SLOT;
/* 槽头: [0]=写序号 (dec 发布), [4]=读序号 (本进程消费完发布) —— 解码端靠它回压,
 * 否则帧会一路堆在 dec 的拼包缓冲里 (实测涨到 5.4 s 帧龄)。 */
static volatile uint32_t *g_shm_w = NULL, *g_shm_r = NULL;

static void shm_map(void)
{
    const char *p = getenv("WFD_SHM");
    if (!p || !*p) return;
    size_t sz = SHM_HEAD + 2 * g_shm_slot;
    int fd = open(p, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        LOG("⚠ 共享内存 %s 打不开 (%s) → 回退管道搬运", p, strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sz) {
        if (ftruncate(fd, (off_t)sz) != 0) {
            LOG("⚠ 共享内存 %s 扩到 %zu 失败 (%s) → 回退管道", p, sz, strerror(errno));
            close(fd); return;
        }
    }
    void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        LOG("⚠ 共享内存 mmap 失败 (%s) → 回退管道搬运", strerror(errno));
        return;
    }
    g_shm_base = (uint8_t *)m;
    g_shm_w = (volatile uint32_t *)(g_shm_base + 0);
    g_shm_r = (volatile uint32_t *)(g_shm_base + 4);
    LOG("共享内存帧缓冲: %s (2 槽 × %zu KB, 帧头 %d B, 管道只走 4 B 序号)",
        p, g_shm_slot / 1024, SHM_HEAD);
}

/* 由源裁剪矩形 (cx,cy,cw,ch) 算出屏上绘制矩形 (dw,dh @ ox,oy)。
 * cover=0: 等比 contain —— 完整显示源, 余量黑边 (默认)
 * cover=1: 等比 cover   —— 铺满整屏, 超出部分居中裁掉 (不变形) */
/* ---- 上屏几何发布 (给 UIBC 反控换算用, 2026-10-28) ----
 * 为什么: 1:1 直贴时画面贴在屏中央, 上下左右留黑边; 反控侧若用写死的
 *   "画面在 19..469" 换算, 触点会整体偏 10~40 px —— 点图标点到邻居。
 *   drmplay 本来就知道真实几何, 写出来让反控侧直接做像素级平移,
 *   换手机 / 换分辨率 / 换裁边结果都自动跟着走 (通用 sink 要的就是这个)。
 * 文件格式 (一行 8 个整数, 单位像素):
 *   ox oy dw dh   ← 画面在屏上的矩形 (480x800 坐标系)
 *   cx cy cw ch   ← 同一块内容在会话帧里的矩形 (1280x720 坐标系)
 * 读不到文件 = 反控侧退回老的 WFD_UIBC_AREA / WFD_UIBC_SPAN 环境变量算法。 */
#define UIBC_GEOM_FILE "/run/wfd-uibc-geom"

static void publish_uibc_geom(int ox, int oy, int dw, int dh,
                              int cx, int cy, int cw, int ch)
{
    static int last[8];
    static int have;
    int g[8];
    char b[128];
    int n, fd;

    if (dw <= 0 || dh <= 0 || cw <= 0 || ch <= 0)
        return;
    g[0] = ox; g[1] = oy; g[2] = dw; g[3] = dh;
    g[4] = cx; g[5] = cy; g[6] = cw; g[7] = ch;
    if (have && memcmp(g, last, sizeof(g)) == 0)
        return;                        /* 几何没变就不重写, 免得每帧落盘 */
    memcpy(last, g, sizeof(g));
    have = 1;
    n = snprintf(b, sizeof(b), "%d %d %d %d %d %d %d %d\n",
                 g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
    fd = open(UIBC_GEOM_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, b, (size_t)n) != n) { /* 失败就下次几何变化再写 */ }
        close(fd);
    }
}

/* ---- 横屏内容自动旋转 (2026-10-29, 主人需求: 苹果电脑横屏内容要尽量占满 480x800) ----
 * 为什么不用驱动的 rotation 属性: verisilicon DC 虽然声明支持 ROTATE_90 (官方 demo
 *   v4l2-drm-three-camera 也在用), 但它对 AR24/NV12 的 src-dst 交换语义没有文档,
 *   还要把整条上屏路径从 legacy SETCRTC 改成 atomic 提交; 试错成本远大于收益。
 * 这里改在"源"侧: 帧体进来先整体转 90 度 (NV12 三个平面一起转), 后面所有缩放/裁边/
 *   1:1 直贴逻辑一个字都不用改 —— 1280x720 转成 720x1280 后, 原有的 contain 自然
 *   得到 450x800 (占屏 94%), 而 480x270 只有 34%。
 * 代价: 每帧多一次 1.38 MB 的转置拷贝 (分块做, 读/写都连续), 30fps 下够用。 */

/* 1 字节/像素的 90 度顺时针转置: src w x h → dst h x w, dst(x,y) = src(y, h-1-x)
 * 分块 32x32: 每块读 32 条连续 32 字节, 写 32 条连续 32 字节 (避免整列跨步写)。 */
static void rot90_cw_u8(const uint8_t *src, uint8_t *dst, int w, int h)
{
    uint8_t t[32][32];
    for (int y0 = 0; y0 < h; y0 += 32) {
        int th = (y0 + 32 <= h) ? 32 : h - y0;
        for (int x0 = 0; x0 < w; x0 += 32) {
            int tw = (x0 + 32 <= w) ? 32 : w - x0;
            for (int i = 0; i < th; i++)
                memcpy(&t[i][0], src + (size_t)(y0 + i) * w + x0, (size_t)tw);
            for (int j = 0; j < tw; j++) {
                uint8_t *d = dst + (size_t)(x0 + j) * h + (size_t)(h - y0 - th);
                for (int i = th - 1; i >= 0; i--)
                    *d++ = t[i][j];
            }
        }
    }
}

/* 2 字节/元素 (NV12 的 UV 对) 的 90 度顺时针转置; w,h 以"元素"计 */
static void rot90_cw_pair(const uint8_t *src8, uint8_t *dst8, int w, int h)
{
    const uint16_t *src = (const uint16_t *)src8;
    uint16_t *dst = (uint16_t *)dst8;
    uint16_t t[16][16];
    for (int y0 = 0; y0 < h; y0 += 16) {
        int th = (y0 + 16 <= h) ? 16 : h - y0;
        for (int x0 = 0; x0 < w; x0 += 16) {
            int tw = (x0 + 16 <= w) ? 16 : w - x0;
            for (int i = 0; i < th; i++)
                memcpy(&t[i][0], src + (size_t)(y0 + i) * w + x0, (size_t)tw * 2);
            for (int j = 0; j < tw; j++) {
                uint16_t *d = dst + (size_t)(x0 + j) * h + (size_t)(h - y0 - th);
                for (int i = th - 1; i >= 0; i--)
                    *d++ = t[i][j];
            }
        }
    }
}

/* BGRA/RGBA → NV12 (BT.601 limited range)。2026-11-04 主人定「只维护新路」后加:
 * 现在显示只有一条路 —— NV12 上 VO 视频层 (硬件转色)。老的 `-p bgra/rgba` 源
 * (ffmpeg / 旧脚本) 必须先在这里变成 NV12, 才能进同一条路。
 * 真机四条投屏链 (Miracast/AirPlay/CarPlay/CarLife/USB) 都是 v4l2_dec 出 NV12,
 * 所以这一步在板上实际不会跑, 只是把「非 NV12」这个例外也收进新路。
 * UV 取 2x2 平均 (标准 4:2:0), 奇数宽/高时忽略最后一行/列。 */
static void bgra_to_nv12(uint8_t *dst, const uint8_t *src, int w, int h, int rgba)
{
    uint8_t *yp = dst;
    uint8_t *uvp = dst + (size_t)w * h;
    for (int r = 0; r < h; r++) {
        const uint8_t *s = src + (size_t)r * w * 4;
        uint8_t *d = yp + (size_t)r * w;
        for (int c = 0; c < w; c++, s += 4) {
            int b = s[0], g = s[1], rr = s[2];
            if (rgba) { int t = b; b = rr; rr = t; }   /* rgba: 低字节是 R */
            d[c] = (uint8_t)(((66 * rr + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    int wc = w & ~1, hc = h & ~1;
    for (int r = 0; r < hc / 2; r++) {
        const uint8_t *s0 = src + (size_t)(r * 2) * w * 4;
        const uint8_t *s1 = s0 + (size_t)w * 4;
        uint8_t *du = uvp + (size_t)r * w;
        for (int c = 0; c < wc / 2; c++) {
            const uint8_t *p[4] = { s0 + c * 8, s0 + c * 8 + 4,
                                    s1 + c * 8, s1 + c * 8 + 4 };
            int b = 0, g = 0, rr = 0;
            for (int k = 0; k < 4; k++) {
                int bb = p[k][0], gg = p[k][1], r2 = p[k][2];
                if (rgba) { int t = bb; bb = r2; r2 = t; }
                b += bb; g += gg; rr += r2;
            }
            b = (b + 2) >> 2; g = (g + 2) >> 2; rr = (rr + 2) >> 2;
            du[c * 2]     = (uint8_t)(((-38 * rr - 74 * g + 112 * b + 128) >> 8) + 128);
            du[c * 2 + 1] = (uint8_t)(((112 * rr - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

/* NV12 整帧 90 度顺时针: Y 平面 + UV 平面 (两者尺寸相同, 各 siz/2) */
static void rot90_cw_nv12(const uint8_t *src, uint8_t *dst, int sw, int sh)
{
    size_t ysz = (size_t)sw * sh;
    rot90_cw_u8(src, dst, sw, sh);
    rot90_cw_pair(src + ysz, dst + ysz, sw / 2, sh / 2);
}

/* WFD_ROT: auto (默认) / 0 / 90 —— -1 表示 auto */
static int g_rot_env = -1;

static double contain_area(int cw, int ch, int W, int H)
{
    double fw = (double)W / cw, fh = (double)H / ch;
    double f = fw < fh ? fw : fh;
    return (cw * f) * (ch * f);
}

/* 该不该转: 只比"转完能占多大", 不认型号 —— 竖屏内容转过去更小就不转,
 * 横屏内容 (苹果电脑 / 平板横屏) 转完更大就转。5% 滞回防止临界抖动。 */
static int pick_rot(int cw, int ch, int W, int H)
{
    if (cw <= 0 || ch <= 0)
        return 0;
    if (g_rot_env == 0)
        return 0;
    if (g_rot_env == 90)
        return 90;
    if (W >= H)
        return 0;                        /* 屏本身是横屏, 没有转的必要 */
    return (contain_area(cw, ch, H, W) > contain_area(cw, ch, W, H) * 1.05) ? 90 : 0;
}

/* mode: 0 = contain (等比, 留黑边)
 *       1 = cover   (等比, 居中裁切铺满)
 *       2 = stretch (满屏, 两轴各自缩放 ⇒ 允许变形, 一点内容都不丢)
 * 2026-11-03 主人定: 手机投屏固定 stretch —— cover 会把 337x714 的内容上下
 * 各裁掉 108 行 (21%), 状态栏/导航栏直接看不见; 宁可轻微压扁也不要丢内容。 */
static void geom_fit(int W, int H, int mode,
                     int cx, int cy, int cw, int ch,
                     int *dw, int *dh, int *ox, int *oy, int *scaled)
{
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    double fw = (double)W / cw;
    double fh = (double)H / ch;
    int w, h;
    if (mode == 2) {
        w = W;
        h = H;
    } else {
        double f = mode ? (fw > fh ? fw : fh) : (fw < fh ? fw : fh);
        w = (int)(cw * f + 0.5);
        h = (int)(ch * f + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (!mode) {
            if (w > W) w = W;
            if (h > H) h = H;
        }
    }
    /* 自适应铺满 (2026-10-28 K80 适配): 内容区能塞进屏时优先 1:1 原始像素直贴,
     * 但**只有它本来就基本占满屏**才算数 —— 否则宁可等比放大到 contain 上限。
     * 判据: 1:1 尺寸在两个方向上都至少占屏 80%。
     *   · 努比亚 409x720 → 85% / 90% ⇒ 保持 1:1 (已验收过的画面一个像素都不动)
     *   · K80   337x720 → 70% / 90% ⇒ 放大到 374x800 (屏高吃满, 左右各 53 px 黑边)
     * 这条规则只看"量出来的占比", 不看手机型号 ⇒ 换任何手机都自动适配。 */
    if (g_1x && mode == 0 && cw <= W && ch <= H) {
        if (cw * 100 >= W * 80 && ch * 100 >= H * 80) {
            w = cw;
            h = ch;
        } else {
            LOG("自适应放大: 内容 %dx%d 1:1 只占屏 %dx%d 的 %d%%/%d%%, 等比铺到 %dx%d",
                cw, ch, W, H, cw * 100 / W, ch * 100 / H, w, h);
        }
    }
    *dw = w;
    *dh = h;
    *ox = (W - w) / 2;
    *oy = (H - h) / 2;
    *scaled = (w != cw || h != ch || cx != 0 || cy != 0);
    /* 把真实上屏几何发布给反控侧 (坐标换算用它, 见函数头注释) */
    publish_uibc_geom(*ox, *oy, *dw, *dh, cx, cy, cw, ch);
}

/* 自动找 NV12 Y 平面里的"非黑内容" bbox (按 4px/2px 步长采样, 够快)。
 * 为什么需要: 手机投屏常把竖屏画面塞进 1920x1080, 左右各 ~1/3 纯黑;
 * 只做"整帧等比缩放"就只能得到屏幕中间一条 480x270。裁掉黑边后再缩放,
 * 竖屏内容才能铺满 480x800 (实测 646x1080 → 480x795)。
 * 返回 1 = 找到可信内容区; 0 = 整帧都黑/太暗/本来就满幅 (不裁)。 */
static int detect_bbox_nv12(const uint8_t *yp, int sw, int sh,
                            int *bx, int *by, int *bw, int *bh)
{
    const int TH = 24;                 /* 亮度阈值 (16 为标准黑电平) */
    int minx = sw, maxx = -1, miny = sh, maxy = -1;

    for (int y = 0; y < sh; y += 2) {
        const uint8_t *row = yp + (size_t)y * sw;
        int first = -1, last = -1;
        for (int x = 0; x < sw; x += 4) {
            if (row[x] > TH) {
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

    /* 采样步长带来的边界补偿 */
    minx = minx - 4 > 0 ? minx - 4 : 0;
    maxx = maxx + 4 < sw ? maxx + 4 : sw - 1;
    miny = miny - 2 > 0 ? miny - 2 : 0;
    maxy = maxy + 2 < sh ? maxy + 2 : sh - 1;

    /* 内容太窄/太矮 ⇒ 大概率只是暗场景里的几个亮点, 不裁 (防抖) */
    if (maxx - minx < sw / 5 || maxy - miny < sh / 5)
        return 0;
    /* 本来就没有黑边 ⇒ 不裁, 保持原行为 */
    if (maxx - minx > sw * 9 / 10 && maxy - miny > sh * 9 / 10)
        return 0;

    *bx = minx;
    *by = miny;
    *bw = maxx - minx + 1;
    *bh = maxy - miny + 1;
    return 1;
}

/* 从 fd 读满 n 字节。
 *   返回值: 1 = 读满; 0 = 空闲 (整包还没开始时等超过 idle_ms 没数据); -1 = 出错/EOF。
 *   为什么要它: 上屏进程改成常驻 (读命名管道), 会话之间管道里没数据是常态,
 *   绝不能像以前那样读不到就退出 —— 一退就没人持有 DRM plane, 面板立刻扫出紫色。 */
/* 等 fd 可读: 1 = 可读, 0 = 超时, -1 = 出错。EINTR 自动续。 */
static int wait_readable(int fd, int ms)
{
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLIN;
    pf.revents = 0;
    for (;;) {
        int pr = poll(&pf, 1, ms);
        if (pr > 0) return 1;
        if (pr == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* 从 fd 读满 n 字节。
 *   返回值: 1 = 读满; 0 = 空闲 (整包还没开始时等超过 idle_ms 没数据); -1 = 出错/EOF。
 *   为什么要它: 上屏进程改成常驻 (读命名管道), 会话之间管道里没数据是常态,
 *   绝不能像以前那样读不到就退出 —— 一退就没人持有 DRM plane, 面板立刻扫出紫色。
 *
 *   2026-10-27 (现场三连问题的根因): 常驻管道的 fd 是 O_RDWR|O_NONBLOCK 打开的
 *   (drmplay 自己持有两端, 所以永远等不到 EOF)。此时 read 拿不到数据返回 EAGAIN,
 *   老代码直接 continue ⇒ **用户态死循环**:
 *     · 实测: 断开投屏后 drmplay 空转 94% CPU (3 s 烧 284 tick, wchan=0);
 *     · 单核板上这等于把解码/收包全饿死 → 下一次投屏巨卡、P2P/RTSP 超时重连;
 *     · 而且它卡在「帧头读到了、帧体没读满」的位置, 画面永远冻在最后一帧
 *       (主人看到的「断开后 K230 还显示手机屏幕」)。
 *   ⇒ EAGAIN 必须 poll 等 (让出 CPU); 半帧等超时 (1 s) 就当截断帧丢弃返回 -1,
 *     调用方在常驻模式下走 idle (刷黑 + 复位), 不再永久卡住。 */
#define BODY_WAIT_MS 1000

/* 2026-11-02: AirPlay 有**两个**源, 会话标记文件是分开的 ——
 *   CarPlay 的 airplayd  → /run/airplay-active
 *   uxplay 镜像          → /run/airplay-cast-active (k230-airplay.service 的 AIRPLAY_ACTIVE_FILE)
 * 播放器必须两个都认, 否则 uxplay 侧「连上了但画面静止」会被当成「其它源静默」,
 * 2.4 s 就刷黑 (主人 2026-11-02 现场: 屏自己黑, 摸一下手机来帧才亮)。 */
static int any_airplay_active(void) {
    return access("/run/airplay-active", F_OK) == 0
        || access("/run/airplay-cast-active", F_OK) == 0;
}
/* ---- 管道定容 (2026-10-27 v12) ----
 * 按「2 帧 + 32 KB」, 上限 1 MB (本板 /proc/sys/fs/pipe-max-size)。
 *  • 太小 (1.34 帧) ⇒ dec 写第二帧就阻塞, 读端每帧都要等 dec 补齐剩下的半帧,
 *    整圈贴死 16.67 ms 的帧周期, 零余量。
 *  • 太大 ⇒ 管道里躺几帧 = 固定多几帧延迟 (主人报过"K230 比手机慢得多")。
 *  ⚠ 必须用**真实帧长**调: 首帧之前只有 -w/-h (480x800 = 576 KB) 的估算值,
 *    拿它定容会把 194 KB 的帧定成 1 MB = 5.4 帧 = +90 ms 延迟 (15:13 实测 101 ms)。
 *    所以帧头报出真实长度后再调一次。 */
static long set_pipe_cap(size_t frame, int verbose)
{
    long want = (long)frame * 2 + 32768;
    if (want < 65536) want = 65536;
    /* 2026-10-28: 上限从 1 MB 提到 4 MB。原因: 出图改回「解码原始尺寸」(1280x720)
     * 后一帧 1350 KB, 1 MB 管道装不下 ⇒ 读一帧要 read() 6 次, 实测「读管道」
     * 从 0.6 ms 涨到 5.5 ms, 整圈 17.8 ms > 60 fps 的 16.67 ms 预算 ⇒ 偶发
     * 2vbl(33 ms) 卡顿。4 MB 容 2.9 帧。
     * ⚠ 内核 /proc/sys/fs/pipe-max-size 必须同时 >= 这个值 (k230-screen.sh 启动时
     * 写 4 MB), 否则 F_SETPIPE_SZ 被拒并回落到 256 KB。 */
    if (want > 4194304) want = 4194304;
    /* A/B 旋钮: 直接指定管道字节数 (0/未设 = 按 2 帧自动算)。
     * 用途: 同一条抖动流对比「1.3 帧」和「2.7 帧」对 2vbl 的影响, 别再改代码重编。 */
    {
        const char *o = getenv("WFD_PIPE_CAP");
        if (o && *o) {
            long v = strtol(o, NULL, 0);
            if (v >= 65536) want = v > 4194304 ? 4194304 : v;
        }
    }
    long got = fcntl(STDIN_FILENO, F_SETPIPE_SZ, want);
    if (got <= 0) got = fcntl(STDIN_FILENO, F_SETPIPE_SZ, 4194304L);
    if (got <= 0) got = fcntl(STDIN_FILENO, F_SETPIPE_SZ, 1048576L);
    if (got <= 0) got = fcntl(STDIN_FILENO, F_SETPIPE_SZ, 65536L);
    if (verbose) {
        if (got > 0)
            LOG("管道缓冲: 申请 %ld → 实得 %ld 字节 (一帧 %zu ⇒ 容 %.1f 帧)", want, (long)got,
                frame, frame > 0 ? (double)got / (double)frame : 0.0);
        else
            LOG("管道缓冲: 设置失败 (%s), 仍是默认值", strerror(errno));
    }
    return got;
}
static int read_full(int fd, void *p, size_t n, int idle_ms)
{
    size_t g = 0;
    while (g < n) {
        if (idle_ms > 0) {
            int pr = wait_readable(fd, g == 0 ? idle_ms : 500);
            if (pr == 0)
                return g == 0 ? 0 : -1;   /* 只有「一个字节都还没来」才算空闲 */
            if (pr < 0) return -1;
        }
        ssize_t r = read(fd, (char *)p + g, n - g);
        if (r > 0) { g += (size_t)r; continue; }
        if (r == 0) return -1;                       /* EOF */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 非阻塞管道: 等数据到, 绝不忙等 (见上面注释) */
            int pr = wait_readable(fd, g == 0 ? (idle_ms > 0 ? idle_ms : 500) : BODY_WAIT_MS);
            if (pr == 0) return g == 0 ? 0 : -1;     /* 半帧超时 = 截断帧, 丢弃 */
            if (pr < 0) return -1;
            continue;
        }
        return -1;
    }
    return 1;
}

/* ===== 帧头重新对齐 (2026-10-27 现场 v10) ===== 
 * 背景: 空口丢包 → dec 卡住不写 → 本进程读帧体半截超时 (BODY_WAIT_MS) 回 idle,
 *   管道里留下半帧 ⇒ 之后每轮帧头都错位。现场日志 13:47:53~57 连刷几十条
 *   「帧头错位 (会话切换残留), 已丢弃 77xxxx 字节 → 回空闲」, 整场掉成卡顿/黑屏。
 * 原实现「drain 到 EAGAIN 就回 idle」在 dec 还在写时会自我维持错位循环。
 * 现实现: 在字节流里滑动找 "NV12" magic (4 KB 块读, 不逐字节), 校验 w/h/len 合理;
 *   找 magic 时多读进来的字节**不能丢** (丢了就是帧体前几千字节 → 下一帧又错位),
 *   统一存在 g_pend 里, 由 stream_read() 优先消费。
 */
static unsigned char g_pend[8192];
static int g_pend_len, g_pend_off;

static int stream_read(int fd, void *p, size_t n, int idle_ms)
{
    size_t g = 0;
    while (g_pend_off < g_pend_len && g < n)
        ((unsigned char *)p)[g++] = g_pend[g_pend_off++];
    if (g_pend_off >= g_pend_len) g_pend_off = g_pend_len = 0;
    if (g == n) return 1;
    int r = read_full(fd, (unsigned char *)p + g, n - g, idle_ms);
    if (r == 0 && g > 0) return -1;      /* 后半截没来 = 截断帧 */
    return r;
}

static int resync_header(int fd, unsigned char hd[16], int idle_ms)
{
    unsigned char blk[4096 + 16];
    int have = 16;
    long scanned = 0;
    memcpy(blk, hd, 16);          /* 错位残留可能有一部分已经读进 hd 了 */
    for (;;) {
        if (have < 16) {
            int pr = wait_readable(fd, have > 0 ? BODY_WAIT_MS : idle_ms);
            if (pr < 0) return -1;
            if (pr == 0) { if (have == 0) return 0; continue; }
            ssize_t r = read(fd, blk + have, sizeof(blk) - (size_t)have);
            if (r == 0) return -1;
            if (r < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return -1;
            }
            have += (int)r;
            scanned += (long)r;
        }
        int found = -1;
        for (int i = 0; i + 4 <= have; i++) {
            if (blk[i] == 'N' && blk[i+1] == 'V' && blk[i+2] == '1' && blk[i+3] == '2') { found = i; break; }
        }
        if (found < 0) {
            int keep = have >= 3 ? 3 : have;   /* 保留 3 字节, magic 可能跨块 */
            memmove(blk, blk + have - keep, (size_t)keep);
            have = keep;
            continue;
        }
        int avail = have - found;
        int cp = avail < 16 ? avail : 16;
        memcpy(hd, blk + found, (size_t)cp);
        if (cp < 16) {                  /* 帧头被块边界切开: 再补几个字节 */
            have = cp;
            memmove(blk, hd, (size_t)cp);
            continue;
        }
        int nw = hd[4] | (hd[5] << 8), nh = hd[6] | (hd[7] << 8);
        uint32_t nl = (uint32_t)hd[8] | ((uint32_t)hd[9] << 8) |
                      ((uint32_t)hd[10] << 16) | ((uint32_t)hd[11] << 24);
        if (nw <= 0 || nw > 4096 || nh <= 0 || nh > 4096 || nl > 8u * 1024 * 1024) {
            have -= found + 1;             /* 假 magic: 跳 1 字节继续找 */
            memmove(blk, blk + found + 1, (size_t)have);
            continue;
        }
        /* 成功: 帧头之后的字节必须留着 (那是帧体的开头) */
        int rest = avail - 16;
        if (rest > 0) {
            if (rest > (int)sizeof(g_pend)) rest = (int)sizeof(g_pend);
            memcpy(g_pend, blk + found + 16, (size_t)rest);
            g_pend_len = rest;
            g_pend_off = 0;
        }
        LOG("帧头重新对齐 (跳过 %ld 字节残留, 带回 %d 字节) %dx%d %u B",
            scanned - avail + found, rest > 0 ? rest : 0, nw, nh, nl);
        return 1;
    }
}

/* ============ NV12 → ARGB 行转换 (2026-10-27 单核提速) ============
 * 为什么: 现场"K230 比手机慢得多"的直接原因是每帧处理 (读管道 10 ms + 转换 8 ms)
 *   超过源端帧间隔 (18 ms) ⇒ 管道积压 ⇒ 延迟一路累积。转换原本 7~9 ms (≈35 Mpx/s),
 *   是整圈里唯一能砍的大头。
 * C908 有 RVV 1.0 (rv64imafdcv), 用 e32m4 一次算 16 个像素:
 *   UV 用 vrgather 从 16 字节里按 {0,0,2,2,...} / {1,1,3,3,...} 展开成每像素一组,
 *   然后全程都是 32 位向量乘加 + vmax/vmin 饱和, 没有一条分支。
 * 实测 (板端, 逐像素与标量版完全一致): 381x800 8.62→2.56 ms (3.4x), 960x540 14.85→4.72 ms (3.1x)。
 * 编译时没有 -march=rv64gcv 则自动退回标量 (__riscv_vector 未定义), 行为不变。 */
#if defined(__riscv_vector) && defined(__riscv_v_intrinsic)
#define NV12_HAVE_RVV 1
#include <riscv_vector.h>
#endif

/* 把一行里连续的 n 个 NV12 像素转成 ARGB, 写进 d[0..n)。
 *   yr  指向该行 Y 的**第一个要转的像素**
 *   uv  指向该行 UV 里与 yr[0] 配对的那一对 (要求 yr[0] 是偶数源列, 即 cx 为偶数) */
static void nv12_row_to_argb(uint32_t *d, const uint8_t *yr, const uint8_t *uv, int n)
{
#ifdef NV12_HAVE_RVV
    static const uint8_t idxU[16] = {0,0,2,2,4,4,6,6,8,8,10,10,12,12,14,14};
    static const uint8_t idxV[16] = {1,1,3,3,5,5,7,7,9,9,11,11,13,13,15,15};
    vuint8m1_t vidxU = __riscv_vle8_v_u8m1(idxU, 16);
    vuint8m1_t vidxV = __riscv_vle8_v_u8m1(idxV, 16);
    int nv = n & ~15;
    for (int x = 0; x < nv; x += 16) {
        size_t vl = 16;
        vuint8m1_t y8  = __riscv_vle8_v_u8m1(yr + x, vl);
        vuint8m1_t uv8 = __riscv_vle8_v_u8m1(uv + x, vl);
        vuint8m1_t u8  = __riscv_vrgather_vv_u8m1(uv8, vidxU, vl);
        vuint8m1_t v8  = __riscv_vrgather_vv_u8m1(uv8, vidxV, vl);

        vint32m4_t Yi = __riscv_vreinterpret_v_u32m4_i32m4(__riscv_vzext_vf4_u32m4(y8, vl));
        vint32m4_t Ui = __riscv_vreinterpret_v_u32m4_i32m4(__riscv_vzext_vf4_u32m4(u8, vl));
        vint32m4_t Vi = __riscv_vreinterpret_v_u32m4_i32m4(__riscv_vzext_vf4_u32m4(v8, vl));

        vint32m4_t c = __riscv_vsub_vx_i32m4(Yi, 16, vl);
        vint32m4_t Y = __riscv_vmul_vx_i32m4(c, 298, vl);
        vint32m4_t U = __riscv_vsub_vx_i32m4(Ui, 128, vl);
        vint32m4_t V = __riscv_vsub_vx_i32m4(Vi, 128, vl);

        vint32m4_t R = __riscv_vadd_vv_i32m4(Y, __riscv_vmul_vx_i32m4(V, 409, vl), vl);
        vint32m4_t G = __riscv_vadd_vv_i32m4(Y, __riscv_vmul_vx_i32m4(U, -100, vl), vl);
        G = __riscv_vadd_vv_i32m4(G, __riscv_vmul_vx_i32m4(V, -208, vl), vl);
        vint32m4_t B = __riscv_vadd_vv_i32m4(Y, __riscv_vmul_vx_i32m4(U, 516, vl), vl);

        R = __riscv_vsra_vx_i32m4(__riscv_vadd_vx_i32m4(R, 128, vl), 8, vl);
        G = __riscv_vsra_vx_i32m4(__riscv_vadd_vx_i32m4(G, 128, vl), 8, vl);
        B = __riscv_vsra_vx_i32m4(__riscv_vadd_vx_i32m4(B, 128, vl), 8, vl);
        R = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(R, 255, vl), 0, vl);
        G = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(G, 255, vl), 0, vl);
        B = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(B, 255, vl), 0, vl);

        vuint32m4_t o = __riscv_vreinterpret_v_i32m4_u32m4(__riscv_vsll_vx_i32m4(R, 16, vl));
        o = __riscv_vor_vv_u32m4(o, __riscv_vreinterpret_v_i32m4_u32m4(__riscv_vsll_vx_i32m4(G, 8, vl)), vl);
        o = __riscv_vor_vv_u32m4(o, __riscv_vreinterpret_v_i32m4_u32m4(B), vl);
        o = __riscv_vor_vx_u32m4(o, 0xFF000000u, vl);
        __riscv_vse32_v_u32m4(d + x, o, vl);
    }
#else
    int nv = 0;
#endif
    for (int x = nv; x < n; x++) {
        int ux = x & ~1;
        int U = uv[ux] - 128, V = uv[ux + 1] - 128;
        int c = yr[x] - 16, Y = 298 * c;
        int r = (Y + 409 * V + 128) >> 8;
        int g = (Y - 100 * U - 208 * V + 128) >> 8;
        int b = (Y + 516 * U + 128) >> 8;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        d[x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
    }
}

/* 把 fb_id 提交给 CRTC。
 *   wait_ms > 0  = 带 EVENT 提交并等 vblank 事件 (启动探测 / 收尾刷黑, 保证真的生效);
 *   wait_ms == 0 = 纯异步, 不排事件;
 *   wait_ms < 0  = 带 EVENT 提交但**不等** (热路径用: ioctl 只要 ~0.05 ms, 屏在下一个
 *                  vblank 自己换帧, 进程不陪着等 16 ms; 下一轮开头再收这个事件,
 *                  既能确认已上屏, 也避免"上次还没生效就再提交"被驱动判 EBUSY)。
 *   返回 0 = 已提交; -1 = 失败 (调用方退回单缓冲直写语义)。 */
static int do_page_flip(int fd, uint32_t crtc, uint32_t fb_id, int wait_ms)
{
    struct drm_mode_crtc_page_flip pf;
    memset(&pf, 0, sizeof(pf));
    pf.crtc_id = crtc;
    pf.fb_id = fb_id;
    pf.flags = wait_ms != 0 ? DRM_MODE_PAGE_FLIP_EVENT : 0;
    pf.user_data = (uint64_t)fb_id;
    if (ioctl(fd, DRM_IOCTL_MODE_CRTC_PAGE_FLIP, &pf) < 0)
        return -1;
    if (wait_ms > 0) {
        struct pollfd p;
        int pr;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        pr = poll(&p, 1, wait_ms);
        if (pr > 0) {
            struct drm_event_vblank ev;
            if (read(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) { /* 事件读不全不算错 */ }
        }
    }
    return 0;
}

/* ---- 逐帧时间轴日志 (2026-10-28, WFD_FRAME_LOG=<path>) ----
 * 用途: 量端到端延迟 —— UIBC 动作发出时刻 ↔ 屏上画面首次变化时刻。
 * 每帧一行, 带 4x4 分块 Y 均值 (离线算 L1 距离当"画面变了"的判据)。
 * 生产不设这个变量 ⇒ g_frame_log=0, 只多一次判断, 行为不变。 */
static int g_frame_log = 0;
static FILE *g_fp = NULL;
static double g_avgY = 0.0;
static double g_blk[16];

/* ---- 1:1 直贴 (2026-10-28, WFD_1X, 默认开) ----
 * 为什么: 去掉解码器 -A 后, 内容区是 409x720, 屏是 480x800 —— 等比放大只能放到
 *   454x800 (1.11 倍)。这 1.11 倍在单核上很贵: 每行要先 SIMD 转 409 px, 再按
 *   xmap 表展开到 454 (32 万次标量 gather), 实测「转换」从 1.7 ms 涨到 12 ms,
 *   整圈 30 ms ⇒ 只有 32 fps。而且 1.11 倍是**非整倍数**重采样, 字边缘反而更糊。
 * 做法: 内容区按原始像素直接贴到屏中央 (两侧各留 35 px 黑边), 每源行只转一次
 *   + 一次 memcpy —— 像素级无损 (最清晰), CPU 也最省。
 * 代价: 画面宽度 409 而不是 454 (屏高仍占 720/800 = 90%)。
 * 回退: WFD_1X=0 → 老行为 (等比放大铺满宽度)。 */

/* 收一个 flip 完成事件 (最多等 ms)。返回 0 = 收到, -1 = 没有/超时。 */
/* tv_us: 带出内核给的 vblank 时间戳 (us)。0 = 驱动没填, 调用方自己回退。 */
static int recv_flip_event(int fd, int ms, double *tv_us)
{
    struct pollfd p;
    int pr;
    p.fd = fd; p.events = POLLIN; p.revents = 0;
    pr = poll(&p, 1, ms);
    if (pr > 0) {
        struct drm_event_vblank ev;
        if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (tv_us)
                *tv_us = ev.tv_sec ? (double)ev.tv_sec * 1e6 + (double)ev.tv_usec : 0.0;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/dri/card0";
    int W = 480, H = 800, want_cid = 0;
    int max_frames = 0;
    double fps = 0;
    /* 输入像素字节序: bgra = 直接对应 AR24 内存布局 (B,G,R,A), ffmpeg 端用 -pix_fmt bgra;
     * rgba = ffmpeg 的 rgba (R,G,B,A), 需要在这里换 R/B;
     * nv12 = 硬解直通 (Y 平面 + UV 交织), 本程序做 YUV→RGB + 等比缩放 */
    int in_fmt = 0;   /* 0=bgra 1=rgba 2=nv12 */
    int blank_only = 0;   /* -B: 只把屏幕刷成纯黑并常驻 (空闲时占住屏, 防止面板扫到未定义内容发紫) */
    int iw = 0, ih = 0;   /* 输入帧尺寸 (nv12 / 缩放时用), 0 = 跟屏一致 */
    int hdr_mode = 0;     /* -H: 每帧前 16 字节头 (magic NV12/w/h/len), 分辨率自适应 */
    int fill_mode = 0;    /* 0 contain / 1 (-f) cover 居中裁切 / 2 (-S) 拉伸铺满 */
    int crop_mode = 1;    /* 默认自动裁黑边; -C 关闭 */
    const char *fifo_path = NULL;   /* -F <fifo>: 常驻模式, 从命名管道读 (永不退出) */
    int fifo_idle_ms = 800;         /* 常驻模式下多少毫秒没数据就刷黑 (去紫屏的关键) */
    /* ★ 2026-10-29 (「有图像 → 黑屏」的第二个真凶): 会话**已经开始**之后的静默阈值。
     * macOS 的 AirPlay 镜像是「画面有变化才发帧」: 静止桌面实测每 ~30 s 才来一串
     * 30 帧 (正好 1 秒的量), 中间一个字节都没有。原来会话中只把阈值放宽到 3×800
     * = 2.4 s, 于是主人一停手不动鼠标, 屏就按「会话结束」刷黑 —— 表现出来正是
     * 「有图像了, 然后就黑屏」。现在会话中静默只当「暂时没新帧」, 保持最后一帧
     * 最多 fifo_hold_ms (默认 120 s), 真断开 (dec 退出/换源) 仍会在阈值后刷黑。 */
    int fifo_hold_ms = 120000;      /* --hold / WFD_HOLD_MS 可调 */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) want_cid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            const char *f = argv[++i];
            in_fmt = !strcmp(f, "nv12") ? 2 : (!strcmp(f, "rgba") ? 1 : 0);
        }
        else if (!strcmp(argv[i], "-i") && i + 2 < argc) {
            iw = atoi(argv[++i]);
            ih = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "-H")) hdr_mode = 1;
        else if (!strcmp(argv[i], "-f")) fill_mode = 1;
        else if (!strcmp(argv[i], "-S")) fill_mode = 2;   /* stretch: 满屏允许变形 */
        else if (!strcmp(argv[i], "-C")) crop_mode = 0;
        else if (!strcmp(argv[i], "-B") || !strcmp(argv[i], "--blank")) blank_only = 1;
        else if (!strcmp(argv[i], "-F") && i + 1 < argc) fifo_path = argv[++i];
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) fifo_idle_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) fifo_hold_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-?")) {
            fprintf(stderr,
                "用法: %s [-d /dev/dri/card0] [-w 480] [-h 800] [-c <connector>] [-n 帧数]\n"
                "          [-r fps] [-p bgra|rgba|nv12] [-i <输入宽 输入高>] [-f] [-C] [-v]\n"
                "  从 stdin 读帧, 直接显示到 DRM 屏 (AR24)\n"
                "  -w/-h      屏幕尺寸 (默认 480x800)\n"
                "  -i W H     输入帧尺寸 (默认同屏); 与屏不同则**等比缩放+黑边**\n"
                "  -p bgra (默认, = ffmpeg -pix_fmt bgra, 与 AR24 内存序一致)\n"
                "  -p rgba (ffmpeg -pix_fmt rgba, 本程序换 R/B)\n"
                "  -p nv12 (硬解输出 YUV420SP 直通, 本程序 YUV→RGB + 缩放)\n"
                "  -H          stdin 每帧前带 16 字节头 (v4l2_dec -H): 分辨率自动跟随\n"
                "  -f          铺满: 等比 cover 居中裁切 (默认 contain, 会留黑边)\n"
                "  -S          拉伸铺满: 两轴各自缩放到屏尺寸, 不留黑边也不裁内容 (会变形)\n"
                "  -C          关闭自动裁黑边 (NV12 默认会先裁掉四周纯黑再缩放)\n", argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "未知参数: %s (--help 看用法)\n", argv[i]);
            return 2;
        }
    }

    {   /* 环境变量覆盖 (drop-in 里配, 不要改命令行) */
        const char *o = getenv("WFD_HOLD_MS");
        if (o && *o) { int v = atoi(o); if (v >= 0) fifo_hold_ms = v; }
        o = getenv("WFD_IDLE_MS");
        if (o && *o) { int v = atoi(o); if (v >= 0) fifo_idle_ms = v; }
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        ERR("open %s: %s", dev, strerror(errno));
        return 1;
    }

    uint32_t cid = 0, eid = 0, crtc = 0;
    if (find_connector(fd, want_cid, &cid, &eid, &crtc) != 0)
        return 1;

    /* 建 dumb buffer (32bpp → ARGB8888) —— **3 块**, 轮流出图。
     * 单块直写会让屏扫到半新半旧 (见文件头 tearing 说明), 主观就是"最多 25fps + 不稳"。
     * 3 块而不是 2 块: 热路径用**异步** flip (不等 vblank), 所以下一帧要画的那块必须是
     * 两个 flip 之前的, 那时它早已离开扫描线 ⇒ 不可能边写边被扫。 */
    struct drm_mode_create_dumb cd;
    uint32_t fb_id[NBUF_MAX] = { 0 };
    uint8_t *mapb[NBUF_MAX] = { NULL };
    int nbuf = 0;              /* 实际建成的块数 (1 = 建不出更多, 退回直写) */
    memset(&cd, 0, sizeof(cd));
    cd.width = (uint32_t)W;
    cd.height = (uint32_t)H;
    cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        ERR("CREATE_DUMB %dx%d: %s", W, H, strerror(errno));
        return 1;
    }
    for (int b = 0; b < NBUF_MAX; b++) {
        struct drm_mode_create_dumb cd2;
        struct drm_mode_fb_cmd2 fb2;
        struct drm_mode_map_dumb md2;
        uint8_t *m;
        memset(&cd2, 0, sizeof(cd2));
        memset(&fb2, 0, sizeof(fb2));
        memset(&md2, 0, sizeof(md2));
        if (b == 0) {
            cd2 = cd;                       /* 第 1 块复用上面已建好的 */
        } else {
            cd2.width = (uint32_t)W;
            cd2.height = (uint32_t)H;
            cd2.bpp = 32;
            if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd2) < 0) {
                LOG("⚠ 第 %d 块 dumb buffer 建不出来 (%s) → 停在 %d 块", b + 1, strerror(errno), nbuf);
                break;
            }
        }
        fb2.width = (uint32_t)W;
        fb2.height = (uint32_t)H;
        fb2.pixel_format = FORMAT_AR24;
        fb2.handles[0] = cd2.handle;
        fb2.pitches[0] = cd2.pitch;
        if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0) {
            LOG("⚠ 第 %d 块 ADDFB2 失败 (%s) → 停在 %d 块", b + 1, strerror(errno), nbuf);
            break;
        }
        md2.handle = cd2.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md2) < 0) {
            LOG("⚠ 第 %d 块 MAP_DUMB 失败 (%s) → 停在 %d 块", b + 1, strerror(errno), nbuf);
            break;
        }
        m = mmap(NULL, cd2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md2.offset);
        if (m == MAP_FAILED) {
            LOG("⚠ 第 %d 块 mmap 失败 (%s) → 停在 %d 块", b + 1, strerror(errno), nbuf);
            break;
        }
        fb_id[b] = fb2.fb_id;
        mapb[b] = m;
        nbuf = b + 1;
    }
    if (nbuf == 0) { ERR("一块 buffer 都没建成"); return 1; }
    uint8_t *map = mapb[0];
    if (g_verbose)
        LOG("dumb: handle=%u pitch=%u size=%llu, %d 块%s", cd.handle, cd.pitch,
            (unsigned long long)cd.size, nbuf, nbuf >= 2 ? " (双缓冲 flip)" : " (单缓冲)");

    /* ⚠ 必须填**不透明**黑 (A=0xFF): AR24 的 alpha 参与平面混合, 填 0 等于全透明,
     * 面板就扫到未定义内容 ⇒ 空闲整屏紫色、投屏时绘制区在"已画/未画"之间闪烁紫色
     * (2026-10-25 实测: 直接 dump scanout 缓冲, alpha 全是 0)。 */
    for (int i = 0; i < nbuf; i++) {
        uint32_t *_q = (uint32_t *)mapb[i];
        for (size_t _j = 0; _j < cd.size / 4; _j++) _q[_j] = 0xFF000000u;
    }

    /* 点屏: connector 的模式表里没有现成 mode 时, 按 WxH 手搓一个 */
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
        /* 四个指针必须全给真内存, 否则 kernel 写 NULL 指针 → EFAULT */
        q.encoders_ptr = (uint64_t)(uintptr_t)qencs;
        q.modes_ptr = (uint64_t)(uintptr_t)modes;
        q.props_ptr = (uint64_t)(uintptr_t)props;
        q.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &q) < 0)
            ERR("GETCONNECTOR(模式表): %s", strerror(errno));
        else {
            for (uint32_t i = 0; i < q.count_modes; i++) {
                if (g_verbose)
                    LOG("mode[%u] %s %ux%u@%u", i, modes[i].name,
                        modes[i].hdisplay, modes[i].vdisplay, modes[i].vrefresh);
                if (modes[i].hdisplay == (uint16_t)W && modes[i].vdisplay == (uint16_t)H) {
                    mode = modes[i];
                    have_mode = 1;
                    break;
                }
            }
            if (!have_mode) { mode = modes[0]; have_mode = 1; }
        }
        free(modes);
        free(qencs);
        free(props);
        free(pvals);
    }
    if (!have_mode) {
        mode.hdisplay = (uint16_t)W;
        mode.vdisplay = (uint16_t)H;
        mode.vrefresh = 60;
        snprintf(mode.name, sizeof(mode.name), "%dx%d", W, H);
        ERR("connector 无 mode 表, 用默认 %dx%d", W, H);
    }

    struct drm_mode_crtc sc;
    memset(&sc, 0, sizeof(sc));
    uint32_t one = cid;
    sc.set_connectors_ptr = (uint64_t)(uintptr_t)&one;
    sc.count_connectors = 1;
    sc.crtc_id = crtc;
    sc.fb_id = fb_id[0];
    sc.mode = mode;
    sc.mode_valid = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
        ERR("SETCRTC(crtc=%u conn=%u): %s", crtc, cid, strerror(errno));
        return 1;
    }
    LOG("✅ 上屏: connector %u crtc %u %s %ux%u@%u AR24 (pitch %u)",
        cid, crtc, mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh, cd.pitch);

    /* 探测 PAGE_FLIP 是否可用 (部分 BSP 的 DRM 没实现 atomic/flip):
     *   现在显示的是 fb_id[0], 试着 flip 到 fb_id[1] 再翻回来, 都成功才启用双缓冲。 */
    int flip_ok = 0;
    if (nbuf >= 2) {
        if (do_page_flip(fd, crtc, fb_id[1], 100) == 0 &&
            do_page_flip(fd, crtc, fb_id[0], 100) == 0) {
            flip_ok = 1;
        } else {
            LOG("⚠ PAGE_FLIP 不可用 (%s) → 退回单缓冲直写 (会有撕裂)", strerror(errno));
        }
    } else {
        LOG("⚠ 只有一块 buffer → 单缓冲直写 (会有撕裂)");
    }
    if (flip_ok)
        LOG("🎞 多缓冲 + PAGE_FLIP 已启用: %d 块轮转, 异步提交 (屏只会扫到完整帧)", nbuf);
    int cur = 0;   /* 当前**正在显示**的那块 */

    /* 方案A: NV12 直上视频层 (建不成/不通就自动退回 CPU 路径) */
    struct vo_nv12 vo;
    int vo_path = 0;
    vo_init(fd, &vo, crtc, W, H, nbuf, fb_id[0]);

    if (blank_only) {
        /* -B: 缓冲区建出来时已 memset(0)=全黑, 这里常驻即可。
         * 用途: 空闲/开机时占住 CRTC, 否则平面被拆掉后面板会扫到未定义内容
         * (现场表现 = 全屏紫色), 投屏起来后再由正常 drmplay 接管。 */
        LOG("  -B 黑屏常驻 (占屏防紫, 被 kill 或正常 drmplay 接管后退出)");
        for (;;) pause();
    }

    /* ---- 常驻模式 (-F): 自己打开命名管道的读写两端 ----
     * 2026-10-26 紫屏真因: 上屏进程跟着管线一起起落, 每段空窗期 CRTC 上没有 plane,
     * 面板扫未定义显存 = 全屏紫色 (现场: 开机一段紫 / 每次投屏起来前闪一下紫)。
     * 改成常驻后, 这唯一一个进程从开机持有 DRM master 直到关机:
     *   · O_RDWR 同时持有读写端 ⇒ 写端 (v4l2_dec) 退出也不会 EOF, read 只会 EAGAIN
     *   · 空闲 fifo_idle_ms 没数据 → 刷黑, 不退出
     * 代价: 它必须能被"和平接管" (kill 后自启脚本会再拉起), 见 wfd-p2p.sh。 */
    int fifo_mode = 0;
    if (fifo_path) {
        mkfifo(fifo_path, 0666);
        int ffd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (ffd < 0) {
            ERR("打开常驻管道 %s: %s (退化为读 stdin)", fifo_path, strerror(errno));
        } else {
            if (ffd != STDIN_FILENO) {
                dup2(ffd, STDIN_FILENO);
                close(ffd);
            }
            fifo_mode = 1;
            LOG("常驻模式: %s (首个会话前空闲 %d ms 刷黑; AirPlay 客户端在 (airplay-active / airplay-cast-active) 时无限保持最后一帧; 其它源静默 %d ms 即刷黑, 进程不倒)",
                fifo_path, fifo_idle_ms, fifo_idle_ms * 3);
        }
    }
    if (fifo_mode) {
        /* 开机第一时间就是黑的 —— 缓冲建出来本来就 memset(0), 这里再显式刷一次
         * (两块都刷, 否则翻到还没刷过的那块会闪一下旧内容) */
        for (int _b = 0; _b < nbuf; _b++) {
            for (int y = 0; y < H; y++) {
                uint32_t *_q = (uint32_t *)(mapb[_b] + (size_t)y * cd.pitch);
                for (int _x = 0; _x < W; _x++) _q[_x] = 0xFF000000u;
            }
        }
    }

    /* ---- 输入帧尺寸 / 缩放几何 (默认整帧等比居中; NV12 首帧再自动裁黑边) ---- */
    int sw = iw > 0 ? iw : W;
    int sh = ih > 0 ? ih : H;
    /* 2026-10-29 横屏自动旋转: sw/sh 是"转完之后"的尺寸 (下游一律用它),
     * sw_raw/sh_raw 是管道里真实的帧尺寸 (读帧体、转置源头用)。 */
    int sw_raw = sw, sh_raw = sh;
    int rot_deg = 0;           /* 0 = 不转, 90 = 顺时针转 90 度 */
    int rot_pending = 1;       /* 还没用"内容 bbox"定过朝向 (第一帧 / 换分辨率后 / 换源后) */
    double t_last_frame = 0;   /* 上一帧帧头时刻 (ms), 用来识别「换源」那段静默 */
    uint8_t *rotbuf = NULL;    /* 转置目标 (只用 heap, 不走共享内存) */
    size_t rot_cap = 0;
    uint8_t *convbuf = NULL;   /* 非 NV12 源 → NV12 的转换目标 (2026-11-04) */
    size_t conv_cap = 0;
    int src_fmt = in_fmt;      /* 源格式 (固定); in_fmt 首帧转换后统一成 2=NV12 */
    size_t frame = (in_fmt == 2) ? (size_t)sw * sh * 3 / 2 : (size_t)sw * sh * 4;

    int cx = 0, cy = 0, cw = sw, ch = sh;   /* 源裁剪矩形, 默认整帧 */
    int dw, dh, ox, oy, scaled;
    geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);

    uint8_t *buf = malloc(frame);
    uint8_t *buf_heap = buf;   /* buf 可能被指向共享内存槽, 堆指针单独留着释放 */
    if (!buf) { ERR("malloc 帧缓冲失败"); return 1; }
    size_t buf_cap = frame;

    LOG("输入: %s %dx%d → 屏 %dx%d (绘制区 %dx%d @ %d,%d%s)",
        in_fmt == 2 ? "NV12" : (in_fmt == 1 ? "RGBA" : "BGRA"),
        sw, sh, W, H, dw, dh, ox, oy, scaled ? ", 已缩放" : "");

    /* 2026-10-26 实测 (真机 720p30): 每帧上屏 20~24 ms, 其中「读管道」20.5~22 ms,
     * 而转换只要 0.2 ms —— 瓶颈是 pipe 默认 64 KB 缓冲: 720p 一帧 1.38 MB 要
     * 在 dec/play 之间来回 21 趟, 单核上每趟都是一次上下文切换。
     * 这里先用 -w/-h 估一个 (常驻模式下首帧可能是几十秒后才来); 帧头报出真实帧长时
     * 会按真实长度再定一次容 (见 set_pipe_cap 的注释)。 */
    set_pipe_cap(frame, 1);

    shm_map();   /* WFD_SHM 没设就什么都不做 (走老管道路径) */

    /* 1:1 直贴开关 (默认开; WFD_1X=0 回到等比放大铺满宽度) */
    {
        const char *e = getenv("WFD_1X");
        if (e && *e) g_1x = atoi(e);
        e = getenv("WFD_NOSTORE");
        if (e && *e) g_nostore = atoi(e);
        LOG("绘制模式: %s (WFD_1X=%d)", g_1x ? "1:1 原始像素直贴" : "等比放大", g_1x);
        /* WFD_ROT: auto (默认) / 0 (不转) / 90 (强制转) */
        e = getenv("WFD_ROT");
        if (e && *e) {
            if (!strcmp(e, "auto")) g_rot_env = -1;
            else g_rot_env = (atoi(e) >= 45) ? 90 : 0;
        }
        LOG("朝向自适应: %s (WFD_ROT=%s)", g_rot_env < 0 ? "自动 (按内容宽高比)" :
            (g_rot_env == 90 ? "强制转 90 度" : "强制不转"),
            g_rot_env < 0 ? "auto" : (g_rot_env == 90 ? "90" : "0"));
    }

    /* 逐帧日志 (取证用, 生产不设) */
    {
        const char *fl = getenv("WFD_FRAME_LOG");
        if (fl && *fl) {
            g_fp = fopen(fl, "a");
            if (g_fp) {
                g_frame_log = 1;
                LOG("逐帧日志: %s (每帧一行, 含 4x4 分块 Y 均值)", fl);
            } else {
                LOG("⚠ 逐帧日志 %s 打不开 (%s)", fl, strerror(errno));
            }
        }
    }

    long n = 0;
    uint32_t f_stamp = 0;      /* 帧时刻 (解码完写在帧头 12~15 字节), 用来量管道延迟 */
    double lat_sum = 0, lat_max = 0;
    double t_flip = 0;         /* 每帧 flip 等待 (含 vblank), 单独计量便于判断是否被锁到 60Hz */
    uint64_t prev_hash = 0;    /* 上一帧 Y 平面抽样哈希 (内容变化检测) */
    long uniq = 0;             /* 内容与上一帧不同的帧数 (真实画面帧率分子) */
    long n_flip = 0;           /* 本统计窗口内成功 flip 的次数 (上屏帧率分子) */
    /* 2026-10-27 现场取证: 手机编码器跑不动时会用**重复帧/skip 帧**填满 RTP 时间轴,
     * 这类帧的帧体极小 (几百字节), 正常帧几 KB~几十 KB ⇒ 帧体大小的双峰分布
     * 就是"源端没真出 60 张画面"的铁证。帧间隔则量源端真实节拍 (16.67 ms = 60fps)。 */
    uint32_t fsz_min = 0, fsz_max = 0;
    double fsz_sum = 0;
    double gap_sum = 0, gap_max = 0;
    long gap_n = 0;
    struct timespec t_prev_frame;
    int have_prev_frame = 0;
    long n_flip_fail = 0;      /* flip 被驱动拒的次数 (EBUSY = 上一帧还没生效) */
    int flip_pending = 0;      /* 已提交但还没收到 vblank 完成事件 */
    int lat_n = 0;
    int redo_crop = 1;    /* 分辨率变化后立刻重测黑边 */
    int need_clear = 1;   /* 只在几何变化时清一次黑底: 每帧都清会让画面黑↔正常闪 */
    int *xmap = NULL;     /* dx → 源列, 避免逐像素整数除法 (K230 上省不少) */
    int xmap_cap = 0;
    /* 2026-10-26: 上屏是单核最大开销 (实测 9.7 ms/帧 = 64% CPU, 而解码只要 2.1 ms/帧)。
     * 原来对**每个输出像素**都重做一次 NV12→ARGB, 但放大 1.5~2 倍时同一源像素会被
     * 重复算 1.5~2 次, 纯浪费。改为两级行缓存:
     *   line[]  = 当前源行转好的 ARGB (cw 个, 色彩变换只做一次)
     *   eline[] = 该源行水平展开到输出宽度后的结果 (dw 个, 供重复输出行直接 memcpy)
     * 每帧色彩变换次数从 dw*dh 降到 min(dh,ch)*cw。 */
    uint32_t *eline = NULL, *line = NULL;
    int eline_cap = 0, line_cap = 0;
    int line_sry = -1, line_cx = -1, line_cw = -1;
    int eline_sry = -1;
    int eline_cx = -1, eline_cw = -1, eline_dw = -1;   /* eline 的有效性 key */
    long eline_hits = 0, eline_miss = 0;
    /* 分段计时 (us): 读管道 / 裁黑边探测 / 转换+展开 / 合计 */
    double t_read = 0, t_crop = 0, t_conv = 0, t_iter = 0;
    /* 2026-10-27 v12 记账修正 (关键): 原来「读管道」= ta(圈首) → tb(读完),
     * 而 ta 在 recv_flip_event **之前** ⇒ 这个数里混进了「等上一帧 vblank」的时间,
     * 看起来 12 ms 像管道拷贝慢, 其实大部分是在等屏。现在拆成三项:
     *   t_vwait = 等上一帧 flip 完成 (vblank) | t_read = 纯管道拷贝 | 转换/裁边/flip 不变
     * 另外记每帧进程 CPU (CLOCK_PROCESS_CPUTIME_ID) —— 这是「60fps 预算 16.67 ms 还剩多少余量」
     * 唯一可信的口径 (整圈被 vblank 锁死, 永远是 16.7, 看不出余量)。 */
    double t_vwait = 0;
    struct timespec cpu_prev;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_prev);
    struct timespec ta, tb, ti0, ti1;
    int px = -1, py = -1, pw = -1, ph = -1;   /* 待确认的裁剪候选 (防误裁) */
    long crop_hold_until = 0;                 /* 几何刚变过 → 到这一帧之前不再动 (防抖) */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int idle_black = 0;       /* 已刷黑 (只在状态切换时刷一次, 别每轮都刷) */
    long session = 0;         /* 第几个会话 (空闲一次 +1), 只用于日志 */
    /* 2026-10-27 v11: 显示节拍探针 —— 上屏 flip 到底是不是每 16.67 ms 一次。
     *   源端 60fps 时如果每帧都隔一个 vblank 上屏, 主观才叫流畅; 一旦出现 33 ms,
     *   屏上就是「同一张画面停了两帧」⇒ 卡顿。这是之前所有统计都漏掉的一维。 */
    double flip_tv = 0;       /* 本帧 flip 生效的 vblank 时刻 (us) */
    double disp_prev = 0;     /* 上一次上屏时刻 (us) */
    long disp_0 = 0;          /* 间隔 < 8 ms    ⇒ 和上一帧抢同一个 vblank (那张没上屏) */
    long disp_1 = 0;          /* 8 ~ 24 ms      ⇒ 1 个 vblank */
    long disp_2 = 0;          /* 24 ~ 41 ms    ⇒ 2 个 vblank (卡一下) */
    long disp_3 = 0;          /* > 41 ms       ⇒ 3+ 个 vblank (明显卡) */
    double disp_max = 0;
    double disp_win_start = 0;   /* 本统计窗口第一个上屏时刻, 只用来算实测 fps */

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &ti0);
        clock_gettime(CLOCK_MONOTONIC, &ta);
        /* 先收上一帧那个 flip 的完成事件 (它通常在上一圈 13 ms 的转换里就已经到了,
         * 正常只花 0~1 ms)。不等就提交会怎样: 上一次 flip 还没在 vblank 生效时再提交,
         * 驱动直接 EBUSY → 那一帧没上屏 (实测每 60 帧丢 3~7 帧)。 */
        if (flip_pending) {
            double t_recv = 0;
            struct timespec tv0, tv1;
            clock_gettime(CLOCK_MONOTONIC, &tv0);
            if (recv_flip_event(fd, 40, &flip_tv) == 0) {
                flip_pending = 0;
                if (flip_tv <= 0) {   /* 驱动没填 vblank 时间戳: 用收到事件的时刻近似 */
                    struct timespec tr;
                    clock_gettime(CLOCK_MONOTONIC, &tr);
                    t_recv = (double)tr.tv_sec * 1e6 + (double)tr.tv_nsec / 1e3;
                    flip_tv = t_recv;
                }
                if (disp_prev > 0) {
                    double di = (flip_tv - disp_prev) / 1000.0;   /* ms */
                    if (di < 0) di = 0;
                    if (di > disp_max) disp_max = di;
                    if (di < 8.0) disp_0++;
                    else if (di <= 24.0) disp_1++;
                    else if (di <= 41.0) disp_2++;
                    else disp_3++;
                }
                if (disp_win_start <= 0) disp_win_start = flip_tv;
                disp_prev = flip_tv;
            }
            clock_gettime(CLOCK_MONOTONIC, &tv1);
            t_vwait += (double)(tv1.tv_sec - tv0.tv_sec) * 1e6 +
                       (double)(tv1.tv_nsec - tv0.tv_nsec) / 1e3;
        }
        /* 从这一刻起才是「真干活」: 读管道 + 裁边 + 转换 + flip */
        clock_gettime(CLOCK_MONOTONIC, &ta);
        /* 多缓冲: 本帧往"两个 flip 之前那块"画 (必定已离开扫描线), 画完异步 flip。
         * 单缓冲时 map 恒定 (mapb[0]), 保持老行为。 */
        int use_shm = 0;   /* 本帧帧体是否在共享内存 (帧头 len 最高位标志) */
        uint32_t shm_seq = 0;
        int nxt = flip_ok ? (cur + 1) % nbuf : 0;
        if (flip_ok) map = mapb[nxt];
        /* 帧体永远读进堆缓冲: 上一帧可能把它指向过旋转缓冲, 不重置就会自己覆盖自己 */
        buf = buf_heap;
        /* -H: 先读 16 字节帧头 (v4l2_dec -H 写的), 分辨率变了就重算缩放几何 */
        if (hdr_mode) {
            unsigned char hd[16];
            int rr;
            /* 2026-10-28: 会话**中途**的短暂停顿不要立刻刷黑。
             * 实测 source 偶发停推 0.8~2.5 s (编码器重启 / WiFi 省电 / 手机卡顿), 老逻辑
             * 停 800 ms 就刷黑 + 丢弃帧头对齐, 恢复时还要 resync ⇒ 主人看到「闪黑 + 卡 2.5 s」。
             * 已经进过会话 (n>0) 时把容忍度放宽到 3×, 画面就停在最后一帧, 不闪。
             * 真正的断开 (source 退出) 靠 sink 侧驱动 dec 关管道 + RTSP 结束, 仍会走 idle。 */
            {
                int wms = fifo_mode ? fifo_idle_ms : 0;
                if (wms > 0 && n > 0) {
                    /* 会话中静默怎么处理, 按源区分 (2026-10-29 16:5x 修「断开后屏不黑」):
                     *   · AirPlay: macOS/iOS 静止桌面本来就不发帧 ⇒ 客户端连着 (airplay-active / airplay-cast-active)
                     *     就无限期保持最后一帧 (wms=0 阻塞), 由看护在断开时删标记;
                     *   · Miracast: 手机断开不一定马上关 TCP, 但**不会再发帧** ⇒ 按 3×idle
                     *     (2.4 s) 判会话结束刷黑 —— 既盖住实测 0.8~2.5 s 的源端抖动, 又能在
                     *     断开后 2~3 s 内变黑。绝不能给 Miracast 用 120 s, 否则断开后屏一直
                     *     停在上一帧 (主人 2026-10-29 现场报的就是这个)。 */
                    if (any_airplay_active())
                        wms = 0;
                    else
                        wms = fifo_idle_ms * 3;
                }
                rr = stream_read(STDIN_FILENO, hd, sizeof(hd), wms);
            }
            if (rr == 0) goto idle;            /* 常驻模式: 没数据 = 会话没开始/已结束 */
            if (rr < 0) {
                if (fifo_mode) goto idle;
                goto done;
            }
            if (memcmp(hd, "NV12", 4) != 0) {
                if (fifo_mode) {
                    /* 会话被打断 / dec 卡住时管道里留下半帧 → 帧头错位。
                     * 原来「排空就回 idle」在 dec 还在写时会自我维持错位循环
                     * (现场连刷几十条「已丢弃 77xxxx 字节」) ⇒ 改成滑动找 magic 就地续上。 */
                    if (resync_header(STDIN_FILENO, hd, n > 0 ? fifo_hold_ms : fifo_idle_ms) <= 0)
                        goto idle;
                    if (memcmp(hd, "NV12", 4) != 0)
                        goto idle;   /* 兜底 */
                } else {
                    ERR("帧头 magic 不对 (不是 v4l2_dec -H 的输出)");
                    goto done;
                }
            }
            /* 2026-10-29: 换源但分辨率不变时, 上面「输入变为」不触发, 朝向会沿用上一场
             * 的结论 —— 实测 AirPlay(Mac 1280x720 满幅) 之后换 Miracast(努比亚同容器)
             * 手机画面被当横屏转了 90 度。源切换在管道上必然有一段静默 (dec 重启),
             * 所以「距上一帧 >1 s 后的第一帧」就重判一次朝向。同一源静止留帧不受影响,
             * 因为重判结果和原来一样, 几何不变 (无重绘、无闪)。 */
            {
                struct timespec tn;
                double tnw;
                clock_gettime(CLOCK_MONOTONIC, &tn);
                tnw = (double)tn.tv_sec * 1e3 + (double)tn.tv_nsec / 1e6;
                if (n > 0 && t_last_frame > 0 && tnw - t_last_frame > 1000.0)
                    rot_pending = 1;
                t_last_frame = tnw;
            }
            int nw = hd[4] | (hd[5] << 8), nh = hd[6] | (hd[7] << 8);
            uint32_t nl = (uint32_t)hd[8] | ((uint32_t)hd[9] << 8) |
                          ((uint32_t)hd[10] << 16) | ((uint32_t)hd[11] << 24);
            f_stamp = (uint32_t)hd[12] | ((uint32_t)hd[13] << 8) |
                      ((uint32_t)hd[14] << 16) | ((uint32_t)hd[15] << 24);
            if (nw > 0 && nh > 0 && (nw != sw_raw || nh != sh_raw)) {
                sw_raw = nw; sh_raw = nh;
                /* 朝向: 先按"容器宽高比"给个初值 (第一帧就有正确几何, 不闪),
                 * 帧体读进来后再用内容 bbox 复核一次 (手机常把竖屏塞进横屏容器)。 */
                rot_pending = 1;
                rot_deg = pick_rot(sw_raw, sh_raw, W, H);
                sw = rot_deg ? sh_raw : sw_raw;
                sh = rot_deg ? sw_raw : sh_raw;
                cx = 0; cy = 0; cw = sw; ch = sh;   /* 新分辨率先按整帧, 下面重测黑边 */
                geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                need_clear = 1;
                LOG("输入变为 %dx%d%s → 绘制区 %dx%d @ %d,%d%s", sw_raw, sh_raw,
                    rot_deg ? " (横屏内容, 转 90 度后)" : "", dw, dh, ox, oy,
                    scaled ? ", 已缩放" : "");
                redo_crop = 1;
                px = -1; py = -1; pw = -1; ph = -1;
            }
            if (nl > 0) {
                /* 最高位 = 帧体在共享内存 (此时 len 低位才是真实帧长, 管道里跟着 4 B 序号) */
                use_shm = (nl & 0x80000000u) ? 1 : 0;
                nl &= 0x7FFFFFFFu;
                if (use_shm && (!g_shm_base || (size_t)nl > g_shm_slot)) {
                    ERR("共享内存帧: len=%u 槽=%zu 基址=%p → 无法取帧", nl, g_shm_slot,
                        (void *)g_shm_base);
                    goto done;
                }
                size_t frame_old = frame;
                frame = nl;
                /* 2026-10-24: 原判据 (frame > sw*sh*4) 对 NV12 永远不成立 →
                 * 帧头从 480x800 变 1920x1080 时没扩容, read() 写越界 → EFAULT
                 * (现场: "read: Bad address", 0 帧)。按实际容量扩容。 */
                /* ★ 2026-10-29 (黑屏真凶): realloc 之后**必须把 buf 也指向新块**。
                 * 原来只更新 buf_heap, buf 还停在 1111 行 malloc 出来的老地址上:
                 *   分辨率从 480x800 换到 1280x720 时帧长 576000 → 1382400, realloc
                 *   走 mremap 把老块搬走/放掉 (glibc 对 >128 KB 的块用 mmap), buf 立刻
                 *   变成悬空指针, 紧接着 stream_read(buf, 1382400) 往已 unmap 的页里写。
                 *   实机 oops: epc 在 stream_read 的 sb 上, badaddr = 老块 base+0x10
                 *   (exactly malloc 元数据偏移), cause 0xf (store page fault)。
                 *   这正是「有图像 → 黑屏」: 480x800 那帧画出来了, 一切到 1280x720
                 *   就死, 死了就没人读管道, v4l2_dec 等 10 s 无读者后放弃。
                 *   合成码流 (480x800 一帧 → 1280x720 一帧) 100% 复现 rc=139。 */
                if (!use_shm && frame > buf_cap) {
                    uint8_t *nb = realloc(buf_heap, frame);
                    if (!nb) { ERR("realloc 帧缓冲失败"); goto done; }
                    buf_heap = nb;
                    buf_cap = frame;
                }
                if (!use_shm) buf = buf_heap;   /* 非共享内存帧: 目标永远是堆缓冲 */
                /* 真实帧长和启动时估算的不一样 ⇒ 按真实帧长重定管道容量
                 * (否则 194 KB 的帧会顶着 1 MB 管道 = 5.4 帧 = +90 ms 延迟) */
                if (frame != frame_old)
                    set_pipe_cap(frame, 1);
            }

        }

        if (use_shm) {
            /* 零拷贝: 管道里只有 4 字节序号, 帧体在共享内存槽 (slot = seq & 1) */
            uint32_t seq = 0;
            int rr = stream_read(STDIN_FILENO, (uint8_t *)&seq, 4, 0);
            if (rr < 0) {
                if (fifo_mode) goto idle;
                goto done;
            }
            buf = g_shm_base + SHM_HEAD + (size_t)(seq & 1) * g_shm_slot;
            shm_seq = seq;
        } else {
            int rr = stream_read(STDIN_FILENO, buf, frame, 0);
            if (rr < 0) {
                if (fifo_mode) goto idle;   /* 半帧就断了: 丢弃, 回空闲态 */
                goto done;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &tb);
        t_read += (tb.tv_sec - ta.tv_sec) * 1e6 + (tb.tv_nsec - ta.tv_nsec) / 1e3;

        /* ---- 统一到 NV12 (2026-11-04, 主人: 只维护新路) ----
         * 显示侧只剩一条路: NV12 → VO 视频层 (硬件转色)。非 NV12 源 (bgra/rgba,
         * 只有老的 ffmpeg 直喂场景) 在这里转一次; 硬件解码器本来就出 NV12 ⇒ 不跑。 */
        if (src_fmt != 2) {
            size_t need = (size_t)sw_raw * sh_raw * 3 / 2;
            if (need > conv_cap) {
                uint8_t *nc = realloc(convbuf, need);
                if (!nc) { ERR("NV12 转换缓冲分配失败 (%zu B)", need); goto done; }
                convbuf = nc;
                conv_cap = need;
            }
            bgra_to_nv12(convbuf, buf, sw_raw, sh_raw, src_fmt == 1);
            buf = convbuf;
            in_fmt = 2;
            static int conv_logged;
            if (!conv_logged) {
                conv_logged = 1;
                LOG("输入 %s → NV12 (CPU 转一次, 之后只走 VO 视频层)",
                    src_fmt == 1 ? "RGBA" : "BGRA");
            }
        }

        /* ---- 朝向判定 + 横屏整帧转置 (2026-10-29) ----
         * 判定用**内容 bbox** 而不是容器尺寸: 手机投屏常把竖屏画面塞进 1920x1080,
         * 只看容器会把手机也转过去。判一次就锁定 (换分辨率时重判)。 */
        if (in_fmt == 2 && (rot_pending || rot_deg)) {
            if (rot_pending) {
                int bx, by, bw, bh, r;
                int ewc = sw_raw, ehc = sh_raw;
                rot_pending = 0;
                if (detect_bbox_nv12(buf, sw_raw, sh_raw, &bx, &by, &bw, &bh)) {
                    ewc = bw; ehc = bh;
                }
                r = pick_rot(ewc, ehc, W, H);
                if (r != rot_deg) {
                    rot_deg = r;
                    sw = rot_deg ? sh_raw : sw_raw;
                    sh = rot_deg ? sw_raw : sh_raw;
                    cx = 0; cy = 0; cw = sw; ch = sh;
                    geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                    need_clear = 1;
                    redo_crop = 1;
                    px = -1; py = -1; pw = -1; ph = -1;
                    LOG("朝向: 内容 %dx%d → 屏 %dx%d ⇒ %s (绘制区 %dx%d @ %d,%d)",
                        ewc, ehc, W, H, rot_deg ? "转 90 度" : "不转", dw, dh, ox, oy);
                }
            }
            if (rot_deg) {
                size_t need = (size_t)sw_raw * sh_raw * 3 / 2;
                static int rot_logged;
                if (need > rot_cap) {
                    uint8_t *nm = realloc(rotbuf, need);
                    if (!nm) { ERR("旋转缓冲分配失败 (%zu B)", need); goto done; }
                    rotbuf = nm;
                    rot_cap = need;
                }
                rot90_cw_nv12(buf, rotbuf, sw_raw, sh_raw);
                buf = rotbuf;
                if (!rot_logged) {
                    rot_logged = 1;
                    LOG("旋转: %dx%d → %dx%d (NV12 分块转置, 每帧一次)", sw_raw, sh_raw, sw, sh);
                }
            }
        }

        /* 帧体大小 + 帧到达间隔 (源端真实节拍) */
        {
            uint32_t fsz = (uint32_t)frame;
            if (fsz_min == 0 || fsz < fsz_min) fsz_min = fsz;
            if (fsz > fsz_max) fsz_max = fsz;
            fsz_sum += fsz;
        }
        if (have_prev_frame) {
            double gap = (double)(tb.tv_sec - t_prev_frame.tv_sec) * 1e3 +
                         (double)(tb.tv_nsec - t_prev_frame.tv_nsec) / 1e6;
            /* 会话刚起 / 断流重连会有一个巨大的间隔, 别让它污染均值 */
            if (gap < 200.0) {
                gap_sum += gap;
                if (gap > gap_max) gap_max = gap;
                gap_n++;
            }
        }
        t_prev_frame = tb;
        have_prev_frame = 1;

        /* ---- 内容变化检测 (2026-10-27, 只为判断"60fps 是不是真的 60 帧不同画面") ----
         * 手机 Miracast 会在编码器跑不动时用**重复帧**把 RTP 时间轴填满: sink 的
         * frames/s 和本进程的整圈都还是 60, 但屏上真正变的只有 25 帧 ⇒ 主观"只有
         * 25fps"。抽样 (每 8 行 × 每 8 列) 算 Y 平面哈希, 成本 ~8k 次比较, 可忽略。 */
        if (in_fmt == 2 && sw > 8 && sh > 8) {
            uint64_t h = 1469598103934665603ULL;
            /* 抽样步长随源宽自适应 (2026-10-27): 原来固定 8, 源从 960 宽换成 480 宽
             * (v4l2_dec -A 改 1×屏) 后采样点只剩 1/4, 同一个画面哈希更容易撞上 ⇒
             * "内容变化" 会虚高。按"约 120 列 × 68 行"定步长, 换分辨率后指标仍可比。 */
            int st = sw >= 640 ? 8 : 4;
            long ysum = 0, ycnt = 0;
            /* 4x4 分块均值 (2026-10-28): 判"画面变了"要用**分块 L1 距离**而不是全局
             * 均值/哈希 —— 全局均值对滚动列表几乎没反应 (深色 UI 滚动前后平均亮度
             * 只差零点几), 而哈希对编码噪声过敏 (静止时每帧都在变) ⇒ 两者都不能量延迟。
             * 分块均值两头都稳: 静止抖动 < 0.4, 真实变化 L1 能到 7~300。 */
            long bsum[16], bcnt[16];
            for (int i = 0; i < 16; i++) { bsum[i] = 0; bcnt[i] = 0; }
            for (int y = 0; y < sh; y += st) {
                const uint8_t *r = buf + (size_t)y * sw;
                int by = y * 4 / (sh > 0 ? sh : 1);
                if (by > 3) by = 3;
                for (int x = 0; x < sw; x += st) {
                    h ^= r[x];
                    h *= 1099511628211ULL;
                    ysum += r[x];
                    ycnt++;
                    int bx = x * 4 / (sw > 0 ? sw : 1);
                    if (bx > 3) bx = 3;
                    bsum[by * 4 + bx] += r[x];
                    bcnt[by * 4 + bx]++;
                }
            }
            if (ycnt) g_avgY = (double)ysum / (double)ycnt;
            for (int i = 0; i < 16; i++)
                g_blk[i] = bcnt[i] ? (double)bsum[i] / (double)bcnt[i] : 0.0;
            if (h != prev_hash) {
                uniq++;
                prev_hash = h;
            }
            if (g_frame_log) {
                struct timespec tl;
                clock_gettime(CLOCK_MONOTONIC, &tl);
                FILE *o = g_fp ? g_fp : stderr;
                fprintf(o, "[FRAME] t=%lld.%03ld h=%016llx stamp=%u chg=%d y=%.2f "
                           "res=%dx%d g=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
                           "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                        (long long)tl.tv_sec, tl.tv_nsec / 1000000,
                        (unsigned long long)h, f_stamp, h != prev_hash, g_avgY, sw, sh,
                        g_blk[0], g_blk[1], g_blk[2], g_blk[3],
                        g_blk[4], g_blk[5], g_blk[6], g_blk[7],
                        g_blk[8], g_blk[9], g_blk[10], g_blk[11],
                        g_blk[12], g_blk[13], g_blk[14], g_blk[15]);
                if (g_fp)
                    fflush(g_fp);
            }
        }
        /* ---- 自动裁黑边 (仅 NV12 + 未 -C): 开头每 5 帧重测 (首帧可能还没解好),
         *      稳定后每 60 帧复核一次 ---- */
        if (crop_mode && in_fmt == 2 &&
            (n == 0 || redo_crop || n % (n < 20 ? 5 : 150) == 0)) {
            /* 2026-10-28: 探测本身要扫 1280x720 的 Y (每 4px/2px 采样 = 11.5 万次),
             * 实测平均 1.4 ms/帧 —— 对 60 fps 的 16.67 ms 预算是硬伤。开播前 20 帧
             * 每 5 帧探一次 (快速锁定), 稳定后每 150 帧 (2.5 s) 才探一次。 */
            int bx, by, bw, bh;
            redo_crop = 0;
            if (detect_bbox_nv12(buf, sw, sh, &bx, &by, &bw, &bh)) {
                int ddx = bx - cx, ddy = by - cy;
                int ddw = bw - cw, ddh = bh - ch;
                if (ddx < 0) ddx = -ddx;
                if (ddy < 0) ddy = -ddy;
                if (ddw < 0) ddw = -ddw;
                if (ddh < 0) ddh = -ddh;
                /* 小幅抖动不重建几何, 免得画面跟着晃 */
                if (n == 0 || ddx > 16 || ddy > 16 || ddw > 16 || ddh > 16) {
                    /* 2026-10-26 实测: 只对「往里裁」做二次确认不够 —— 暗场/亮场切换时
                     * 探到的框会在两个值之间来回跳 (现场 play.log: 257x540 → 645x540 →
                     * 257x540, 每 4 s 一次), 每次跳都要清屏+重算几何 = 画面周期性
                     * 「顿一下 / 突然放大」。改成: 任何几何变化都要连续两次一致才生效,
                     * 且生效后锁 90 帧 (约 3 s) 不再动, 让画面稳定。 */
                    int confirmed = (px >= 0 &&
                                     abs(bx - px) <= 24 && abs(by - py) <= 24 &&
                                     abs(bw - pw) <= 24 && abs(bh - ph) <= 24);
                    if (n == 0 || (confirmed && n >= crop_hold_until)) {
                        cx = bx; cy = by; cw = bw; ch = bh;
                        px = -1; py = -1; pw = -1; ph = -1;
                        crop_hold_until = n + 90;
                        geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                        need_clear = 1;
                        LOG("自动裁黑边: 源 %dx%d @ %d,%d (整帧 %dx%d) → 绘制区 %dx%d @ %d,%d%s",
                            cw, ch, cx, cy, sw, sh, dw, dh, ox, oy,
                            fill_mode ? ", 铺满" : "");
                    } else {
                        px = bx; py = by; pw = bw; ph = bh;   /* 待确认 */
                    }
                }
            } else if (n == 0 && (cx || cy || cw != sw || ch != sh)) {
                cx = 0; cy = 0; cw = sw; ch = sh;
                geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
                need_clear = 1;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &ta);
        t_crop += (ta.tv_sec - tb.tv_sec) * 1e6 + (ta.tv_nsec - tb.tv_nsec) / 1e3;

        /* ---- NV12 直上 VO 视频层 (CPU 一个像素都不碰: 不转色, 不缩放) ----
         * 条件: NV12 + 1:1 (内容区 = 整帧, 不缩放)。黑边自己刷 ——
         * OSD4 (primary) 必须关掉, 它在视频层之上, 而 VO 背景是白色。
         * 2026-11-04: 不再排除旋转 —— 旋转帧在上面的 rot_deg 分支里已经用
         * rot90_cw_nv12() 分块转置进 rotbuf 并把 sw/sh 换过, 到这里 buf 就是
         * 「已转好」的 NV12, 与不旋转帧没有区别 (CarLife 832x480 / AirPlay
         * 横屏内容都是这条路, 以前被 !rot_deg 挡在视频层外面)。 */
        vo_path = 0;
        if (vo.ok && in_fmt == 2 && !scaled && (!rot_deg || vo.rot_ok) &&
            cw == sw && ch == sh && dw == sw && dh == sh) {
            if (vo_blit_nv12(&vo, nxt, buf, sw, sh, ox, oy) == 0) {
                vo_path = 1;
                if (!vo.logged) {
                    vo.logged = 1;
                    LOG("🎬 视频层直出: NV12 %dx%d 贴到 %dx%d @ %d,%d (转色/裁剪交 VO, CPU 只搬字节)",
                        sw, sh, W, H, ox & ~1, oy & ~1);
                }
            }
        }
        /* ---- 需要缩放时: CPU 只缩 NV12, 转色仍交 VO (2026-11-04) ----
         * 真机 Miracast 是 1280x720 源裁出 409x720 → 放大到 480x800, 永远不 1:1。
         * 旧路在这条路上每帧 ~12 ms (转色+缩放全 CPU); 这里省掉转色, 输出位宽也砍掉 62%。
         * 旋转帧同样走这里 (buf 已是转好的 NV12, 见上文)。 */
        if (!vo_path && vo.ok && in_fmt == 2 && (!rot_deg || vo.rot_ok) && scaled &&
            dw > 0 && dh > 0 && ox >= 0 && oy >= 0) {
            if (vo_blit_nv12_scaled(&vo, nxt, buf, sw, sh, cx, cy, cw, ch,
                                    ox, oy, dw, dh) == 0) {
                vo_path = 1;
                if (!vo.logged2) {
                    vo.logged2 = 1;
                    LOG("🎬 视频层缩放出图: NV12 %dx%d @%d,%d → %dx%d @%d,%d (VO 硬件转色, CPU 只缩放)",
                        cw, ch, cx, cy, dw, dh, ox & ~1, oy & ~1);
                }
            }
        }
        if (vo_path)
            goto draw_done;

        if (!scaled && in_fmt != 2) {
            /* 1:1 快速路径 */
            if (in_fmt == 0) {
                for (int y = 0; y < H; y++)
                    memcpy(map + (size_t)y * cd.pitch, buf + (size_t)y * sw * 4, (size_t)sw * 4);
            } else {
                for (int y = 0; y < H; y++) {
                    uint8_t *d = map + (size_t)y * cd.pitch;
                    const uint8_t *s = buf + (size_t)y * sw * 4;
                    for (int x = 0; x < sw; x++, d += 4, s += 4) {
                        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    }
                }
            }
        } else if (in_fmt == 2 && !scaled && cx == 0 && cy == 0 && cw == sw && ch == sh &&
                   dw == sw && dh == sh && ox == 0 && oy == 0 && (int)cd.pitch == W * 4) {
            /* NV12 1:1 (源 = 绘制区 = 屏) 直写快路径 (2026-10-27):
             * 原来是 "算到 eline[] → memcpy 到 drow", 每帧白搬 1.5 MB 读 + 1.5 MB 写。
             * 这里直接写屏缓冲, 且相邻两像素共用一组 U/V (色度乘加只做一半)。
             * 手机投 480x800 / 屏正好 480x800 时都走这条。 */
            const uint8_t *yp = buf;
            const uint8_t *uvp = buf + (size_t)sw * sh;
            for (int y = 0; y < sh; y++)
                nv12_row_to_argb((uint32_t *)(map + (size_t)y * cd.pitch),
                                 yp + (size_t)y * sw, uvp + (size_t)(y >> 1) * sw, sw);
            goto draw_done;
        } else if (in_fmt == 2 && dw == cw && dh == ch && (cx & 1) == 0 &&
                   ox >= 0 && oy >= 0 && ox + cw <= W && oy + ch <= H &&
                   (int)cd.pitch == W * 4) {
            /* 1:1 直贴快路径 (2026-10-28, WFD_1X): 内容区原始像素贴屏中央。
             * 每源行只做一次 SIMD YUV→RGB (cw 个像素) + 一次 memcpy, 没有 xmap
             * 展开、没有重采样。实测把「转换」从 12 ms 打回 ~4 ms。 */
            if (need_clear) {
                need_clear = 0;
                for (int _b = 0; _b < nbuf; _b++)
                    for (int y = 0; y < H; y++) {
                        uint32_t *_q = (uint32_t *)(mapb[_b] + (size_t)y * cd.pitch);
                        for (int _x = 0; _x < W; _x++) _q[_x] = 0xFF000000u;
                    }
            }
            const uint8_t *yp = buf;
            const uint8_t *uvp = buf + (size_t)sw * sh;
            static uint32_t *scratch = NULL;
            if (g_nostore && !scratch) scratch = malloc((size_t)cw * 4);
            for (int dy = 0; dy < ch; dy++) {
                const uint8_t *yrow = yp + (size_t)(cy + dy) * sw;
                const uint8_t *uvrow = uvp + (size_t)((cy + dy) >> 1) * sw;
                uint8_t *drow = g_nostore ? (uint8_t *)scratch
                                          : map + (size_t)(oy + dy) * cd.pitch + (size_t)ox * 4;
                nv12_row_to_argb((uint32_t *)drow, yrow + cx, uvrow + cx, cw);
            }
            goto draw_done;
        } else {
            /* 黑边填充 (仅缩放时有意义) —— 只在几何变化时做一次,
             * 否则每帧先刷黑再画 ⇒ 扫描到中间态就是"黑一下亮一下"。 */
            if (scaled && need_clear) {
                need_clear = 0;
                /* 两块都清: 只清当前这块的话, 翻到另一块会闪一帧旧几何的画面。
                 * 只在几何变化时做一次, 不影响每帧开销。 */
                for (int _b = 0; _b < nbuf; _b++) {
                    for (int y = 0; y < H; y++)
                    {
                        uint32_t *_q = (uint32_t *)(mapb[_b] + (size_t)y * cd.pitch);
                        for (int _x = 0; _x < W; _x++) _q[_x] = 0xFF000000u;   /* 不透明黑 */
                    }
                }
            }
            /* 绘制区可能超出屏 (cover 模式): 先算屏内可视范围 */
            int dy0 = oy < 0 ? -oy : 0;
            int dy1 = dh;
            if (oy + dy1 > H) dy1 = H - oy;
            int dx0 = ox < 0 ? -ox : 0;
            int dx1 = dw;
            if (ox + dx1 > W) dx1 = W - ox;

            /* dx → 源列 查表 (每帧只算 dw 次除法) */
            if (dw > xmap_cap) {
                int *nm = realloc(xmap, (size_t)dw * sizeof(int));
                if (!nm) { ERR("xmap 分配失败"); goto done; }
                xmap = nm;
                xmap_cap = dw;
            }
            for (int dx = 0; dx < dw; dx++)
                xmap[dx] = cx + (int)((long)dx * cw / dw);

            const uint8_t *yp = buf;
            const uint8_t *uvp = buf + (size_t)sw * sh;
            for (int dy = dy0; dy < dy1; dy++) {
                int sy = oy + dy;
                uint8_t *drow = map + (size_t)sy * cd.pitch + (size_t)(ox + dx0) * 4;
                int sry = cy + (int)((long)dy * ch / dh);
                if (in_fmt == 2) {
                    /* 单级行缓存 (2026-10-26 第二次修正): 直接把「输出行」按目标
                     * 宽度 dw 转好并缓存, 按源行号 sry 索引。
                     * 教训: 上一版先转整条源行 (cw 个) 再水平展开, 在**降采样**时
                     * 更慢 —— 720p→480x270 要转 270x1280=345,600 次, 而原始逐像素
                     * 只需 480x270=129,600 次。现在只转实际用到的 dw 列:
                     *   唯一源行数 x dw 次色彩变换 + 其余重复行 memcpy。
                     * 720p→480x270: 270x480 = 129,600 (与原始持平, 无重复行);
                     * 1080p 内容 257x540→381x800: 540x381 = 205,740 (原始 304,800)。 */
                    if (sry != eline_sry || cx != eline_cx || cw != eline_cw || dw != eline_dw) {
                        eline_miss++;
                        if (dw > eline_cap) {
                            uint32_t *nm = realloc(eline, (size_t)dw * sizeof(uint32_t));
                            if (!nm) { ERR("eline 分配失败"); goto done; }
                            eline = nm; eline_cap = dw;
                        }
                        if (dw > cw) {
                            /* 放大 (dw>cw): 先按源宽 cw 转一次, 再查表展开到 dw。
                             * 每行只 cw 次色彩变换 (如 1080p 内容 257 宽 → 381 宽画布)。 */
                            if (sry != line_sry || cx != line_cx || cw != line_cw) {
                                if (cw > line_cap) {
                                    uint32_t *nm = realloc(line, (size_t)cw * sizeof(uint32_t));
                                    if (!nm) { ERR("line 分配失败"); goto done; }
                                    line = nm; line_cap = cw;
                                }
                                const uint8_t *yrow = yp + (size_t)sry * sw;
                                const uint8_t *uvrow = uvp + (size_t)(sry / 2) * sw;
                                /* cx 为偶数时 UV 与像素严格 2:2 对齐, 可整行走向量化;
                                 * 奇数时第一对 UV 跨在 cx 左边, 退回标量 (罕见)。 */
                                if ((cx & 1) == 0) {
                                    nv12_row_to_argb(line, yrow + cx, uvrow + cx, cw);
                                } else {
                                    for (int i = 0; i < cw; i++) {
                                        int X = cx + i, ux = X & ~1;
                                        int Y = yrow[X];
                                        int U = uvrow[ux] - 128, V = uvrow[ux + 1] - 128;
                                        int c = Y - 16;
                                        int r = (298 * c + 409 * V + 128) >> 8;
                                        int g = (298 * c - 100 * U - 208 * V + 128) >> 8;
                                        int b = (298 * c + 516 * U + 128) >> 8;
                                        line[i] = (uint32_t)(b < 0 ? 0 : (b > 255 ? 255 : b))
                                                | ((uint32_t)(g < 0 ? 0 : (g > 255 ? 255 : g)) << 8)
                                                | ((uint32_t)(r < 0 ? 0 : (r > 255 ? 255 : r)) << 16)
                                                | 0xFF000000u;
                                    }
                                }
                                line_sry = sry; line_cx = cx; line_cw = cw;
                            }
                            for (int dx = 0; dx < dw; dx++)
                                eline[dx] = line[xmap[dx] - cx];
                        } else {
                            /* 缩小/等宽 (dw<=cw): 只转实际用到的 dw 列, 别转整条源行。 */
                            const uint8_t *yrow = yp + (size_t)sry * sw;
                            const uint8_t *uvrow = uvp + (size_t)(sry / 2) * sw;
                            for (int dx = 0; dx < dw; dx++) {
                                int X = xmap[dx];
                                int ux = X & ~1;
                                int Y = yrow[X];
                                int U = uvrow[ux] - 128;
                                int V = uvrow[ux + 1] - 128;
                                int c = Y - 16;
                                int r = (298 * c + 409 * V + 128) >> 8;
                                int g = (298 * c - 100 * U - 208 * V + 128) >> 8;
                                int b = (298 * c + 516 * U + 128) >> 8;
                                eline[dx] = (uint32_t)(b < 0 ? 0 : (b > 255 ? 255 : b))
                                          | ((uint32_t)(g < 0 ? 0 : (g > 255 ? 255 : g)) << 8)
                                          | ((uint32_t)(r < 0 ? 0 : (r > 255 ? 255 : r)) << 16)
                                          | 0xFF000000u;
                            }
                        }
                        eline_sry = sry;
                        eline_cx = cx; eline_cw = cw; eline_dw = dw;
                    } else {
                        eline_hits++;
                    }
                    memcpy(drow, eline + dx0, (size_t)(dx1 - dx0) * 4);
                } else {
                    const uint8_t *srow = buf + (size_t)sry * sw * 4;
                    for (int dx = dx0; dx < dx1; dx++) {
                        int srx = xmap[dx];
                        const uint8_t *s = srow + (size_t)srx * 4;
                        if (in_fmt == 0) { drow[0]=s[0]; drow[1]=s[1]; drow[2]=s[2]; }
                        else { drow[0]=s[2]; drow[1]=s[1]; drow[2]=s[0]; }
                        drow[3] = 0xff;
                        drow += 4;
                    }
                }
            }
        }
draw_done:
        /* 本帧已经读完了 (画进屏缓冲了) ⇒ 告诉解码端这一槽可以复写 (回压) */
        if (use_shm && g_shm_r && (int32_t)(shm_seq - *g_shm_r) > 0) {
            __sync_synchronize();
            *g_shm_r = shm_seq;
        }
        /* 管道延迟 = 解码器出帧 → 上屏完成 (不含网络/编码) */
        /* 转换耗时必须**每帧**累计 (上一版只在 lat_n>=60 那一帧累计, 再除以 60,
         * 结果被缩小 60 倍, 一度误判「转换只要 0.2 ms」)。 */
        {
            struct timespec tc;
            clock_gettime(CLOCK_MONOTONIC, &tc);
            t_conv += (tc.tv_sec - ta.tv_sec) * 1e6 + (tc.tv_nsec - ta.tv_nsec) / 1e3;
        }

        /* 画完才 flip: 屏上一帧永远是完整画面 (单缓冲时什么都不做)。
         * 异步提交 (只花 ~0.05 ms), 屏在下一个 vblank 自己换; 绝不能在这里等事件,
         * 一等就是 16 ms, 整圈立刻超过 60 fps 的预算 ⇒ 掉帧。 */
        if (vo.ok) {
            /* 方案A: 走 atomic (视频层或 OSD4 二选一), 照样带 flip 事件 ⇒ 显示节拍统计不变 */
            struct timespec tf0, tf1;
            uint32_t pfb = vo_path ? 0 : fb_id[nxt];
            clock_gettime(CLOCK_MONOTONIC, &tf0);
            if (vo_commit(fd, &vo, vo_path, vo_path ? vo.fb[nxt] : 0, pfb, 1) == 0) {
                cur = nxt;
                flip_pending = 1;
                n_flip++;
                vo.on = vo_path;
                idle_black = 0;
            } else {
                n_flip_fail++;
                vo.ok = 0;      /* 出错就彻底退回 CPU 路径, 别每帧刷错 */
            }
            clock_gettime(CLOCK_MONOTONIC, &tf1);
            t_flip += (double)(tf1.tv_sec - tf0.tv_sec) * 1e6 +
                      (double)(tf1.tv_nsec - tf0.tv_nsec) / 1e3;
        } else if (flip_ok) {
            struct timespec tf0, tf1;
            clock_gettime(CLOCK_MONOTONIC, &tf0);
            if (do_page_flip(fd, crtc, fb_id[nxt], -1) == 0) {
                cur = nxt;
                flip_pending = 1;
                n_flip++;
            } else {
                n_flip_fail++;
            }
            clock_gettime(CLOCK_MONOTONIC, &tf1);
            t_flip += (double)(tf1.tv_sec - tf0.tv_sec) * 1e6 +
                      (double)(tf1.tv_nsec - tf0.tv_nsec) / 1e3;
        }
        if (f_stamp) {
            struct timespec nts;
            uint32_t nowms;
            double d;
            clock_gettime(CLOCK_MONOTONIC, &nts);
            nowms = (uint32_t)(nts.tv_sec * 1000u + (uint32_t)(nts.tv_nsec / 1000000));
            d = (double)(int32_t)(nowms - f_stamp);
            lat_sum += d; lat_n++;
            if (d > lat_max) lat_max = d;
            if (lat_n >= 60) {
                struct timespec cpu_now;
                clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_now);
                double cpu_us = ((double)(cpu_now.tv_sec - cpu_prev.tv_sec) * 1e6 +
                                 (double)(cpu_now.tv_nsec - cpu_prev.tv_nsec) / 1e3);
                cpu_prev = cpu_now;
                LOG("管道延迟 avg %.1f ms max %.0f ms | 分段(每帧): 等vblank %.1f 读管道 %.1f 裁边 %.1f 转换 %.1f flip %.1f 整圈 %.1f ms | 每帧CPU %.2f ms | 内容变化 %ld/%d 帧 | 上屏 flip %ld/%d",
                    lat_sum / lat_n, lat_max,
                    t_vwait / lat_n / 1000.0, t_read / lat_n / 1000.0, t_crop / lat_n / 1000.0,
                    t_conv / lat_n / 1000.0, t_flip / lat_n / 1000.0,
                    t_iter / lat_n / 1000.0, cpu_us / lat_n / 1000.0,
                    uniq, lat_n + 0, n_flip, lat_n + 0);
                LOG("   源端节拍: 帧间隔 avg %.2f ms max %.1f (60fps 应为 16.67) | 帧体 min %u / avg %.1f / max %u KB",
                    gap_n > 0 ? gap_sum / gap_n : 0.0, gap_max,
                    fsz_min / 1024, lat_n > 0 ? fsz_sum / lat_n / 1024.0 : 0.0,
                    fsz_max / 1024);
                LOG("   显示节拍: 同vbl %ld / 1vbl %ld / 2vbl %ld / 3+vbl %ld (最长 %.1f ms) ⇒ 实测 %.1f fps",
                    disp_0, disp_1, disp_2, disp_3, disp_max,
                    (disp_win_start > 0 && disp_prev > disp_win_start)
                        ? (disp_1 + disp_2 + disp_3) / ((disp_prev - disp_win_start) / 1e6) : 0.0);
                if (n_flip_fail)
                    LOG("   ⚠ 本窗口 flip 被拒 %ld 次 (EBUSY/其他)", n_flip_fail);
                uniq = 0; n_flip = 0; n_flip_fail = 0;
                disp_0 = disp_1 = disp_2 = disp_3 = 0; disp_max = 0; disp_win_start = 0;
                fsz_min = fsz_max = 0; fsz_sum = 0;
                gap_sum = gap_max = 0; gap_n = 0;
                lat_sum = 0; lat_max = 0; lat_n = 0;
                t_read = t_crop = t_conv = t_flip = t_iter = 0;
                t_vwait = 0;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &ti1);
        t_iter += (ti1.tv_sec - ti0.tv_sec) * 1e6 + (ti1.tv_nsec - ti0.tv_nsec) / 1e3;
        n++;
        idle_black = 0;
        pace(&next, fps);
        if (max_frames > 0 && n >= max_frames)
            break;
        continue;

idle:
        /* 空闲时把读序号追平写序号: 否则解码端上一次没被消费的帧会把它卡在回压里 */
        if (g_shm_r && g_shm_w && *g_shm_r != *g_shm_w) {
            *g_shm_r = *g_shm_w;
            __sync_synchronize();
        }
        /* ★ 2026-10-29 (主人报「AirPlay 只有声音、画面全黑」的第二个入口):
         * macOS 静止桌面会几分钟不发帧; 半帧超时 (BODY_WAIT_MS) / 重对齐失败
         * 也会走到这里。只要看护还挂着 airplay-active / airplay-cast-active
         * (客户端没断), 就**只丢半帧、保持最后一帧**, 绝不刷黑。 */
        if (fifo_mode && n > 0 && any_airplay_active()) {
            g_pend_len = g_pend_off = 0;
            clock_gettime(CLOCK_MONOTONIC, &next);
            continue;
        }
        /* 常驻模式: 管道里没数据 (会话没开始, 或 source 已断开) —— 刷黑, 复位会话状态,
         * 继续等下一次。绝不退出: 一旦退出, CRTC 上就没有 plane, 面板立刻扫出紫色。 */
        if (!fifo_mode)
            goto done;
        if (!idle_black) {
            /* 收掉挂起的 flip 事件, 否则下面这次"刷黑 flip"会被判 EBUSY,
             * 屏就一直停在最后一帧 (这就是"断开后屏不黑"的复发路径)。 */
            while (recv_flip_event(fd, 50, NULL) == 0) { flip_pending = 0; }
            /* 所有块都刷黑再 flip 一次 */
            for (int _b = 0; _b < nbuf; _b++) {
                for (int y = 0; y < H; y++) {
                    uint32_t *_q = (uint32_t *)(mapb[_b] + (size_t)y * cd.pitch);
                    for (int _x = 0; _x < W; _x++) _q[_x] = 0xFF000000u;
                }
            }
            if (vo.ok) {
                int _b = (cur + 1) % nbuf;
                if (vo_commit(fd, &vo, 0, 0, fb_id[_b], 0) == 0) {
                    cur = _b;
                    vo.on = 0;
                }
            } else if (flip_ok) {
                int _b = (cur + 1) % nbuf;
                if (do_page_flip(fd, crtc, fb_id[_b], 100) == 0)
                    cur = _b;
            }
            idle_black = 1;
            if (n > 0)
                LOG("会话 %ld 结束: %ld 帧 → 刷黑等下一次", session, n);
            session++;
        }
        n = 0;
        g_pend_len = g_pend_off = 0;
        /* 2026-10-29: 每个会话重新判一次朝向。源端不是说同一个源就一直同朝向 ——
         * AirPlay(Mac, 1280x720 横屏满幅) 之后换成 Miracast(努比亚, 也是 1280x720 容器,
         * 但手机把竖屏画面竖着塞在中间, 内容 bbox = 407x720), 分辨率一样 ⇒ 不触发
         * 「输入变为」, 旧代码只在换分辨率时才重判, 结果手机被当成横屏转了 90 度,
         * 只剩 480x271 (占屏 34%)。会话开始一律重判, 用内容 bbox 说话。 */
        rot_pending = 1;
        px = py = pw = ph = -1;
        redo_crop = 1;
        need_clear = 1;
        crop_hold_until = 0;
        lat_sum = lat_max = 0;
        lat_n = 0;
        t_read = t_crop = t_conv = t_flip = t_iter = 0;
        cx = 0; cy = 0; cw = sw; ch = sh;
        geom_fit(W, H, fill_mode, cx, cy, cw, ch, &dw, &dh, &ox, &oy, &scaled);
        clock_gettime(CLOCK_MONOTONIC, &next);
    }

done:
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    LOG("收到 %ld 帧, %.2f s, %.1f fps", n, sec, sec > 0 ? n / sec : 0.0);

    LOG("行缓存: 需重算 %ld 行 / 命中复用 %ld 行", eline_miss, eline_hits);
    for (int _b = 0; _b < nbuf; _b++)
        if (mapb[_b]) munmap(mapb[_b], cd.size);
    free(xmap);
    free(eline);
    free(line);
    free(buf_heap);
    close(fd);
    return 0;
}
