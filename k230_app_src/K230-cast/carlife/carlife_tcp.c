/*
 * carlife_tcp.c —— 无线 CarLife 传输层 (WiFi)
 *
 * ⚠ 百度没有公开"无线 CarLife 的发现方式和端口"。本文件只实现**传输与帧复用**:
 *    手机连上 K230 的 AP 后, 只要能对我们这个 TCP 端口建链并收发同样的
 *    CarLife 帧 (channel/len 外层), 上层会话就能直接跑通 USB 那一套逻辑。
 *    端口/发现方式后续按真机抓包再定, 先用 --tcp <port> 可配。
 */
#include "carlife_io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    char peer[64];
} tcp_ctx;

static int tcp_rd(void *ctx, uint8_t *buf, size_t cap, int timeout_ms)
{
    tcp_ctx *t = ctx;
    fd_set rf;
    FD_ZERO(&rf); FD_SET(t->fd, &rf);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int pr = select(t->fd + 1, &rf, NULL, NULL, &tv);
    if (pr == 0) return 0;
    if (pr < 0) return -1;
    ssize_t r = recv(t->fd, buf, cap, 0);
    if (r == 0) return -1;               /* 对端关链 */
    if (r < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return (int)r;
}

static int tcp_wr(void *ctx, const uint8_t *buf, size_t n)
{
    tcp_ctx *t = ctx;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(t->fd, buf + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return (int)off;
}

cl_io *cl_tcp_listen_accept(int port, int log_fd, int timeout_s)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return NULL;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        dprintf(log_fd, "[wifi] bind :%d 失败: %s\n", port, strerror(errno));
        close(s);
        return NULL;
    }
    listen(s, 1);
    dprintf(log_fd, "[wifi] 监听 TCP :%d, 等手机接入 (最多 %d s)\n", port, timeout_s);

    fd_set rf;
    FD_ZERO(&rf); FD_SET(s, &rf);
    struct timeval tv = { timeout_s, 0 };
    int pr = select(s + 1, &rf, NULL, NULL, &tv);
    if (pr <= 0) { close(s); return NULL; }

    struct sockaddr_in pa; socklen_t pl = sizeof pa;
    int c = accept(s, (struct sockaddr *)&pa, &pl);
    close(s);
    if (c < 0) return NULL;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    tcp_ctx *t = calloc(1, sizeof *t);
    t->fd = c;
    snprintf(t->peer, sizeof t->peer, "%s:%u", inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
    dprintf(log_fd, "[wifi] 客户端已接入: %s\n", t->peer);

    cl_io *io = calloc(1, sizeof *io);
    io->rd = tcp_rd; io->wr = tcp_wr; io->ctx = t;
    return io;
}

cl_io *cl_tcp_wrap_fd(int fd, const char *peer)
{
    if (fd < 0) return NULL;
    tcp_ctx *t = calloc(1, sizeof *t);
    if (!t) { close(fd); return NULL; }
    t->fd = fd;
    snprintf(t->peer, sizeof t->peer, "%s", peer ? peer : "?");
    cl_io *io = calloc(1, sizeof *io);
    if (!io) { close(fd); free(t); return NULL; }
    io->rd = tcp_rd; io->wr = tcp_wr; io->ctx = t;
    return io;
}

const char *cl_tcp_peer(cl_io *io)
{
    return io && io->ctx ? ((tcp_ctx *)io->ctx)->peer : "?";
}

void cl_tcp_close(cl_io *io)
{
    if (!io) return;
    tcp_ctx *t = io->ctx;
    if (t) { close(t->fd); free(t); }
    free(io);
}
