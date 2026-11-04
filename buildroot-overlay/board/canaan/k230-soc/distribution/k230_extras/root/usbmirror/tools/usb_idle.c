/* usb_idle.c —— 只 claim 接口 + 保持句柄, 不发任何 adb 包, 观察手机是否会重枚举。
 * 用来判断「手机隔一会儿掉线重来」是线/供电问题, 还是被我们的 adb 握手触发。
 *
 * 用法: ./usb_idle [秒=110]        正常结束会打印 USB 是否仍在
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

static long now_s(void) { return (long)time(NULL); }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 110;

    libusb_context *ctx = NULL;
    libusb_init(&ctx);
    libusb_device **list; ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *h = NULL; int iface = -1;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd)) continue;
        if (dd.idVendor != 0x19d2 || dd.idProduct != 0xffcf) continue;
        if (libusb_open(list[i], &h) != 0) continue;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg) == 0) {
            for (int k = 0; k < cfg->bNumInterfaces; k++)
                if (cfg->interface[k].altsetting[0].bInterfaceClass == 0xff &&
                    cfg->interface[k].altsetting[0].bInterfaceSubClass == 0x42) iface = k;
            libusb_free_config_descriptor(cfg);
        }
        break;
    }
    libusb_free_device_list(list, 1);
    if (!h) { printf("没找到手机\n"); return 2; }

    int rc = libusb_claim_interface(h, iface);
    printf("claim iface %d rc=%d (%s), 保持 %d s 不发包\n", iface, rc, libusb_error_name(rc), secs);
    fflush(stdout);

    long t0 = now_s();
    int lost = 0;
    while (now_s() - t0 < secs) {
        sleep(2);
        int got = 0;
        unsigned char b[8];
        int r = libusb_bulk_transfer(h, 0x83, b, sizeof b, &got, 1);
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            printf("[%3ld s] 设备掉了: %s\n", now_s() - t0, libusb_error_name(r));
            fflush(stdout);
            lost = 1;
            break;
        }
    }
    if (!lost) printf("[%3ld s] 全程在, 没重枚举\n", now_s() - t0);
    if (!lost) libusb_release_interface(h, iface);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
