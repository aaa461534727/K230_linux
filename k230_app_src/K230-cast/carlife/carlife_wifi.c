/*
 * carlife_wifi.c —— 无线 CarLife (WiFi) 发现 + 通道建链
 *
 * ★ 协议来源 (2026-10-30 从手机端 APK 逆向确认, 不是猜的):
 *   反编译 com.baidu.carlife 6.0.0 的
 *     com.baidu.carlife.core.connect.e   (ConnectManager)
 *     com.baidu.carlife.core.connect.g   (ConnectSocket)
 *     com.baidu.carlife.core.connect.c   (CarlifeCmdMessage)
 *   得到的事实:
 *
 *   1) 发现: 手机每 2 s 向本网段广播地址的 **UDP 7999** 发 ASCII "CarlifeHost"(11 B)。
 *      —— 见 e.j() 里的 TimerTask; 手机只发不收, 车机只要**听到**就知道手机 IP
 *         (取报文源地址), 不需要回包。
 *   2) 车机是 client, 手机是 server: ConnectManager.c() 在手机侧起了 7 个
 *      ServerSocket, 车机要主动连过去:
 *          7240 Cmd   8240 Video  9240 Media  9241 TTS
 *          9242 VR    9340 Touch  9440 Data
 *      (端口表 = com.baidu.carlife.core.f 的 jB..jH; 名字 = jJ..jP)
 *   3) 手机侧计数: 除 Data 外 6 个 socket 都连上才算"已连接"
 *      (e.a(g): if (!name.equals("Data")) B++; if (B >= 6) 通知已连接)。
 *   4) 每条 TCP 连接只承载**一个通道**, 因此没有 USB/AOA 的 8 字节外层头
 *      (channel|len), 只有 8 字节内层头: u16 len | u16 reserved | u32 msg
 *      (c.a(byte[8]) 解析的就是这 8 字节; 长度上限 32768)。
 *   5) Video/Media 通道直接写裸字节 (g.a(byte[],int)), 不带任何头:
 *      Video = H.264 Annex-B 码流; Media = PCM。
 */
#include "carlife.h"
#include "carlife_io.h"
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

#define HELLO_PORT  7999
#define HELLO_MAGIC "CarlifeHost"

int cl_wifi_wait_hello(char *ip_out, size_t cap, int timeout_s, int log_fd)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { dprintf(log_fd, "[wifi] udp socket 失败: %s\n", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(HELLO_PORT);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        dprintf(log_fd, "[wifi] bind udp :%d 失败: %s\n", HELLO_PORT, strerror(errno));
        close(s);
        return -1;
    }
    dprintf(log_fd, "[wifi] 监听 UDP :%d 等手机广播 \"CarlifeHost\" (最多 %d s)\n",
            HELLO_PORT, timeout_s);

    time_t deadline = time(NULL) + timeout_s;
    for (;;) {
        fd_set rf;
        FD_ZERO(&rf); FD_SET(s, &rf);
        struct timeval tv = { 2, 0 };
        int pr = select(s + 1, &rf, NULL, NULL, &tv);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) {
            if (time(NULL) > deadline) break;
            continue;
        }
        uint8_t buf[256];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        if ((size_t)r < strlen(HELLO_MAGIC) || memcmp(buf, HELLO_MAGIC, strlen(HELLO_MAGIC)) != 0)
            continue;
        if (!inet_ntop(AF_INET, &from.sin_addr, ip_out, (socklen_t)cap)) continue;
        dprintf(log_fd, "[wifi] 收到 %s 的发现广播 (%d B)\n", ip_out, (int)r);
        close(s);
        return 0;
    }
    close(s);
    return -1;
}

int cl_wifi_connect(const char *ip, int port, int timeout_s, int log_fd)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }

    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno != EINPROGRESS) {
        dprintf(log_fd, "[wifi] 连 %s:%d 失败: %s\n", ip, port, strerror(errno));
        close(s);
        return -1;
    }
    if (rc != 0) {
        fd_set wf;
        FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval tv = { timeout_s, 0 };
        int pr = select(s + 1, NULL, &wf, NULL, &tv);
        if (pr <= 0) { dprintf(log_fd, "[wifi] 连 %s:%d 超时\n", ip, port); close(s); return -1; }
        int err = 0; socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) { dprintf(log_fd, "[wifi] 连 %s:%d 失败: %s\n", ip, port, strerror(err)); close(s); return -1; }
    }
    fcntl(s, F_SETFL, fl);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* 解码器是 fork+exec 起的, 继承 fd 的话会话断了它也攥着连接不放
     * (手机侧看不到断链, 不会再广播) ⇒ 设 CLOEXEC, exec 时自动关掉。 */
    fcntl(s, F_SETFD, FD_CLOEXEC);
    /* 手机换网 (例如从办公 Wi-Fi 切到本机热点) 时它对端直接消失, 不等 FIN, 连接会
     * 一直挂在 ESTAB, 我们就永远等不到新的广播 ⇒ 开 keepalive 让内核主动探活:
     * 5 s 空闲起探, 每 3 s 一次, 3 次不回就报错, 约 14 s 内结束会话回去重新听广播。 */
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#ifdef TCP_KEEPIDLE
    int idle = 5, intvl = 3, cnt = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
#endif
    return s;
}
