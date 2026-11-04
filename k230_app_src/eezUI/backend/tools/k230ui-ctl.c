/**
 * k230ui-ctl.c —— 命令行调试客户端 (板端/本机都能用, 不依赖 python/nc)
 *
 * 干什么: 手动给 k230-ui-backend 发一条命令并打印回复, 用来在没 UI 的情况下
 *         验证后端 / 排查"点了没反应"。
 * 怎么用:
 *   k230ui-ctl STATUS
 *   k230ui-ctl SELECT miracast
 *   k230ui-ctl STOP
 *   k230ui-ctl AUTO
 *   k230ui-ctl --socket /tmp/k230-ui.sock STATUS    # 指定 socket
 * 依赖  : 无 (纯 libc)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#define SOCK_DEFAULT "/run/k230-ui/backend.sock"

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [--socket PATH] STATUS | SELECT <svc> | STOP | AUTO | PING\n"
            "默认 socket: %s\n", a0, SOCK_DEFAULT);
}

int main(int argc, char **argv)
{
    const char *sockpath = SOCK_DEFAULT;
    char cmd[128] = "";
    int i, fd, rc;
    struct sockaddr_un addr;
    struct pollfd pfd;
    char reply[512];
    ssize_t n;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            sockpath = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            if (cmd[0]) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
    }
    if (!cmd[0]) {
        usage(argv[0]);
        return 1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect %s: %s (后端没起来?)\n", sockpath, strerror(errno));
        close(fd);
        return 1;
    }

    {
        char line[160];
        snprintf(line, sizeof(line), "%s\n", cmd);
        if (write(fd, line, strlen(line)) < 0) {
            fprintf(stderr, "write: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    rc = poll(&pfd, 1, 5000);
    if (rc <= 0) {
        fprintf(stderr, "timeout waiting reply\n");
        close(fd);
        return 1;
    }

    n = recv(fd, reply, sizeof(reply) - 1, 0);
    if (n <= 0) {
        fprintf(stderr, "recv: %s\n", n == 0 ? "closed" : strerror(errno));
        close(fd);
        return 1;
    }
    reply[n] = '\0';
    fputs(reply, stdout);
    close(fd);
    return strncmp(reply, "ERR", 3) == 0 ? 2 : 0;
}
