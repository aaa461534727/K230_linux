/* cnxn_try.c —— 试探手机 adbd 对 CNXN 的接受条件。
 * 变量两个: ① 24B 头 + 7B 载荷 是一个 URB 还是两个 URB  ② data_check 用 crc32 还是字节和
 * 编译: gcc -O2 -o cnxn_try cnxn_try.c -lusb-1.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

static uint32_t crc32_calc(const uint8_t *p, int n)
{
    static uint32_t tab[256]; static int init;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); tab[i] = c; } init = 1; }
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t sum8(const uint8_t *p, int n) { uint32_t s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }
static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }

int main(int argc, char **argv)
{
    libusb_context *ctx = NULL; libusb_init(&ctx);
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
    if (!h) { printf("没找到设备\n"); return 2; }
    libusb_set_auto_detach_kernel_driver(h, 1);
    int rc = libusb_claim_interface(h, iface);
    printf("claim iface %d rc=%d\n", iface, rc);
    libusb_set_interface_alt_setting(h, iface, 0);
    libusb_clear_halt(h, 0x83); libusb_clear_halt(h, 0x02);

    const char *names[] = { "单URB+crc32", "单URB+和", "拆两步+crc32", "拆两步+和" };
    int only = argc > 1 ? atoi(argv[1]) : -1;
    for (int t = 0; t < 4; t++) {
        if (only >= 0 && t != only) continue;
        int split = (t >= 2), use_sum = (t & 1);
        uint8_t hdr[24], pay[16];
        const char *banner = "host::";
        int plen = 7;
        memcpy(pay, banner, plen);
        put32(hdr + 0, 0x4e584e43u);
        put32(hdr + 4, 0x01000001u);
        put32(hdr + 8, 256 * 1024);
        put32(hdr + 12, plen);
        put32(hdr + 16, use_sum ? sum8(pay, plen) : crc32_calc(pay, plen));
        put32(hdr + 20, 0x4e584e43u ^ 0xffffffffu);
        int s1 = 0, s2 = 0;
        int r1;
        if (split) {
            r1 = libusb_bulk_transfer(h, 0x02, hdr, 24, &s1, 1000);
            int r2 = libusb_bulk_transfer(h, 0x02, pay, plen, &s2, 1000);
            printf("\n--- %s: hdr rc=%d/%d pay rc=%d/%d\n", names[t], r1, s1, r2, s2);
        } else {
            uint8_t all[64]; memcpy(all, hdr, 24); memcpy(all + 24, pay, plen);
            r1 = libusb_bulk_transfer(h, 0x02, all, 24 + plen, &s1, 1000);
            printf("\n--- %s: 一次 rc=%d/%d\n", names[t], r1, s1);
        }
        int got = 0, ok = 0;
        for (int i = 0; i < 3; i++) {
            uint8_t buf[4096]; got = 0;
            rc = libusb_bulk_transfer(h, 0x83, buf, sizeof buf, &got, 1000);
            if (rc == 0 && got >= 24) {
                printf("  ← %d B cmd=0x%08x a0=%u a1=%u len=%u  <== 有应答\n",
                       got, get32(buf), get32(buf+4), get32(buf+8), get32(buf+12));
                ok = 1; break;
            }
            printf("  [%d] rc=%d got=%d\n", i, rc, got);
        }
        if (ok) { printf("  ✅ %s 被接受\n", names[t]); }
    }
    libusb_close(h); libusb_exit(ctx);
    return 0;
}
