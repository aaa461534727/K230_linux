/* mock_adbd_stream.c —— 端到端自测用: 在 unix socket 上假冒 adbd, 把指定
 * H.264 文件按 ADB WRTE 分片吐给 host。
 *
 * 用途: 没有手机时验证「usb_mirror 取流 -> 管道 -> v4l2_dec -> drmplay」整条链路:
 *   ./tools/mock_adbd_stream /tmp/mock.sock /tmp/sample.h264 &
 *   ./usb_mirror --test-unix /tmp/mock.sock | /usr/bin/v4l2_dec /dev/stdin /run/wfd-player.fifo -H -D
 *
 * 天然限速: host 侧 stdout 阻塞 (管道背压) 时不回 OKAY, 本程序就卡在 recv 上。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

static int g_fd;

static void put32(unsigned char *p, unsigned int v) { p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
static unsigned int get32(const unsigned char *p) { return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned)p[3]<<24); }

static int dev_send(unsigned int cmd, unsigned int a0, unsigned int a1, const void *d, int len)
{
    unsigned char h[24];
    put32(h+0, cmd); put32(h+4, a0); put32(h+8, a1);
    put32(h+12, len); put32(h+16, 0); put32(h+20, cmd ^ 0xffffffffu);
    if (write(g_fd, h, 24) != 24) return -1;
    if (len > 0 && write(g_fd, d, len) != len) return -1;
    return 0;
}

static int dev_recv(unsigned int *cmd, unsigned int *a0, unsigned int *a1, unsigned char *buf, int cap, int *len)
{
    unsigned char h[24];
    int got = 0;
    while (got < 24) { ssize_t n = read(g_fd, h+got, 24-got); if (n <= 0) return -1; got += (int)n; }
    *cmd = get32(h+0); *a0 = get32(h+4); *a1 = get32(h+8); *len = get32(h+12);
    if (get32(h+20) != (*cmd ^ 0xffffffffu)) return -1;
    if (*len > cap) return -1;
    got = 0;
    while (got < *len) { ssize_t n = read(g_fd, buf+got, *len-got); if (n <= 0) return -1; got += (int)n; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "用法: %s <socket路径> <file.h264> [分片字节=8192]\n", argv[0]); return 2; }
    const char *sock = argv[1], *file = argv[2];
    int chunk = argc > 3 ? atoi(argv[3]) : 8192;
    if (chunk < 64 || chunk > 256 * 1024) chunk = 8192;

    FILE *f = fopen(file, "rb");
    if (!f) { perror("fopen"); return 1; }

    unlink(sock);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); return 1; }
    listen(ls, 1);
    fprintf(stderr, "[mockd] 监听 %s, 等 host…\n", sock);

    g_fd = accept(ls, NULL, NULL);
    if (g_fd < 0) { perror("accept"); return 1; }
    close(ls);
    fprintf(stderr, "[mockd] host 已连上\n");

    unsigned char *buf = malloc(chunk);
    unsigned int cmd, a0, a1; int len;

    if (dev_recv(&cmd, &a0, &a1, buf, chunk, &len) != 0 || cmd != 0x4e584e43u) {
        fprintf(stderr, "[mockd] 期待 CNXN\n"); return 1;
    }
    fprintf(stderr, "[mockd] CNXN ver=0x%08x maxdata=%u\n", a0, a1);

    /* 不需要鉴权 (真机会发 AUTH TOKEN, 这里直接放行, 测的是数据面) */
    const char banner[] = "device::ro.product.name=mock;ro.build.version.sdk=33;";
    dev_send(0x4e584e43u, 0x01000001u, 256 * 1024, banner, (int)sizeof banner);

    int rr = dev_recv(&cmd, &a0, &a1, buf, chunk, &len);
    if (rr != 0 || cmd != 0x4e45504fu) {   /* "OPEN" 小端 */
        fprintf(stderr, "[mockd] 期待 OPEN(0x4e45504f), 收到 0x%08x (recv rc=%d len=%d)\n", cmd, rr, len);
        return 1;
    }
    unsigned int host_id = a0, my_id = 700;
    fprintf(stderr, "[mockd] OPEN: %s\n", (char *)buf);
    dev_send(0x59414b4fu, my_id, host_id, NULL, 0);

    long total = 0; int pkts = 0;
    for (;;) {
        int n = (int)fread(buf, 1, chunk, f);
        if (n <= 0) break;
        if (dev_send(0x45545257u, my_id, host_id, buf, n) != 0) { fprintf(stderr, "[mockd] 写失败\n"); break; }
        total += n; pkts++;
        if (dev_recv(&cmd, &a0, &a1, buf, chunk, &len) != 0) { fprintf(stderr, "[mockd] host 断开\n"); break; }
        if (cmd != 0x59414b4fu) { fprintf(stderr, "[mockd] 期待 OKAY, 收到 0x%08x\n", cmd); break; }
        if (pkts % 64 == 0) fprintf(stderr, "[mockd] 已发 %ld B / %d 包\n", total, pkts);
    }
    dev_send(0x45534c43u, my_id, host_id, NULL, 0);
    fprintf(stderr, "[mockd] 结束: 共 %ld 字节 / %d 包 (分片 %d B)\n", total, pkts, chunk);
    fclose(f);
    free(buf);
    close(g_fd);
    unlink(sock);
    return 0;
}

