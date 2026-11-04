/* arbiter_stub.c — K230 版"传输仲裁器"桩
 *
 * 上游 carplay-wireless (carplayd) 启动第一件事是连 /run/carplay/arbiter.sock 并 claim wireless,
 * 因为它的设计里"有线 CarPlay"和"无线 CarPlay"会抢同一套 iAP2 身份。
 * K230 的 dwc2 只有 host 模式 ⇒ **根本做不了有线 CarPlay** ⇒ 仲裁器退化成"永远批准 wireless"。
 *
 * 协议 (见 wireless/src/arbiter_client.rs):
 *   客户端 → "claim wireless\n"   服务端 → "ok\n"
 *   客户端断开 = 释放; 需要抢占时服务端推 "preempt\n" (本桩不发, 因为没有谁会来抢)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CP_ARBITER_SOCK "/run/carplay/arbiter.sock"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    mkdir("/run/carplay", 0755);
    unlink(CP_ARBITER_SOCK);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { LOGE("socket: %s", strerror(errno)); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CP_ARBITER_SOCK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind %s: %s", CP_ARBITER_SOCK, strerror(errno));
        return 1;
    }
    chmod(CP_ARBITER_SOCK, 0666);
    listen(srv, 4);
    LOGI("仲裁器桩就绪: %s (K230 只有无线, 一律批准)", CP_ARBITER_SOCK);

    while (!g_stop) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        char line[128] = { 0 };
        ssize_t n = read(c, line, sizeof(line) - 1);
        if (n > 0) {
            if (strncmp(line, "claim", 5) == 0) {
                const char ok[] = "ok\n";
                ssize_t w = write(c, ok, sizeof(ok) - 1);
                (void)w;
                LOGI("批准 claim: %s", line);
                /* 保持连接: 客户端断开即视为释放, 这里只管挂着 */
                char buf[64];
                while (read(c, buf, sizeof(buf)) > 0) { }
            }
        }
        close(c);
    }
    close(srv);
    unlink(CP_ARBITER_SOCK);
    return 0;
}
