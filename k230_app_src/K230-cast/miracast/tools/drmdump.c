
// drmdump: 把当前 CRTC 的 scanout 帧 dump 成 BGRA 原始数据 (调试用)
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdlib.h>

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,type) _IOWR(DRM_IOCTL_BASE,nr,type)
struct drm_mode_card_res { uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr; uint32_t count_fbs,count_crtcs,count_connectors,count_encoders; uint32_t min_width,max_width,min_height,max_height; };
struct drm_mode_crtc { uint64_t set_connectors_ptr; uint32_t count_connectors; uint32_t crtc_id; uint32_t fb_id; uint32_t x,y; uint32_t gamma_size; uint32_t mode_valid; struct { char name[32]; uint32_t clock; uint16_t hdisplay,hsync_start,hsync_end,htotal,vdisplay,vsync_start,vsync_end,vtotal; uint32_t vrefresh; uint32_t flags; uint32_t type; } mode; };
struct drm_mode_fb_cmd { uint32_t fb_id,width,height,pitch,bpp,depth; uint32_t handle; };
struct drm_mode_map_dumb { uint32_t handle; uint32_t pad; uint64_t offset; };
#define GETRES   DRM_IOWR(0xA0, struct drm_mode_card_res)
#define GETCRTC  DRM_IOWR(0xA1, struct drm_mode_crtc)
#define GETFB    DRM_IOWR(0xAD, struct drm_mode_fb_cmd)
#define MAPDUMB  DRM_IOWR(0xB3, struct drm_mode_map_dumb)

int main(int argc,char**argv){
    const char*dev=argc>1?argv[1]:"/dev/dri/card0";
    const char*out=argc>2?argv[2]:"/tmp/frame.bgra";
    int fd=open(dev,O_RDWR);
    if(fd<0){perror("open");return 1;}
    for(uint32_t id=40; id<=60; id++){
        struct drm_mode_crtc c; memset(&c,0,sizeof c); c.crtc_id=id;
        if(ioctl(fd,GETCRTC,&c)<0) continue;
        if(!c.fb_id) continue;
        struct drm_mode_fb_cmd fb; memset(&fb,0,sizeof fb); fb.fb_id=c.fb_id;
        if(ioctl(fd,GETFB,&fb)<0){perror("getfb");return 1;}
        struct drm_mode_map_dumb md; memset(&md,0,sizeof md); md.handle=fb.handle;
        if(ioctl(fd,MAPDUMB,&md)<0){perror("mapdumb");return 1;}
        uint8_t*p=mmap(0,fb.pitch*fb.height,PROT_READ,MAP_SHARED,fd,md.offset);
        if(p==MAP_FAILED){perror("mmap");return 1;}
        FILE*f=fopen(out,"wb");
        fwrite(p,1,(size_t)fb.pitch*fb.height,f); fclose(f);
        printf("crtc=%u fb=%u %ux%u pitch=%u -> %s\n",id,c.fb_id,fb.width,fb.height,fb.pitch,out);
        return 0;
    }
    printf("no active crtc with fb\n");
    return 2;
}
