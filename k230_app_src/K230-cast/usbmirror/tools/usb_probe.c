/* usb_probe.c —— 排查 "adb 握手没人答" 到底卡在 USB 哪一层
 *
 * 按顺序做, 每步都打印事实:
 *   1. 枚举所有设备, 列出 config/iface/alt/endpoint (找 adb 接口)
 *   2. 控制传输 GET_DESCRIPTOR(device)      —— USB 链路是否通
 *   3. claim + SET_INTERFACE(alt=0)
 *   4. 读端点 halt 状态 (clear_halt 的返回值) —— 端点是否使能
 *   5. bulk OUT 一个标准 CNXN, 打印实际字节  —— 发得出去吗
 *   6. bulk IN 轮询 10 s, 打印每次结果         —— 设备到底回不回
 *   7. 再 bulk OUT 一个全 0 的 512 B, 看是否 STALL (端点真通 = 不 STALL)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libusb-1.0/libusb.h>

static void dump_dev(libusb_device *dev)
{
    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);
    printf("  %04x:%04x bus=%d addr=%d\n", dd.idVendor, dd.idProduct,
           libusb_get_bus_number(dev), libusb_get_device_address(dev));
    for (int ci = 0; ci < dd.bNumConfigurations; ci++) {
        struct libusb_config_descriptor *cfg;
        if (libusb_get_config_descriptor(dev, ci, &cfg) != 0) continue;
        printf("    cfg%d: niface=%d\n", ci, cfg->bNumInterfaces);
        for (int i = 0; i < cfg->bNumInterfaces; i++)
            for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];
                printf("      if%d alt%d: cls=%02x sub=%02x proto=%02x nep=%d\n",
                       id->bInterfaceNumber, id->bAlternateSetting,
                       id->bInterfaceClass, id->bInterfaceSubClass,
                       id->bInterfaceProtocol, id->bNumEndpoints);
                for (int e = 0; e < id->bNumEndpoints; e++)
                    printf("        ep 0x%02x attr=0x%02x mps=%d\n",
                           id->endpoint[e].bEndpointAddress, id->endpoint[e].bmAttributes,
                           id->endpoint[e].wMaxPacketSize);
            }
        libusb_free_config_descriptor(cfg);
    }
}

static uint32_t crc32_calc(const unsigned char *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xFFFFFFFFu;
}

static void put32(unsigned char *p, uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }

int main(int argc, char **argv)
{
    int want_vid = 0x19d2, want_pid = 0xffcf;
    if (argc >= 3) { sscanf(argv[1], "%x", &want_vid); sscanf(argv[2], "%x", &want_pid); }

    libusb_context *ctx;
    libusb_init(&ctx);
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    printf("=== 1. 设备枚举 (%zd 个) ===\n", n);
    libusb_device *target = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        if (dd.idVendor == 0x1d6b) continue;          /* 跳过 root hub */
        dump_dev(list[i]);
        if (dd.idVendor == want_vid && dd.idProduct == want_pid) target = list[i];
    }
    if (!target) { printf("\n✗ 没找到 %04x:%04x\n", want_vid, want_pid); return 1; }

    libusb_device_handle *h;
    int rc = libusb_open(target, &h);
    printf("\n=== 2. open = %d (%s) ===\n", rc, libusb_error_name(rc));
    if (rc != 0) return 1;

    unsigned char dbuf[18];
    rc = libusb_get_descriptor(h, 0x01, 0, dbuf, 18);
    printf("=== 2b. GET_DESCRIPTOR(device) = %d  bcdUSB=%02x%02x ===\n", rc, dbuf[3], dbuf[2]);

    libusb_set_auto_detach_kernel_driver(h, 1);
    rc = libusb_claim_interface(h, 1);
    printf("=== 3. claim iface 1 = %d (%s) ===\n", rc, libusb_error_name(rc));
    if (rc != 0) return 1;

    rc = libusb_set_interface_alt_setting(h, 1, 0);
    printf("=== 3b. SET_INTERFACE(1,0) = %d (%s) ===\n", rc, libusb_error_name(rc));

    rc = libusb_clear_halt(h, 0x83);
    printf("=== 4. clear_halt(IN 0x83) = %d (%s) ===\n", rc, libusb_error_name(rc));
    rc = libusb_clear_halt(h, 0x02);
    printf("=== 4b. clear_halt(OUT 0x02) = %d (%s) ===\n", rc, libusb_error_name(rc));

    /* 5. CNXN */
    const char banner[] = "host::";
    int blen = (int)sizeof banner;
    unsigned char pkt[24 + 64];
    put32(pkt+0, 0x4e584e43u);
    put32(pkt+4, 0x01000001u);
    put32(pkt+8, 32768);
    put32(pkt+12, blen);
    put32(pkt+16, crc32_calc((const unsigned char *)banner, blen));
    put32(pkt+20, 0x4e584e43u ^ 0xFFFFFFFFu);
    memcpy(pkt+24, banner, blen);
    int sent = 0;
    rc = libusb_bulk_transfer(h, 0x02, pkt, 24 + blen, &sent, 3000);
    printf("=== 5. bulk OUT CNXN (24+%d B) = %d (%s) sent=%d ===\n", blen, rc, libusb_error_name(rc), sent);

    /* 6. 轮询 IN */
    printf("=== 6. 轮询 IN 0x83, 10 s ===\n");
    int total = 0;
    for (int i = 0; i < 10; i++) {
        unsigned char buf[512];
        int got = 0;
        rc = libusb_bulk_transfer(h, 0x83, buf, sizeof buf, &got, 1000);
        if (rc == 0 && got > 0) {
            printf("    [%2d s] rc=0 got=%d  cmd=0x%08x a0=%u a1=%u len=%u\n", i, got,
                   buf[0]|(buf[1]<<8)|(buf[2]<<16)|((uint32_t)buf[3]<<24),
                   buf[4]|(buf[5]<<8)|(buf[6]<<16)|((uint32_t)buf[7]<<24),
                   buf[8]|(buf[9]<<8)|(buf[10]<<16)|((uint32_t)buf[11]<<24),
                   buf[12]|(buf[13]<<8)|(buf[14]<<16)|((uint32_t)buf[15]<<24));
            total += got;
        } else {
            printf("    [%2d s] rc=%d (%s) got=%d\n", i, rc, libusb_error_name(rc), got);
        }
    }
    printf("    共收到 %d 字节\n", total);

    /* 7. 端点真通测试: 发 512 B 全 0, 期望 rc=0 (若 STALL 会 -9 PIPE) */
    {
        unsigned char z[512]; memset(z, 0, sizeof z);
        int s2 = 0;
        rc = libusb_bulk_transfer(h, 0x02, z, sizeof z, &s2, 3000);
        printf("=== 7. bulk OUT 512B 全 0 = %d (%s) sent=%d ===\n", rc, libusb_error_name(rc), s2);
    }

    libusb_release_interface(h, 1);
    libusb_close(h);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}

