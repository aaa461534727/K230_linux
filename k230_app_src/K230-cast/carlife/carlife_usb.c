/*
 * carlife_usb.c —— USB AOA (Android Open Accessory) 握手 + bulk 收发
 *
 * 流程 (Google AOA 2.0 公开标准):
 *   ① control 0xC0/51 读协议版本
 *   ② control 0x40/52 逐个发 6 条识别串 (UTF-16LE, 带结尾 NUL)
 *   ③ control 0x40/53 让手机切 accessory 模式
 *   ④ 手机重新枚举成 18d1:2d00 / 18d1:2d01, 打开 interface 0 的 bulk 对
 */
#include "carlife.h"
#include "carlife_io.h"
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AOA_GET_PROTOCOL 51
#define AOA_SEND_ID      52
#define AOA_START        53

/* CarLife 车机端识别串 (Android 按 manufacturer/model/version 派发
 * USB_ACCESSORY_ATTACHED; 这组是 CarLife 官方车机用的值) */
static const char *IDS[6] = {
    "Baidu", "CarLife", "Baidu CarLife",
    "1.0.0", "http://carlife.baidu.com/", "0720SerialNo."
};

static int g_last_err;
const char *cl_usb_last_error(void) { return libusb_error_name(g_last_err); }

struct cl_usb {
    libusb_device_handle *h;
    uint8_t ep_in;
    uint8_t ep_out;
    int intf;
    int maxp_in;
};

static void logf_(int fd, const char *fmt, ...)
{
    if (fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(fd, buf, (size_t)n); (void)w; }
}

static int aoa_get_protocol(libusb_device_handle *h)
{
    uint8_t buf[2] = {0, 0};
    int r = libusb_control_transfer(h, 0xC0, AOA_GET_PROTOCOL, 0, 0, buf, 2, 1000);
    if (r < 0) return -1;
    return buf[0] | (buf[1] << 8);
}

static int aoa_send_string(libusb_device_handle *h, int idx, const char *s, int log_fd)
{
    /* ★ 2026-10-30 现场实证: 必须发 **UTF-8 + NUL**, 不是 AOA 规范写的 UTF-16LE。
     *   发 UTF-16LE 时, Nubia NX589J (Android 7.1.1) 的 f_accessory 把缓冲区当 C 串
     *   处理, 每个串只剩第一个字符 —— 手机侧 `dumpsys usb` 显示
     *   mCurrentAccessory=UsbAccessory[mManufacturer=B, mModel=C, mDescription=B, ...],
     *   于是 USB_ACCESSORY_ATTACHED 不匹配 com.baidu.carlife 的过滤器, App 根本不会打开
     *   accessory 的 fd ⇒ 我们写 bulk OUT 一直 LIBUSB_ERROR_TIMEOUT。
     *   参考实现 (CarLife-HeadUnit-Emulator hu_lab/usb.py) 同样是 value.encode('utf-8') + b'\0'。
     *   规范: 单串含 NUL 不超过 256 字节。 */
    size_t n = strlen(s) + 1;
    if (n > 256) { logf_(log_fd, "[usb] AOA52 idx=%d 串过长 (需 <256 B)\n", idx); return -1; }
    int r = libusb_control_transfer(h, 0x40, AOA_SEND_ID, 0, (uint16_t)idx,
                                   (uint8_t *)s, (uint16_t)n, 2000);
    if (r < 0) logf_(log_fd, "[usb] AOA52 idx=%d '%s' 失败: %s\n", idx, s, libusb_error_name(r));
    else logf_(log_fd, "[usb] AOA52 idx=%d '%s' ok (%zu B utf-8)\n", idx, s, n);
    return r;
}

/* 在设备表里找一台"能说 AOA"的安卓手机并切过去 */
int cl_usb_open_and_switch(int log_fd)
{
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(NULL, &list);
    if (n < 0) return -1;

    libusb_device_handle *chosen = NULL;
    int chosen_proto = 0;
    for (ssize_t i = 0; i < n && !chosen; i++) {
        libusb_device *d = list[i];
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(d, &dd) != 0) continue;
        if (dd.idVendor == 0x18d1 && (dd.idProduct == 0x2d00 || dd.idProduct == 0x2d01))
            continue;                       /* 已经是 accessory 了 */
        libusb_device_handle *h = NULL;
        if (libusb_open(d, &h) != 0 || !h) continue;
        int proto = aoa_get_protocol(h);
        if (proto >= 1) { chosen = h; chosen_proto = proto; break; }
        libusb_close(h);
    }
    libusb_free_device_list(list, 1);
    if (!chosen) { logf_(log_fd, "[usb] 没找到支持 AOA 的设备\n"); return -1; }

    logf_(log_fd, "[usb] 找到 AOA 设备, 协议版本=%d\n", chosen_proto);
    for (int i = 0; i < 6; i++)
        if (aoa_send_string(chosen, i, IDS[i], log_fd) < 0) {
            libusb_close(chosen);
            return -1;
        }

    int r = libusb_control_transfer(chosen, 0x40, AOA_START, 0, 0, NULL, 0, 2000);
    libusb_close(chosen);
    if (r < 0) { logf_(log_fd, "[usb] AOA53 失败: %s\n", libusb_error_name(r)); return -1; }
    logf_(log_fd, "[usb] AOA53 已下发, 等手机重新枚举…\n");
    return 0;
}

static int find_bulk_pair(libusb_device *d, int *intf, uint8_t *in, uint8_t *out, int *maxp)
{
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(d, &cfg) != 0 || !cfg) return -1;
    int found = -1;
    for (int i = 0; i < cfg->bNumInterfaces && found < 0; i++) {
        const struct libusb_interface *it = &cfg->interface[i];
        for (int a = 0; a < it->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &it->altsetting[a];
            uint8_t bi = 0, bo = 0;
            int mi = 0;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                if ((ep->bmAttributes & 0x3) != 0x2) continue;      /* bulk */
                if (ep->bEndpointAddress & 0x80) { bi = ep->bEndpointAddress; mi = ep->wMaxPacketSize; }
                else bo = ep->bEndpointAddress;
            }
            if (bi && bo) {
                *intf = id->bInterfaceNumber; *in = bi; *out = bo; *maxp = mi;
                found = 0;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

cl_usb *cl_usb_wait_accessory(int timeout_ms, int log_fd)
{
    int waited = 0;
    while (waited < timeout_ms) {
        libusb_device **list = NULL;
        ssize_t n = libusb_get_device_list(NULL, &list);
        for (ssize_t i = 0; i < n; i++) {
            libusb_device *d = list[i];
            struct libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(d, &dd) != 0) continue;
            if (dd.idVendor != 0x18d1 || (dd.idProduct != 0x2d00 && dd.idProduct != 0x2d01))
                continue;
            libusb_device_handle *h = NULL;
            if (libusb_open(d, &h) != 0 || !h) continue;
            int intf = -1, maxp = 0; uint8_t bi = 0, bo = 0;
            if (find_bulk_pair(d, &intf, &bi, &bo, &maxp) != 0) { libusb_close(h); continue; }
            if (libusb_claim_interface(h, intf) != 0) { libusb_close(h); continue; }
            libusb_clear_halt(h, bi);
            libusb_clear_halt(h, bo);
            cl_usb *u = calloc(1, sizeof *u);
            u->h = h; u->ep_in = bi; u->ep_out = bo; u->intf = intf; u->maxp_in = maxp;
            libusb_free_device_list(list, 1);
            logf_(log_fd, "[usb] accessory 就绪: if=%d ep_in=0x%02x ep_out=0x%02x mps=%d\n",
                  intf, bi, bo, maxp);
            return u;
        }
        libusb_free_device_list(list, 1);
        usleep(200 * 1000);
        waited += 200;
    }
    logf_(log_fd, "[usb] 等 accessory 超时 (%d ms)\n", timeout_ms);
    return NULL;
}

int cl_usb_read(cl_usb *u, uint8_t *buf, size_t cap, int timeout_ms)
{
    int got = 0;
    int r = libusb_bulk_transfer(u->h, u->ep_in, buf, (int)cap, &got, (unsigned)timeout_ms);
    if (r == 0) return got;
    if (r == LIBUSB_ERROR_TIMEOUT) return 0;
    g_last_err = r;
    return -1;
}

int cl_usb_send(cl_usb *u, const uint8_t *frame, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int sent = 0;
        int r = libusb_bulk_transfer(u->h, u->ep_out, (uint8_t *)(frame + off),
                                     (int)(n - off), &sent, 2000);
        if (r != 0 || sent <= 0) { g_last_err = r; return -1; }
        off += (size_t)sent;
    }
    return (int)off;
}

int cl_usb_write(cl_usb *u, const uint8_t *buf, size_t n) { return cl_usb_send(u, buf, n); }

static int io_rd(void *ctx, uint8_t *buf, size_t cap, int ms)
{
    return cl_usb_read((cl_usb *)ctx, buf, cap, ms);
}

static int io_wr(void *ctx, const uint8_t *buf, size_t n)
{
    return cl_usb_send((cl_usb *)ctx, buf, n);
}

cl_io *cl_usb_as_io(struct cl_usb *u)
{
    cl_io *io = calloc(1, sizeof *io);
    if (!io) return NULL;
    io->rd = io_rd; io->wr = io_wr; io->ctx = (void *)u;
    return io;
}

void cl_io_free(cl_io *io) { free(io); }        /* 不关底层 usb */

void cl_usb_close(cl_usb *u)
{
    if (!u) return;
    if (u->h) {
        libusb_release_interface(u->h, u->intf);
        libusb_close(u->h);
    }
    free(u);
}
