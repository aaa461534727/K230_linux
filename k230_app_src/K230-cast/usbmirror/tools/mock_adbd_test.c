/* mock_adbd_test.c —— 用一个 socketpair 假冒 adbd, 在没有手机的情况下把
 * adb.c 的握手 / 鉴权 / 开通道 / 分包 全部跑一遍 (主机上 make -C tools test 即可)。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "../adb.h"

static int g_fd;
static int g_mode;          /* 0 = 先要求鉴权; 1 = 直接 CNXN */
static int g_hdr_only;

static void put32(uint8_t *p, uint32_t v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static uint32_t get32(const uint8_t *p) { return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }

static int dev_send(uint32_t cmd, uint32_t a0, uint32_t a1, const void *d, int len)
{
    uint8_t h[24];
    put32(h+0, cmd); put32(h+4, a0); put32(h+8, a1);
    put32(h+12, len); put32(h+16, 0); put32(h+20, cmd ^ 0xFFFFFFFFu);
    if (write(g_fd, h, 24) != 24) return -1;
    if (len > 0 && write(g_fd, d, len) != len) return -1;
    return 0;
}

static int dev_recv(uint32_t *cmd, uint32_t *a0, uint32_t *a1, uint8_t *buf, int cap, int *len)
{
    uint8_t h[24];
    int got = 0;
    while (got < 24) { ssize_t n = read(g_fd, h+got, 24-got); if (n <= 0) return -1; got += n; }
    *cmd = get32(h+0); *a0 = get32(h+4); *a1 = get32(h+8); *len = get32(h+12);
    if (get32(h+20) != (*cmd ^ 0xFFFFFFFFu)) return -1;
    if (*len > cap) return -1;
    got = 0;
    while (got < *len) { ssize_t n = read(g_fd, buf+got, *len-got); if (n <= 0) return -1; got += n; }
    return 0;
}

static void *dev_thread(void *arg)
{
    (void)arg;
    uint8_t buf[1024 * 1024];
    uint32_t cmd, a0, a1; int len;

    /* 1) CNXN */
    if (dev_recv(&cmd, &a0, &a1, buf, sizeof buf, &len) != 0 || cmd != ADB_CMD_CNXN) { fprintf(stderr,"[mock] 期待 CNXN\n"); return NULL; }
    printf("[mock] 收到 CNXN ver=0x%08x maxdata=%u banner=%s\n", a0, a1, (char*)buf);

    if (g_mode == 0) {
        uint8_t tok[20]; for (int i = 0; i < 20; i++) tok[i] = (uint8_t)(i + 1);
        dev_send(ADB_CMD_AUTH, ADB_AUTH_TOKEN, 0, tok, 20);
        if (dev_recv(&cmd, &a0, &a1, buf, sizeof buf, &len) != 0) return NULL;
        if (cmd != ADB_CMD_AUTH || a0 != ADB_AUTH_SIGNATURE || len != 256) {
            fprintf(stderr, "[mock] 期待 AUTH 签名(256B), 实际 cmd=0x%08x a0=%u len=%d\n", cmd, a0, len);
            return NULL;
        }
        printf("[mock] 收到 256B RSA 签名 (sig[0..3]=%02x%02x%02x%02x), 视为通过\n", buf[0],buf[1],buf[2],buf[3]);
    }

    const char banner[] = "device::ro.product.name=mock;ro.build.version.sdk=33;";
    dev_send(ADB_CMD_CNXN, 0x01000001, 256 * 1024, banner, (int)sizeof banner);

    /* 2) 等 A_OPEN */
    int round = 0;
    for (;;) {
        if (dev_recv(&cmd, &a0, &a1, buf, sizeof buf, &len) != 0) break;
        if (cmd != ADB_CMD_OPEN) { fprintf(stderr, "[mock] 期待 A_OPEN, 收到 0x%08x\n", cmd); break; }
        uint32_t host_id = a0, my_id = 100 + round;
        printf("[mock] OPEN id=%u service=%s\n", host_id, (char*)buf);
        dev_send(ADB_CMD_OKAY, my_id, host_id, NULL, 0);

        /* 3) 送 3 个 WRTE (模拟 H.264 分片) + 收 OKAY */
        int total = 0;
        for (int i = 0; i < 3; i++) {
            int n = 1000 + i;
            memset(buf, 'A' + i, n);
            dev_send(ADB_CMD_WRTE, my_id, host_id, buf, n);
            total += n;
            if (dev_recv(&cmd, &a0, &a1, buf, sizeof buf, &len) != 0) return NULL;
            if (cmd != ADB_CMD_OKAY || a0 != host_id || a1 != my_id) {
                fprintf(stderr, "[mock] 期待 OKAY(%u,%u), 收到 0x%08x (%u,%u)\n", host_id, my_id, cmd, a0, a1);
                return NULL;
            }
        }
        printf("[mock] 已发 %d 字节并收到 3 个 OKAY\n", total);
        dev_send(ADB_CMD_CLSE, my_id, host_id, NULL, 0);
        round++;
        if (g_hdr_only || round >= 2) break;
    }
    return NULL;
}

static int host_run(int fd)
{
    adb_dev d;
    int rc = adb_connect_test(&d, fd, 5000, 1);
    if (rc != 0) { fprintf(stderr, "[host] connect 失败 %d (%s)\n", rc, adb_strerror(rc)); return 1; }

    long bytes = 0; int closes = 0;
    while (closes < 2) {
        uint32_t remote = 0;
        rc = adb_open_channel(&d, 1, "exec:screenrecord --output-format=h264 --bit-rate 8000000 -", &remote, 5000);
        if (rc != 0) { fprintf(stderr, "[host] open 失败 %d\n", rc); return 1; }
        printf("[host] 通道已开 remote=%u\n", remote);
        for (;;) {
        uint32_t cmd, a0, a1; uint8_t *data; int len;
            if (adb_recv_packet(&d, 3000, &cmd, &a0, &a1, &data, &len) < 0) { fprintf(stderr, "[host] recv 失败\n"); return 1; }
            if (cmd == ADB_CMD_WRTE && a1 == 1) { bytes += len; adb_send_okay(&d, a1, a0); }
            else if (cmd == ADB_CMD_CLSE && a1 == 1) { closes++; adb_free_packet(data); break; }
            adb_free_packet(data);
        }
        usleep(150000);   /* 模拟 screenrecord 180s 上限后的重开间隔 */
    }
    printf("[host] 共收到 %ld 字节, 校验 %s\n", bytes, bytes == 2 * (1000 + 1001 + 1002) ? "OK" : "FAIL");
    adb_disconnect(&d);
    return bytes == 2 * 3003 ? 0 : 1;
}

int main(int argc, char **argv)
{
    g_mode = (argc > 1 && !strcmp(argv[1], "--noauth")) ? 1 : 0;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }
    g_fd = sv[1];
    pthread_t th;
    pthread_create(&th, NULL, dev_thread, NULL);
    int rc = host_run(sv[0]);
    pthread_join(th, NULL);
    printf("\n结果: %s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}

