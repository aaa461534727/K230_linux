
/* ai2dplay —— K230 硬件显示播放器 (方案A: VDEC NV12 → AI2D 硬件转+缩 → DRM 上屏)
 *
 * 与 drmplay 协议兼容: 从同一管道/共享内存读 16 字节帧头(magic NV12/w/h/len)+ NV12 帧体,
 * 但**不再用 CPU 做 NV12→ARGB / 缩放**, 全部交给 canaan non-ai-2d (V4L2 M2M)。
 * CPU 只做: 一次 1.38MB 的 NV12 memcpy 喂进 AI2D, 以及 ioctl/DMA 交接。
 * 输出固定 AR24 480x800, 由 AI2D 完成非均匀拉伸(等同 drmplay 的 -S stretch)。
 *
 * 2026-11-03 主人定: CPU 不许干像素处理。本程序据此实现。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <linux/videodev2.h>

/* ---------- DRM ioctl 定义 (裸 ioctl, 不依赖 libdrm) ---------- */
#define DRM_IOCTL_BASE 'd'
#define DRM_IO(nr)            _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOWR(nr,type)     _IOWR(DRM_IOCTL_BASE, nr, type)
struct drm_mode_create_dumb { uint32_t height,width,bpp,flags,handle,pitch; uint64_t size; };
struct drm_mode_map_dumb    { uint32_t handle; uint32_t pad; uint64_t offset; };
struct drm_mode_fb_cmd2 {
    uint32_t fb_id,width,height,pixel_format,flags;
    uint32_t handles[4],pitches[4],offsets[4];
    uint64_t modifier[4];
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr; uint32_t count_connectors; uint32_t crtc_id;
    uint32_t fb_id; uint32_t x,y; uint32_t gamma_size; uint32_t mode_valid;
    /* struct drm_mode_modeinfo mode; 简化: 我们只关心前几个字段对齐 */
    unsigned char mode[64];
};
struct drm_mode_crtc_page_flip { uint32_t crtc_id; uint32_t fb_id; uint32_t flags; uint32_t reserved; uint64_t user_data; };
struct drm_event_vblank { uint32_t type,length; uint64_t user_data; uint32_t tv_sec,tv_usec,sequence,crtc_id; };
struct drm_mode_get_resources {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs,count_crtcs,count_connectors,count_encoders;
    uint32_t min_width,max_width,min_height,max_height;
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id; uint32_t connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height; uint32_t subpixel;
    uint32_t count_modes2; /* 占位 */
};
struct drm_mode_modeinfo {
    uint32_t clock; uint16_t hdisplay,vdisplay;
    uint16_t hsync_start,hsync_end,htotal,vsync_start,vsync_end,vtotal,vscan;
    uint32_t vrefresh; uint32_t flags; uint32_t type; char name[32];
};
#define DRM_IOCTL_MODE_GETRESOURCES  DRM_IOWR(0xA0, struct drm_mode_get_resources)
#define DRM_IOCTL_MODE_GETCONNECTOR  DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETCRTC       DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC       DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_CREATE_DUMB   DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB      DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_ADDFB2        DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_CRTC_PAGE_FLIP DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_SETMASTER          DRM_IO(0x1e)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x2E, struct drm_prime_handle)
struct drm_prime_handle { uint32_t handle,flags; int32_t fd; };

#define FORMAT_AR24 0x34325241u   /* 'A','R','2','4' 小端 => ARGB8888 */
#define DRM_MODE_PAGE_FLIP_EVENT 0x01

/* ---------- 参数/状态 ---------- */
static int   g_drm = -1, g_crtc = 0, g_conn = 0;
static int   g_ai2d = -1;
static int   g_w = 480, g_h = 800;     /* 屏尺寸 (固定) */
static int   g_fifo = -1;
static const char *g_fifo_path = NULL;   /* -F <fifo>; 不给则从 stdin 读 (内联管道) */
static int   g_idle_ms = 800;            /* 常驻 fifo 无数据多久刷黑 (同 drmplay) */
static uint8_t *g_shm_base = NULL;
static volatile uint32_t *g_shm_w = NULL, *g_shm_r = NULL;
#define SHM_HEAD 64
#define SHM_SLOT (4u<<20)

/* AI2D M2M 缓冲 (3 块 ping-pong) */
#define NBA 3
static uint8_t *g_obuf_y[NBA], *g_obuf_uv[NBA];   /* 输出(NV12 输入) MMAP */
static unsigned g_olen_y[NBA], g_olen_uv[NBA];
static int   g_cap_fd[NBA];        /* EXPBUF 出的 dma-buf fd */
static uint32_t g_cap_fb[NBA];     /* DRM framebuffer id */
static int   g_cap_queued[NBA];    /* 当前是否在 AI2D 队列里 */
static int   g_cap_disp = -1;      /* 当前正在显示的那块 */
static int   g_ai2d_in_w = 0, g_ai2d_in_h = 0;
static uint32_t g_fb_black = 0;    /* 空闲刷黑用 dumb 缓冲 */

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }
static void on_fatal(int s){
    char m[48]; int n=snprintf(m,sizeof(m),"ai2dplay: FATAL signal %d\n", s);
    (void)!write(2, m, n);
    _exit(128+s);
}
static void plog(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void plog(const char *fmt, ...){
    va_list ap; char buf[256]; struct timespec ts;
    clock_gettime(CLOCK_REALTIME,&ts);
    va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    fprintf(stderr,"[ai2dplay]%02d:%02d:%02d %s\n",
            (int)(ts.tv_sec/3600%24),(int)(ts.tv_sec/60%60),(int)(ts.tv_sec%60),buf);
}

/* ================= DRM ================= */
static int prime_import(int dmafd, uint32_t *handle){
    struct drm_prime_handle ph; memset(&ph,0,sizeof(ph));
    ph.fd=dmafd; ph.flags=O_CLOEXEC;
    if (ioctl(g_drm, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph)<0) return -1;
    *handle=ph.handle; return 0;
}

static int drm_setup(void){
    g_drm = open("/dev/dri/card0", O_RDWR);
    if (g_drm<0){ plog("open /dev/dri/card0: %s", strerror(errno)); return -1; }
    if (ioctl(g_drm, DRM_IOCTL_SETMASTER, 0)<0)
        plog("⚠ SETMASTER %s (继续, 可能已是 master)", strerror(errno));

    uint32_t crtcs[8]={0}, conns[8]={0};
    struct drm_mode_get_resources r; memset(&r,0,sizeof(r));
    r.crtc_id_ptr=(uint64_t)(uintptr_t)crtcs; r.count_crtcs=8;
    r.connector_id_ptr=(uint64_t)(uintptr_t)conns; r.count_connectors=8;
    if (ioctl(g_drm, DRM_IOCTL_MODE_GETRESOURCES, &r)<0){ plog("GETRESOURCES %s",strerror(errno)); return -1; }
    if (r.count_crtcs==0 || r.count_connectors==0){ plog("无 crtc/connector"); return -1; }
    g_crtc=crtcs[0]; g_conn=conns[0];
    plog("crtc=%u conn=%u", g_crtc, g_conn);
    return 0;
}

static int getcrtc_mode(struct drm_mode_modeinfo *mode){
    struct drm_mode_crtc sc; memset(&sc,0,sizeof(sc)); sc.crtc_id=g_crtc;
    if (ioctl(g_drm, DRM_IOCTL_MODE_GETCRTC, &sc)==0 && (sc.mode_valid & 1)){
        memcpy(mode, sc.mode, sizeof(*mode));
        plog("当前 CRTC 模式 %s %ux%u@%u", mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh);
        return 0;
    }
    memset(mode,0,sizeof(*mode)); mode->hdisplay=g_w; mode->vdisplay=g_h; mode->vrefresh=60;
    snprintf(mode->name,sizeof(mode->name),"%dx%d",g_w,g_h);
    plog("取不到 CRTC 模式, 用默认 %dx%d", g_w, g_h);
    return 0;
}
static int pick_mode(struct drm_mode_modeinfo *mode){
    struct drm_mode_get_connector q; memset(&q,0,sizeof(q));
    q.connector_id=g_conn;
    uint32_t encs[8]; struct drm_mode_modeinfo modes[16];
    uint32_t props[64]; uint64_t pvals[64];
    q.encoders_ptr=(uint64_t)(uintptr_t)encs; q.modes_ptr=(uint64_t)(uintptr_t)modes;
    q.props_ptr=(uint64_t)(uintptr_t)props; q.prop_values_ptr=(uint64_t)(uintptr_t)pvals;
    if (ioctl(g_drm, DRM_IOCTL_MODE_GETCONNECTOR, &q)<0){ plog("GETCONNECTOR %s",strerror(errno)); return -1; }
    int found=-1;
    for (uint32_t i=0;i<q.count_modes;i++){
        if (modes[i].hdisplay==(uint16_t)g_w && modes[i].vdisplay==(uint16_t)g_h){ *mode=modes[i]; found=1; break; }
    }
    if (found<0 && q.count_modes>0) *mode=modes[0];
    if (found<0 && q.count_modes==0){
        memset(mode,0,sizeof(*mode)); mode->hdisplay=g_w; mode->vdisplay=g_h; mode->vrefresh=60;
        snprintf(mode->name,sizeof(mode->name),"%dx%d",g_w,g_h);
    }
    plog("mode %s %ux%u@%u", mode->name, mode->hdisplay, mode->vdisplay, mode->vrefresh);
    return 0;
}

static int make_black_fb(void){
    struct drm_mode_create_dumb cd; memset(&cd,0,sizeof(cd));
    cd.width=g_w; cd.height=g_h; cd.bpp=32;
    if (ioctl(g_drm, DRM_IOCTL_MODE_CREATE_DUMB,&cd)<0) return -1;
    struct drm_mode_fb_cmd2 f; memset(&f,0,sizeof(f));
    f.width=g_w; f.height=g_h; f.pixel_format=FORMAT_AR24; f.handles[0]=cd.handle; f.pitches[0]=cd.pitch;
    if (ioctl(g_drm, DRM_IOCTL_MODE_ADDFB2,&f)<0) return -1;
    struct drm_mode_map_dumb md; memset(&md,0,sizeof(md)); md.handle=cd.handle;
    if (ioctl(g_drm, DRM_IOCTL_MODE_MAP_DUMB,&md)<0) return -1;
    uint8_t *m=mmap(NULL,cd.size,PROT_READ|PROT_WRITE,MAP_SHARED,g_drm,md.offset);
    if (m!=MAP_FAILED){ uint32_t *q=(uint32_t*)m; for (size_t i=0;i<cd.size/4;i++) q[i]=0xFF000000u; }
    g_fb_black=f.fb_id;
    return 0;
}

/* ================= AI2D M2M ================= */
static int ai2d_alloc_out(unsigned iw, unsigned ih){
    /* 释放旧的 (STREAMOFF 后 REQBUFS 0) */
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof(rb));
    rb.count=0; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=V4L2_MEMORY_MMAP;
    ioctl(g_ai2d, VIDIOC_REQBUFS, &rb);
    for (int i=0;i<NBA;i++){
        if (g_obuf_y[i]){ munmap(g_obuf_y[i], g_olen_y[i]); g_obuf_y[i]=NULL; }
        if (g_obuf_uv[i]){ munmap(g_obuf_uv[i], g_olen_uv[i]); g_obuf_uv[i]=NULL; }
    }
    memset(&rb,0,sizeof(rb));
    rb.count=NBA; rb.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; rb.memory=V4L2_MEMORY_MMAP;
    if (ioctl(g_ai2d, VIDIOC_REQBUFS, &rb)<0 || rb.count==0){
        plog("AI2D out REQBUFS %s", strerror(errno)); return -1;
    }
    for (unsigned i=0;i<rb.count && i<NBA;i++){
        struct v4l2_buffer b; struct v4l2_plane pl[2]; memset(&b,0,sizeof(b)); memset(pl,0,sizeof(pl));
        b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=V4L2_MEMORY_MMAP; b.index=i;
        b.m.planes=pl; b.length=2;
        if (ioctl(g_ai2d, VIDIOC_QUERYBUF, &b)<0){ plog("AI2D out QUERYBUF %s",strerror(errno)); return -1; }
        g_obuf_y[i]=mmap(NULL,pl[0].length,PROT_READ|PROT_WRITE,MAP_SHARED,g_ai2d,pl[0].m.mem_offset);
        g_obuf_uv[i]=mmap(NULL,pl[1].length,PROT_READ|PROT_WRITE,MAP_SHARED,g_ai2d,pl[1].m.mem_offset);
        g_olen_y[i]=pl[0].length; g_olen_uv[i]=pl[1].length;
    }
    g_ai2d_in_w=iw; g_ai2d_in_h=ih;
    return 0;
}
static int ai2d_setup(void){
    g_ai2d=open("/dev/video4", O_RDWR);
    if (g_ai2d<0){ plog("open /dev/video4(AI2D): %s",strerror(errno)); return -1; }
    /* 捕获(输出到屏) AR24 480x800 */
    struct v4l2_format f; memset(&f,0,sizeof(f));
    f.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width=g_w; f.fmt.pix_mp.height=g_h;
    f.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_ARGB32; f.fmt.pix_mp.field=V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes=1;
    if (ioctl(g_ai2d, VIDIOC_S_FMT, &f)<0){ plog("AI2D cap S_FMT %s",strerror(errno)); return -1; }
    plog("AI2D cap fmt AR24 %dx%d", g_w, g_h);
    /* 先设 output(NV12) 格式, 保证 capture EXPBUF 之前队列就绪 */
    struct v4l2_format fo; memset(&fo,0,sizeof(fo));
    fo.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; fo.fmt.pix_mp.width=1280; fo.fmt.pix_mp.height=720;
    fo.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12; fo.fmt.pix_mp.field=V4L2_FIELD_NONE; fo.fmt.pix_mp.num_planes=2;
    ioctl(g_ai2d, VIDIOC_S_FMT, &fo);

    /* 捕获用 MMAP + EXPBUF (最稳, 基准里验证过) */
    struct v4l2_requestbuffers rb; memset(&rb,0,sizeof(rb));
    rb.count=NBA; rb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory=V4L2_MEMORY_MMAP;
    if (ioctl(g_ai2d, VIDIOC_REQBUFS, &rb)<0 || rb.count==0){
        plog("AI2D cap REQBUFS(MMAP) %s", strerror(errno)); return -1;
    }
    for (unsigned i=0;i<rb.count && i<NBA;i++){
        struct v4l2_exportbuffer eb; memset(&eb,0,sizeof(eb));
        eb.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; eb.index=i; eb.plane=0; eb.flags=O_CLOEXEC;
        if (ioctl(g_ai2d, VIDIOC_EXPBUF, &eb)<0){ plog("AI2D EXPBUF %s",strerror(errno)); return -1; }
        g_cap_fd[i]=eb.fd;
        uint32_t hnd;
        if (prime_import(eb.fd, &hnd)<0){ plog("PRIME import %s",strerror(errno)); return -1; }
        struct drm_mode_fb_cmd2 f2; memset(&f2,0,sizeof(f2));
        f2.width=g_w; f2.height=g_h; f2.pixel_format=FORMAT_AR24;
        f2.handles[0]=hnd; f2.pitches[0]=g_w*4; f2.offsets[0]=0;
        if (ioctl(g_drm, DRM_IOCTL_MODE_ADDFB2,&f2)<0){ plog("ADDFB2 cap %u %s",i,strerror(errno)); return -1; }
        g_cap_fb[i]=f2.fb_id; g_cap_queued[i]=1;
        plog("AI2D cap[%u] fb=%u", i, f2.fb_id);
    }
    /* 输入缓冲先按 1280x720 备着, 真正尺寸由首帧决定 */
    if (ai2d_alloc_out(1280,720)<0) return -1;
    /* 队列里先放 2 块 capture (第 3 块留给"正在显示") */
    for (int i=0;i<NBA;i++){
        struct v4l2_buffer b; struct v4l2_plane pl[1]; memset(&b,0,sizeof(b)); memset(pl,0,sizeof(pl));
        b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_MMAP; b.index=i;
        b.m.planes=pl; b.length=1;
        if (ioctl(g_ai2d, VIDIOC_QBUF, &b)<0){ plog("cap QBUF %d %s",i,strerror(errno)); return -1; }
    }
    int t=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; if (ioctl(g_ai2d,VIDIOC_STREAMON,&t)<0){ plog("cap STREAMON %s",strerror(errno)); return -1; }
    t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  if (ioctl(g_ai2d,VIDIOC_STREAMON,&t)<0){ plog("out STREAMON %s",strerror(errno)); return -1; }
    return 0;
}

/* 把一块 NV12(w,h) 喂给 AI2D 并取回 AR24 fb_id (通过 *fb_out)。返回 0 成功 */
static int ai2d_feed(const uint8_t *nv12, unsigned w, unsigned h, uint32_t *fb_out){
    /* 输入尺寸变化 → 重新分配输出缓冲 */
    if (w!=g_ai2d_in_w || h!=g_ai2d_in_h){
        int t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; ioctl(g_ai2d,VIDIOC_STREAMOFF,&t);
        if (ai2d_alloc_out(w,h)<0) return -1;
        t=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; if (ioctl(g_ai2d,VIDIOC_STREAMON,&t)<0) return -1;
        plog("AI2D 输入尺寸切换为 %ux%u", w, h);
    }
    /* 选一块空闲的输出缓冲 (用 round-robin, 配合每帧 DQBUF 保证不冲突) */
    static int oi=0;
    int o=oi; oi=(oi+1)%NBA;
    unsigned wh=w*h;
    if ((size_t)wh > g_olen_y[o] || (size_t)(wh/2) > g_olen_uv[o]){ plog("AI2D 缓冲太小 %ux%u",w,h); return -1; }
    memcpy(g_obuf_y[o], nv12, wh);
    memcpy(g_obuf_uv[o], nv12+wh, wh/2);

    struct v4l2_buffer b; struct v4l2_plane pl[2]; memset(&b,0,sizeof(b)); memset(pl,0,sizeof(pl));
    b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; b.memory=V4L2_MEMORY_MMAP; b.index=o; b.m.planes=pl; b.length=2;
    pl[0].bytesused=wh; pl[1].bytesused=wh/2;
    if (ioctl(g_ai2d, VIDIOC_QBUF, &b)<0){ plog("AI2D out QBUF %s",strerror(errno)); return -1; }

    /* 等一块 capture 完成 */
    struct pollfd pfd; pfd.fd=g_ai2d; pfd.events=POLLIN; pfd.revents=0;
    if (poll(&pfd,1,2000)<=0){ plog("AI2D poll 超时"); return -1; }
    struct v4l2_buffer d; struct v4l2_plane dp[1]; memset(&d,0,sizeof(d)); memset(dp,0,sizeof(dp));
    d.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; d.memory=V4L2_MEMORY_MMAP; d.m.planes=dp; d.length=1;
    if (ioctl(g_ai2d, VIDIOC_DQBUF, &d)<0){ plog("AI2D DQBUF %s",strerror(errno)); return -1; }
    int ci=d.index;
    g_cap_queued[ci]=0;
    *fb_out=g_cap_fb[ci];
    /* M2M: 处理完后必须 DQBUF 回收 output 缓冲, 否则缓冲会耗尽 */
    struct v4l2_buffer ob; struct v4l2_plane opl[2]; memset(&ob,0,sizeof(ob)); memset(opl,0,sizeof(opl));
    ob.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; ob.memory=V4L2_MEMORY_MMAP; ob.index=o; ob.m.planes=opl; ob.length=2;
    if (ioctl(g_ai2d, VIDIOC_DQBUF, &ob)<0){ plog("AI2D out DQBUF %s", strerror(errno)); }
    return ci;   /* 返回 capture 块号, 调用方负责 flip 后重新入队 */
}

/* flip 到指定 fb, 并维护 capture 队列 (把上一次显示的块在 flip 完成后重新入队) */
static int g_flip_ok=0; static int g_flip_pending=0;
static int do_flip(uint32_t fb){
    struct drm_mode_crtc_page_flip pf; memset(&pf,0,sizeof(pf));
    pf.crtc_id=g_crtc; pf.fb_id=fb; pf.flags=DRM_MODE_PAGE_FLIP_EVENT; pf.user_data=fb;
    if (ioctl(g_drm, DRM_IOCTL_MODE_CRTC_PAGE_FLIP, &pf)<0) return -1;
    return 0;
}
static void recv_flip_event(void){
    struct pollfd p; p.fd=g_drm; p.events=POLLIN; p.revents=0;
    if (poll(&p,1,100)>0){ struct drm_event_vblank ev; if (read(g_drm,&ev,sizeof(ev))==(ssize_t)sizeof(ev)){} }
}
/* 把一块 capture 缓冲重新入队给 AI2D (MMAP) */
static void cap_requee(int idx){
    struct v4l2_buffer b; struct v4l2_plane pl[1]; memset(&b,0,sizeof(b)); memset(pl,0,sizeof(pl));
    b.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory=V4L2_MEMORY_MMAP; b.index=idx; b.m.planes=pl; b.length=1;
    if (ioctl(g_ai2d,VIDIOC_QBUF,&b)==0) g_cap_queued[idx]=1;
}

/* ================= 输入 (fifo 常驻 / stdin 内联) ================= */
/* 常驻 fifo 约定同 drmplay: O_RDWR|O_NONBLOCK + dup2 到 fd0
 *   ① open 不阻塞 (无写端也立刻返回)  ② 自持写端 ⇒ 写端退出不 EOF, 靠 idle 超时刷黑
 *   ③ /proc/<pid>/fd/0 指向管道 ⇒ wfd-p2p 的 player_ok 检查通过 */
static uint8_t *g_frame=NULL; static size_t g_frame_cap=0;

static int input_open(void){
    if (g_fifo_path && strcmp(g_fifo_path,"-") && strcmp(g_fifo_path,"/dev/stdin")){
        mkfifo(g_fifo_path, 0666);                 /* EEXIST 忽略 */
        int fd = open(g_fifo_path, O_RDWR|O_NONBLOCK);
        if (fd < 0) return -1;
        if (fd != 0){ dup2(fd, 0); close(fd); }
        g_fifo = 0;
        return 0;
    }
    int fl = fcntl(0, F_GETFL, 0);                 /* stdin/内联管道: 非阻塞读 fd0 */
    if (fl >= 0) fcntl(0, F_SETFL, fl | O_NONBLOCK);
    g_fifo = 0;
    return 0;
}

/* 读满 n 字节; first_to=起始等待(空闲判定) rest_to=帧中途等待
 * 返回 1=读满  0=首字节前超时(空闲)  -2=帧中途断(丢弃)  -1=错误 */
static int read_full(int fd, void *buf, size_t n, int first_to, int rest_to){
    uint8_t *p=buf; size_t got=0;
    while (got<n){
        ssize_t r=read(fd, p+got, n-got);
        if (r>0){ got+=r; continue; }
        if (r==0) return got ? -2 : 0;
        if (errno==EAGAIN || errno==EWOULDBLOCK){
            struct pollfd pf; pf.fd=fd; pf.events=POLLIN; pf.revents=0;
            int pr=poll(&pf,1,(got==0)?first_to:rest_to);
            if (pr==0) return got ? -2 : 0;
            if (pr<0){ if (errno==EINTR) continue; return -1; }
            continue;
        }
        if (errno==EINTR) continue;
        return -1;
    }
    return 1;
}

/* 返回: 0 = 拿到一帧(nv12/w/h), 1 = 空闲/丢弃(去刷黑), -1 = 致命 */
static int recv_frame(uint8_t **out_nv12, unsigned *out_w, unsigned *out_h){
    unsigned char hd[16];
    int r=read_full(g_fifo, hd, 16, g_idle_ms, 2000);
    if (r<=0) return 1;                            /* 空闲超时 / 半帧: 刷黑 */
    if (memcmp(hd,"NV12",4)!=0){ plog("magic 错: %c%c%c%c (只支持 NV12)",hd[0],hd[1],hd[2],hd[3]); return 1; }
    unsigned w=hd[4]|(hd[5]<<8), h=hd[6]|(hd[7]<<8);
    uint32_t len=hd[8]|(hd[9]<<8)|(hd[10]<<16)|(hd[11]<<24);
    int shm = (len & 0x80000000u)!=0; len &= 0x7fffffff;
    uint32_t slot=0;
    if (shm){ if (read_full(g_fifo,&slot,4,2000,2000)!=1) return 1; }
    if (len > g_frame_cap || !g_frame){ free(g_frame); g_frame=calloc(1,(size_t)len+64); g_frame_cap=(size_t)len+64; }
    if (!shm){
        if (read_full(g_fifo,g_frame,len,2000,2000)!=1) return 1;
    } else {
        if (!g_shm_base) return 1;
        uint8_t *src = g_shm_base + SHM_HEAD + (size_t)(slot&1)*SHM_SLOT;
        memcpy(g_frame, src, len);
        if (g_shm_r) { *g_shm_r = slot; __sync_synchronize(); }   /* 回压: 消费完发布读序号 */
    }
    *out_nv12=g_frame; *out_w=w; *out_h=h;
    return 0;
}

static void shm_map(void){
    const char *p=getenv("WFD_SHM");
    if (!p||!*p) return;
    size_t sz=SHM_HEAD+2*SHM_SLOT;
    int fd=open(p,O_RDWR|O_CREAT,0666);
    if (fd<0){ plog("⚠ SHM %s 打不开→回退管道",p); return; }
    struct stat st; if (fstat(fd,&st)!=0||(size_t)st.st_size<sz) ftruncate(fd,(off_t)sz);
    void *m=mmap(NULL,sz,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd);
    if (m==MAP_FAILED){ plog("⚠ SHM mmap 失败→回退管道"); return; }
    g_shm_base=(uint8_t*)m; g_shm_w=(volatile uint32_t*)(m+0); g_shm_r=(volatile uint32_t*)(m+4);
    plog("SHM 帧缓冲 %s (2×%uKB)", p, SHM_SLOT/1024);
}

/* ================= main ================= */
int main(int argc, char **argv){
    signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
    signal(SIGSEGV,on_fatal); signal(SIGBUS,on_fatal); signal(SIGILL,on_fatal); signal(SIGABRT,on_fatal);
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-w")&&i+1<argc) g_w=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-h")&&i+1<argc) g_h=atoi(argv[++i]);
        else if (!strcmp(argv[i],"-F")&&i+1<argc) g_fifo_path=argv[++i];
        else if (!strcmp(argv[i],"-d")&&i+1<argc){ } /* 预留 */
    }
    plog("启动: %s (%dx%d), 硬件 NV12→AR24 由 AI2D 完成", g_fifo_path?g_fifo_path:"(stdin)", g_w, g_h);

    if (drm_setup()<0) return 1;
    struct drm_mode_modeinfo mode; memset(&mode,0,sizeof(mode));
    if (getcrtc_mode(&mode)<0) return 1;
    if (make_black_fb()<0) plog("⚠ 黑屏缓冲建不出");
    if (ai2d_setup()<0){ plog("✗ AI2D 初始化失败, 退出"); return 1; }

    /* 先刷黑占住 CRTC */
    if (g_fb_black){
        struct drm_mode_crtc sc; memset(&sc,0,sizeof(sc));
        uint32_t one=g_conn; sc.set_connectors_ptr=(uint64_t)(uintptr_t)&one; sc.count_connectors=1;
        sc.crtc_id=g_crtc; sc.fb_id=g_fb_black; memcpy(sc.mode,&mode,sizeof(mode)); sc.mode_valid=1;
        if (ioctl(g_drm, DRM_IOCTL_MODE_SETCRTC, &sc)<0){ plog("SETCRTC %s",strerror(errno)); return 1; }
        plog("✅ 上屏(黑) crtc=%u conn=%u", g_crtc, g_conn);
    }
    /* 探测 flip */
    if (do_flip(g_cap_fb[0])==0){ recv_flip_event(); if (do_flip(g_cap_fb[1])==0){ recv_flip_event(); g_flip_ok=1; } }
    plog(g_flip_ok?"🎞 PAGE_FLIP 可用":"⚠ PAGE_FLIP 不可用(将无双缓冲)");

    shm_map();
    if (input_open()<0){ plog("✗ 打开输入失败: %s", strerror(errno)); return 1; }

    int idle=0;
    while (!g_stop){
        uint8_t *nv12; unsigned w,h;
        int rr=recv_frame(&nv12,&w,&h);
        if (rr==1){
            if (!idle && g_flip_ok){
                if (g_flip_pending){ recv_flip_event(); if (g_cap_disp>=0){ cap_requee(g_cap_disp); g_cap_disp=-1; } g_flip_pending=0; }
                if (g_fb_black){ do_flip(g_fb_black); recv_flip_event(); }
                idle=1; plog("会话结束 → 刷黑");
            }
            continue;
        }
        if (rr<0) break;

        uint32_t fb=0; int ci=ai2d_feed(nv12,w,h,&fb);
        if (ci<0){ usleep(5000); continue; }
        { static unsigned long fcnt=0; fcnt++; if ((fcnt & 15)==0) plog("已收 %lu 帧 -> fb=%u (w=%u h=%u)", fcnt, fb, w, h); }
        if (g_flip_ok){
            /* 等上一次 flip 真正完成, 才能回收它占用的 capture 块 */
            if (g_flip_pending){ recv_flip_event(); if (g_cap_disp>=0){ cap_requee(g_cap_disp); g_cap_disp=-1; } g_flip_pending=0; }
            if (do_flip(fb)<0){ plog("flip 失败 %s",strerror(errno)); cap_requee(ci); }
            else { g_flip_pending=1; g_cap_disp=ci; }
        }
        idle=0;
    }
    plog("退出");
    return 0;
}
