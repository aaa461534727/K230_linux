/*
 * carlife_io.h —— 传输层抽象 (USB AOA / TCP)
 *
 * 有线和无线只差"字节怎么走", 上层帧/会话逻辑完全共用:
 *   有线: cl_io 由 carlife_usb.c 提供 (bulk IN/OUT)
 *   无线: cl_io 由 carlife_tcp.c 提供 (一条 TCP 连接)
 */
#ifndef CARLIFE_IO_H
#define CARLIFE_IO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int   (*rd)(void *ctx, uint8_t *buf, size_t cap, int timeout_ms);
    int   (*wr)(void *ctx, const uint8_t *buf, size_t n);
    void   *ctx;
} cl_io;

/* TCP: 监听并接受一个客户端, 返回 io 句柄 (失败 NULL) */
cl_io *cl_tcp_listen_accept(int port, int log_fd, int timeout_s);
/* TCP: 把已连好的 fd 包成 io 句柄 (无线每通道一条连接时用) */
cl_io *cl_tcp_wrap_fd(int fd, const char *peer);
void   cl_tcp_close(cl_io *io);
const char *cl_tcp_peer(cl_io *io);

/* ---- 无线: UDP 7999 发现 + 每通道 TCP 建链 (carlife_wifi.c) ---- */
/* 等到手机广播 "CarlifeHost", 把源 IP 写进 ip_out; 0=成功, -1=超时/错误 */
int cl_wifi_wait_hello(char *ip_out, size_t cap, int timeout_s, int log_fd);
/* 连到手机某个通道端口, 返回 fd; -1=失败 */
int cl_wifi_connect(const char *ip, int port, int timeout_s, int log_fd);

/* USB: 把 cl_usb 包成 cl_io */
struct cl_usb;
cl_io *cl_usb_as_io(struct cl_usb *u);
void   cl_io_free(cl_io *io);

#endif
