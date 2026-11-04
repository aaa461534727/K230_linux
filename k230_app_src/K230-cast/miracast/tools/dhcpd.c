/******************************************************************************
 * dhcpd.c — 极简 DHCP 服务器 (给 Miracast P2P GO 用)
 *
 * 为什么要有它: K230 当 P2P GO 时, 手机作为 client 加入组后必须拿到 IP 才能连
 * 192.168.49.1:7236 的 RTSP sink。镜像里既没有 dnsmasq/udhcpd/busybox,
 * 板上的 wpa_supplicant 也没编 CONFIG_DHCP_SERVER, 所以自己写一个单文件版。
 *
 * 特点:
 *   - 零依赖, 只用 POSIX socket, 静态链接可直接塞固件
 *   - 只做 DISCOVER→OFFER / REQUEST→ACK (以及 RELEASE/DECLINE), 不做 relay/多子网
 *   - 地址池默认 192.168.49.10 起, 最多 16 个租约, 租期 1 小时
 *   - 全部回包走广播 (P2P 场景最简单也最稳)
 *
 * 用法:
 *   dhcpd -i wlan0 [-a 192.168.49.1] [-p 192.168.49.10] [-n 16] [-t 3600] [-v]
 *     -i  GO 网卡 (必需; 用 SO_BINDTODEVICE 锁死, 不会污染其他网卡)
 *     -a  服务器地址/网关/DNS (默认 192.168.49.1)
 *     -p  地址池起始 (默认 192.168.49.10)
 *     -n  池大小 (默认 16, 上限 64)
 *     -t  租期秒 (默认 3600)
 *     -v  每包都打日志
 *
 * 编译 (K230 riscv64, 静态):
 *   <toolchain>/riscv64-unknown-linux-gnu-gcc -O2 -static -o dhcpd dhcpd.c
 ******************************************************************************/
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define MAX_LEASES       64
#define PKT_BUF          1024

/* BOOTP + DHCP 固定头 (后面跟 options) */
struct dhcp_pkt {
    uint8_t  op;      /* 1=BOOTREQUEST 2=BOOTREPLY */
    uint8_t  htype;   /* 1 = ethernet */
    uint8_t  hlen;    /* 6 */
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  opts[312];
} __attribute__((packed));

#define DHCP_MAGIC 0x63825363u

enum { DHCPDISCOVER = 1, DHCPOFFER = 2, DHCPREQUEST = 3, DHCPDECLINE = 4,
       DHCPACK = 5, DHCPNAK = 6, DHCPRELEASE = 7, DHCPINFORM = 8 };

struct lease {
    uint8_t  mac[6];
    uint32_t ip;        /* host order */
    time_t   expires;
    int      used;
};

static struct lease leases[MAX_LEASES];
static int      n_leases;          /* 池大小 */
static uint32_t pool_start;        /* host order */
static uint32_t server_ip;         /* host order */
static uint32_t netmask;           /* host order */
static int      g_lease_sec = 3600;
static int      verbose;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint32_t ip2h(uint32_t net) { return ntohl(net); }

static void mac_str(const uint8_t *m, char *out, size_t n)
{
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* 找一个该 MAC 的租约 (没有就新建; 池满返回 0) */
static uint32_t lease_get_or_alloc(const uint8_t *mac)
{
    time_t now = time(NULL);
    int i, free_slot = -1;

    for (i = 0; i < n_leases; i++) {
        if (leases[i].used && !memcmp(leases[i].mac, mac, 6)) {
            leases[i].expires = now + g_lease_sec;
            return leases[i].ip;
        }
    }
    for (i = 0; i < n_leases; i++) {
        if (!leases[i].used || leases[i].expires < now) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0)
        return 0;   /* 池满 */

    memcpy(leases[free_slot].mac, mac, 6);
    leases[free_slot].ip = pool_start + (uint32_t)free_slot;
    leases[free_slot].expires = now + g_lease_sec;
    leases[free_slot].used = 1;
    return leases[free_slot].ip;
}

static void lease_free(const uint8_t *mac)
{
    int i;
    for (i = 0; i < n_leases; i++)
        if (leases[i].used && !memcmp(leases[i].mac, mac, 6))
            leases[i].used = 0;
}

/* 取 DHCP option 53 (message type); 找不到返回 0 */
static int pkt_msg_type(const struct dhcp_pkt *p)
{
    int i = 0;
    while (i < 312) {
        uint8_t code = p->opts[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        if (i + 1 >= 312) break;
        uint8_t len = p->opts[i + 1];
        if (code == 53 && len >= 1) return p->opts[i + 2];
        i += 2 + len;
    }
    return 0;
}

static uint8_t *put_opt(uint8_t *o, uint8_t code, uint8_t len, const void *val)
{
    *o++ = code;
    *o++ = len;
    memcpy(o, val, len);
    return o + len;
}

static uint8_t *put_u8(uint8_t *o, uint8_t code, uint8_t v)  { return put_opt(o, code, 1, &v); }
static uint8_t *put_u32(uint8_t *o, uint8_t code, uint32_t host_v)
{
    uint32_t be = htonl(host_v);
    return put_opt(o, code, 4, &be);
}

/* 构造并发出 OFFER / ACK */
static int send_reply(int fd, const struct dhcp_pkt *req, uint8_t msg_type,
                      uint32_t yiaddr_host)
{
    uint8_t buf[PKT_BUF];
    struct dhcp_pkt *r = (struct dhcp_pkt *)buf;
    struct sockaddr_in dst;
    uint8_t *o;
    char mac[20];

    memset(buf, 0, sizeof(buf));
    r->op     = 2;                  /* BOOTREPLY */
    r->htype  = 1;
    r->hlen   = 6;
    r->xid    = req->xid;
    r->flags  = req->flags ? req->flags : 0x8000;   /* 要求广播 */
    r->yiaddr = htonl(yiaddr_host);
    r->siaddr = htonl(server_ip);
    memcpy(r->chaddr, req->chaddr, 16);
    r->magic  = htonl(DHCP_MAGIC);

    o = r->opts;
    o = put_u8(o, 53, msg_type);
    o = put_u32(o, 54, server_ip);            /* server identifier */
    o = put_u32(o, 51, (uint32_t)g_lease_sec); /* lease time */
    o = put_u32(o, 1, netmask);               /* subnet mask */
    o = put_u32(o, 3, server_ip);             /* router */
    o = put_u32(o, 6, server_ip);             /* dns */
    o = put_u32(o, 28, server_ip | ~netmask); /* broadcast */
    *o++ = 255;

    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(DHCP_CLIENT_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (sendto(fd, r, (size_t)(o - buf), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;

    mac_str(req->chaddr, mac, sizeof(mac));
    logmsg("[dhcpd] %s → %s %u.%u.%u.%u (xid=0x%08x)",
           msg_type == DHCPOFFER ? "OFFER" : "ACK", mac,
           (yiaddr_host >> 24) & 0xff, (yiaddr_host >> 16) & 0xff,
           (yiaddr_host >> 8) & 0xff, yiaddr_host & 0xff, req->xid);
    return 0;
}

static volatile sig_atomic_t g_stop;

static void on_sig(int s) { (void)s; g_stop = 1; }

/* 用 sigaction 且不带 SA_RESTART: 否则 recvfrom 会被自动重启, SIGTERM 永远退不出去
 * (2026-10-21 踩过: pkill 后进程一直在, 结果同时跑了 3 个 dhcpd) */
static void install_sig(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

int main(int argc, char **argv)
{
    const char *ifname = NULL;
    const char *addr_s = "192.168.49.1";
    const char *pool_s = "192.168.49.10";
    int pool_n = 16, lease_sec = 3600, opt;
    int fd;
    struct sockaddr_in sa;
    struct in_addr ia;

    while ((opt = getopt(argc, argv, "i:a:p:n:t:vh")) != -1) {
        switch (opt) {
        case 'i': ifname = optarg; break;
        case 'a': addr_s = optarg; break;
        case 'p': pool_s = optarg; break;
        case 'n': pool_n  = atoi(optarg); break;
        case 't': lease_sec = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr,
                    "用法: %s -i <ifname> [-a 192.168.49.1] [-p 192.168.49.10] "
                    "[-n 16] [-t 3600] [-v]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (lease_sec > 0) g_lease_sec = lease_sec;
    if (!ifname) { fprintf(stderr, "必须用 -i 指定 GO 网卡\n"); return 1; }
    if (pool_n < 1) pool_n = 1;
    if (pool_n > MAX_LEASES) pool_n = MAX_LEASES;

    if (inet_aton(addr_s, &ia) == 0) { fprintf(stderr, "server 地址非法: %s\n", addr_s); return 1; }
    server_ip = ip2h(ia.s_addr);
    if (inet_aton(pool_s, &ia) == 0) { fprintf(stderr, "池起始非法: %s\n", pool_s); return 1; }
    pool_start = ip2h(ia.s_addr);
    netmask    = 0xffffff00u;      /* /24 */
    n_leases   = pool_n;

    /* 单实例锁: UDP 上即使不开 SO_REUSEADDR, 带 SO_BINDTODEVICE 的多份仍能各自 bind 成功,
     * 会静默跑出好几个 dhcpd 抢包 (2026-10-21 踩过) ⇒ 用 flock 硬卡住 */
    {
        const char *lockfile = "/run/dhcpd.lock";
        int lfd = open(lockfile, O_CREAT | O_RDWR, 0644);
        if (lfd < 0) {
            perror("open lock");
        } else if (flock(lfd, LOCK_EX | LOCK_NB) < 0) {
            fprintf(stderr, "[dhcpd] 已经有一个 dhcpd 在跑 (%s 被锁住), 本次退出\n", lockfile);
            return 1;
        }
        /* lfd 故意不关: 进程退出时锁自动释放 */
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    {   /* 锁死网卡: 不让其他网卡的 DHCP 串进来 */
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)(strlen(ifname) + 1)) < 0) {
            perror("SO_BINDTODEVICE"); close(fd); return 1;
        }
    }
    /* 故意不开 SO_REUSEADDR: UDP 开了它会让第二个实例也能 bind 到 :67,
     * 静默跑出多个 dhcpd 抢包 (2026-10-21 踩过)。不开则第二份直接 EADDRINUSE。 */
    opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    {   /* 1s 超时: 让 g_stop 每秒钟都有机会被检查到 */
        struct timeval tv = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(DHCP_SERVER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind :67 (是不是别的 dhcp 服务占着?)");
        close(fd);
        return 1;
    }

    install_sig(SIGINT, on_sig);
    install_sig(SIGTERM, on_sig);

    logmsg("[dhcpd] 监听 %s :%d  server=%s  pool=%s +%d  (/24)",
           ifname, DHCP_SERVER_PORT, addr_s, pool_s, pool_n);

    while (!g_stop) {
        uint8_t buf[PKT_BUF];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n;
        struct dhcp_pkt *p = (struct dhcp_pkt *)buf;
        int type;
        uint32_t yi;
        char mac[20];

        n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;          /* 超时/被打断: 回去看 g_stop */
            perror("recvfrom");
            break;
        }
        if (n < (ssize_t)(sizeof(struct dhcp_pkt) - 312 + 4))   /* 头 + magic */
            continue;
        if (p->op != 1 || p->htype != 1 || p->hlen != 6)
            continue;
        if (ntohl(p->magic) != DHCP_MAGIC)
            continue;

        type = pkt_msg_type(p);
        mac_str(p->chaddr, mac, sizeof(mac));

        switch (type) {
        case DHCPDISCOVER:
            yi = lease_get_or_alloc(p->chaddr);
            if (!yi) { logmsg("[dhcpd] 池满, 拒绝 %s", mac); break; }
            if (verbose) logmsg("[dhcpd] DISCOVER %s", mac);
            send_reply(fd, p, DHCPOFFER, yi);
            break;

        case DHCPREQUEST:
            yi = lease_get_or_alloc(p->chaddr);
            if (!yi) { logmsg("[dhcpd] 池满, 拒绝 %s", mac); break; }
            if (verbose) logmsg("[dhcpd] REQUEST %s", mac);
            send_reply(fd, p, DHCPACK, yi);
            break;

        case DHCPRELEASE:
        case DHCPDECLINE:
            lease_free(p->chaddr);
            logmsg("[dhcpd] %s %s", type == DHCPRELEASE ? "RELEASE" : "DECLINE", mac);
            break;

        default:
            if (verbose) logmsg("[dhcpd] 忽略 msg type=%d (%s)", type, mac);
            break;
        }
    }

    logmsg("[dhcpd] 退出");
    close(fd);
    return 0;
}
