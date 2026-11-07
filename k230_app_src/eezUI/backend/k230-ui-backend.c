/**
 * k230-ui-backend.c —— LVGL 主界面的后端守护进程
 *
 * 它解决什么问题:
 *   主界面 (k230-ui) 和投屏显示 (k230-screen/drmplay) 抢同一个 DRM master,
 *   不能同时跑; 而"谁该上屏"又要看用户点了哪个按钮、会话什么时候结束。
 *   本守护进程 = 唯一的决策者:
 *     · 对外只开一个 Unix socket (文本行协议), UI 只跟它说话, 不直接碰 systemd;
 *     · 收到 SELECT <svc> 只起该路服务、**不碰 DRM**: LVGL 二级画布继续占屏
 *       (waiting 态), 用户不会被刷黑, 也不会被宽限期弹回主界面;
 *     · 探到真实投屏会话建立 (端口/会话标记) 才切 video 模式: 停 k230-ui、起 drmplay 刷视频流;
 *     · 探到视频流断开再切回 wait 模式: 停 drmplay、重新拉起 k230-ui
 *       (靠 /run/k230-ui/active-page 自动回到刚才那个按钮的二级画布);
 *     · STOP = 回主界面 (唯一能离开二级画布的动作)。
 *
 * 状态: idle -> starting -> waiting <-> (starting) -> running -> waiting ... -> stopping -> idle
 *
 * 协议 (一行一条, 见 frontend/src/backend_client.h):
 *   请求  STATUS | SELECT <svc> | STOP | AUTO
 *   响应  OK state=<idle|starting|waiting|running|stopping|error|unknown> service=<svc|none> page=<svc|none> mode=<...> msg=<文本>
 *         ERR msg=<文本>
 *   page = 后端认为当前该显示的二级画布 (waiting/running 时 = service; idle 时 = none)
 *
 * 用法:
 *   k230-ui-backend                       # 板端 (systemd 拉起)
 *   k230-ui-backend --socket /tmp/x.sock --mode-cmd ./mock.sh   # 本机联调
 *   k230-ui-backend --no-init              # 启动时不自动进 ui 模式
 *   k230-ui-backend --verbose              # 每条请求打到 stderr (journal)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#define SOCK_DEFAULT  "/run/k230-ui/backend.sock"
#define SCRIPT_DEFAULT "/usr/bin/k230-ui-mode.sh"

#define PROBE_PERIOD_S 2.0   /* 多久探一次会话 (waiting 探开始 / running 探断开) */
#define CLIENT_READ_MS 500   /* 读客户端一行最多等多久 */
/* 连续几次探不到会话才算真断 (端口刚建/刚拆的瞬间会抖) */
#define PROBE_MISS_MAX 2

/* ---------------------------------------------------------------- 状态机 */
typedef enum {
    ST_UNKNOWN = 0,
    ST_IDLE,
    ST_STARTING,
    ST_WAITING,      /* 服务已起, 但屏仍归 LVGL 二级画布 (不刷黑) */
    ST_RUNNING,      /* 真实投屏中: 屏归 drmplay 视频流, k230ui 被停 */
    ST_STOPPING,
    ST_ERROR
} state_t;

static const char *state_str(state_t s)
{
    switch (s) {
    case ST_IDLE:     return "idle";
    case ST_STARTING: return "starting";
    case ST_WAITING:  return "waiting";
    case ST_RUNNING:  return "running";
    case ST_STOPPING: return "stopping";
    case ST_ERROR:    return "error";
    default:          return "unknown";
    }
}

static struct {
    state_t state;
    char    service[24];   /* miracast / airplay / carlife / carplay / none */
    char    page[24];      /* 该显示的二级画布 / none */
    char    message[128];
} g_st = { ST_UNKNOWN, "none", "none", "booting" };

/* 子进程 (模式脚本) 跟踪 */
static pid_t g_child = 0;
static char  g_child_next[24];       /* 脚本跑完后要进入的 service 名 ("" = 回 idle) */
static state_t g_child_next_state;   /* 脚本跑完后要进入的状态 */
static char  g_pending_svc[24];      /* 忙的时候收到的下一个请求 (最后一条胜出) */
/* 板上真正处在哪个模式: ui / wait:<svc> / video:<svc> / auto */
static char  g_mode[32] = "ui";
static int   g_pending = 0;          /* 0 无 / 1 = select / 2 = stop / 3 = auto */

/* 配置 */
static char  g_sock_path[108] = SOCK_DEFAULT;
static char  g_script[256] = SCRIPT_DEFAULT;
static int   g_verbose = 0;
static int   g_listen = -1;
static double g_last_probe = 0;
static int    g_probe_miss = 0;      /* running 态连续几次探不到会话 */

static const char *KNOWN_SERVICES[] = { "miracast", "airplay", "carlife", "carplay" };

/* ---------------------------------------------------------------- 小工具 */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void vlog(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_st.message, sizeof(g_st.message), fmt, ap);
    va_end(ap);
}

static int is_known_service(const char *s)
{
    size_t i;
    for (i = 0; i < sizeof(KNOWN_SERVICES) / sizeof(KNOWN_SERVICES[0]); i++)
        if (!strcmp(s, KNOWN_SERVICES[i])) return 1;
    return 0;
}

/* ---------------------------------------------------------------- 会话探针 */
/* 数 /proc/net/tcp{,6} 里 本地或远端端口属于 ports[] 且状态 ESTABLISHED 的连接数 */
static int conn_cnt(const int *ports, int nports)
{
    static const char *files[] = { "/proc/net/tcp", "/proc/net/tcp6" };
    int total = 0;
    size_t fi;

    for (fi = 0; fi < sizeof(files) / sizeof(files[0]); fi++) {
        FILE *f = fopen(files[fi], "r");
        char line[512];
        int first = 1;

        if (!f) continue;
        while (fgets(line, sizeof(line), f)) {
            char local[128], remote[128];
            unsigned st;
            char *colon;
            long lport, rport;
            int i;

            if (first) { first = 0; continue; }  /* 表头 */
            if (sscanf(line, "%*d: %127s %127s %x", local, remote, &st) != 3)
                continue;
            if (st != 0x01) continue;            /* 只要 ESTABLISHED */

            colon = strrchr(local, ':');
            lport = colon ? strtol(colon + 1, NULL, 16) : -1;
            colon = strrchr(remote, ':');
            rport = colon ? strtol(colon + 1, NULL, 16) : -1;

            for (i = 0; i < nports; i++) {
                if (lport == ports[i] || rport == ports[i]) { total++; break; }
            }
        }
        fclose(f);
    }
    return total;
}

static int file_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

/* 返回值: 1 = 真实投屏会话已建立 / 仍在, 0 = 还没连上 / 已断开。
 *
 * 为什么判据里**不含蓝牙反控**: 蓝牙是 AirPlay 用户在手机上先连的那一步,
 * 它连上不等于视频流开始; 反过来视频断开后蓝牙可能还连着, 若把它算进来
 * 就永远不会从 video 回到二级页。waiting 态只认「视频会话」信号。
 *
 * 这些端口/标记出现在实际出帧之前 (RTSP/会话建立那一拍), 正是切 video 的时机:
 * 后端一探到就停 k230-ui 起 drmplay, 解码器的 fifo 读者随即接上。 */
static int video_session_alive(const char *svc)
{
    static const int p_miracast[] = { 7236 };
    static const int p_airplay[]  = { 7100, 7101 };
    static const int p_carlife[]  = { 7240, 8240, 9240, 9241, 9242, 9340, 9440 };

    if (!strcmp(svc, "miracast"))
        return conn_cnt(p_miracast, 1) > 0;
    if (!strcmp(svc, "airplay"))
        return conn_cnt(p_airplay, 2) > 0 || file_exists("/run/airplay-cast-active");
    if (!strcmp(svc, "carplay"))
        /* 以 /run/airplay-active (帧活动 marker) 为准: airplayd 在有屏幕流连接时置之,
         * 手机断开 (掉 WiFi 常不发 FIN, 7000 悬在 ESTABLISHED 30~40s) 后由帧静默超时 /
         * RTSP 静默看门狗在数秒内清掉. 若只看 7000 连接数, 会卡在最后一帧不回二级页. */
        return file_exists("/run/airplay-active");
    if (!strcmp(svc, "carlife"))
        return conn_cnt(p_carlife, 7) > 0 || file_exists("/run/carlife-active");
    return 1;  /* 不认识的 service: 不要误判 */
}

/* ---------------------------------------------------------------- 跑模式脚本 */
/* mode: "ui" / "stop" / "auto" / "wait" / "video" (后两个带 arg=<svc>)
 * next_svc:   脚本成功后进入的 service ("" = idle)
 * next_state: 脚本成功后进入的状态 (ST_WAITING / ST_RUNNING / ST_IDLE) */
static int spawn_mode2(const char *mode, const char *arg, const char *next_svc,
                       state_t next_state)
{
    pid_t pid;

    if (g_child > 0) {
        vlog("[backend] 上一个动作还没结束, 忽略 spawn(%s)", mode);
        return -1;
    }

    vlog("[backend] spawn: %s %s %s", g_script, mode, arg ? arg : "");

    pid = fork();
    if (pid < 0) {
        set_msg("fork failed: %s", strerror(errno));
        g_st.state = ST_ERROR;
        return -1;
    }
    if (pid == 0) {
        /* 子进程: 交给 sh 跑 (脚本是 #!/bin/sh) */
        if (arg && *arg)
            execl("/bin/sh", "sh", g_script, mode, arg, (char *)NULL);
        else
            execl("/bin/sh", "sh", g_script, mode, (char *)NULL);
        _exit(127);
    }

    g_child = pid;
    snprintf(g_child_next, sizeof(g_child_next), "%s", next_svc);
    g_child_next_state = next_state;
    /* 记下目标模式 (脚本成功与否后面由 reap_child 兜) */
    if (!strcmp(mode, "wait") && arg && *arg)
        snprintf(g_mode, sizeof(g_mode), "wait:%s", arg);
    else if (!strcmp(mode, "video") && arg && *arg)
        snprintf(g_mode, sizeof(g_mode), "video:%s", arg);
    else if (!strcmp(mode, "auto"))
        snprintf(g_mode, sizeof(g_mode), "auto");
    else
        snprintf(g_mode, sizeof(g_mode), "ui");
    return 0;
}

/* 单参数便捷版 */
static int spawn_mode(const char *mode, state_t next_state)
{
    return spawn_mode2(mode, "", "", next_state);
}

/* 检查子进程是否结束, 结束就推进状态机 */
static void reap_child(void)
{
    int status;
    pid_t r;

    if (g_child <= 0) return;

    r = waitpid(g_child, &status, WNOHANG);
    if (r != g_child) return;

    {
        int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

        vlog("[backend] child %d done, ok=%d", (int)r, ok);

        if (!ok) {
            g_st.state = ST_ERROR;
            set_msg("mode switch failed (unit may be missing)");
            /* 失败也要把自己的状态归零, 否则 UI 会一直显示 starting */
            g_pending = 0;
        } else if (g_child_next[0] &&
                   (g_child_next_state == ST_WAITING || g_child_next_state == ST_RUNNING)) {
            g_st.state = g_child_next_state;
            snprintf(g_st.service, sizeof(g_st.service), "%s", g_child_next);
            snprintf(g_st.page, sizeof(g_st.page), "%s", g_child_next);
            set_msg(g_child_next_state == ST_WAITING
                        ? "%s waiting for device" : "%s running",
                    g_child_next);
            g_last_probe = now_s();
            g_probe_miss = 0;
        } else {
            g_st.state = ST_IDLE;
            snprintf(g_st.service, sizeof(g_st.service), "none");
            snprintf(g_st.page, sizeof(g_st.page), "none");
            set_msg("ready");
        }
    }

    g_child = 0;
    g_child_next[0] = '\0';

    /* 忙的时候攒下的请求, 现在执行 */
    if (g_pending) {
        char svc[24];
        snprintf(svc, sizeof(svc), "%s", g_pending_svc);
        if (g_pending == 1 && svc[0] && strcmp(svc, g_st.service)) {
            g_st.state = ST_STARTING;
            snprintf(g_st.service, sizeof(g_st.service), "%s", svc);
            snprintf(g_st.page, sizeof(g_st.page), "%s", svc);
            set_msg("starting %s", svc);
            spawn_mode2("wait", svc, svc, ST_WAITING);
        } else if (g_pending == 2 || g_pending == 3) {
            /* 注意: 不能要求 state != IDLE。
             * 2026-11-03 板上踩过: AUTO 跑完状态归 IDLE (屏交给了仲裁器), 此时排队的
             * STOP 被当成"已经在 idle 了"丢掉 ⇒ 用户按了回主界面, 屏上还是投屏/仲裁器画面。 */
            /* 3 = AUTO: 不能降级成 stop, 否则用户想交还仲裁器却停在主界面 */
            g_st.state = ST_STOPPING;
            snprintf(g_st.page, sizeof(g_st.page), "none");
            set_msg("stopping");
            spawn_mode(g_pending == 3 ? "auto" : "stop", ST_IDLE);
        }
        g_pending = 0;
        g_pending_svc[0] = '\0';
    }
}

/* ---------------------------------------------------------------- 命令处理 */
static void reply(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\n';
    (void)!write(fd, buf, (size_t)n);
}

static void cmd_status(int fd)
{
    reply(fd, "OK state=%s service=%s page=%s mode=%s msg=%s", state_str(g_st.state),
          g_st.service[0] ? g_st.service : "none",
          g_st.page[0] ? g_st.page : "none", g_mode,
          g_st.message[0] ? g_st.message : "-");
}

static void cmd_select(int fd, const char *svc)
{
    int busy = (g_child > 0);

    if (!is_known_service(svc)) {
        reply(fd, "ERR msg=unknown service %s", svc);
        return;
    }

    /* 同一路已在 waiting: 「重新连接」按钮走这里 —— 重跑一次 wait 模式把
     * 服务/fifo 读者/UI 都补回来 (脚本里有快路径, 不会动屏也不会重配蓝牙)。
     * running 态 k230ui 已被停, 用户点不到, 直接幂等回 OK。 */
    if (!busy && g_st.state == ST_RUNNING && !strcmp(g_st.service, svc)) {
        reply(fd, "OK state=%s service=%s page=%s msg=already running",
              state_str(g_st.state), g_st.service, g_st.page);
        return;
    }

    if (busy) {
        /* 正在切: 记住最后一次点击, 切完再执行 */
        snprintf(g_pending_svc, sizeof(g_pending_svc), "%s", svc);
        g_pending = 1;
        reply(fd, "OK state=%s service=%s page=%s msg=queued %s", state_str(g_st.state),
              g_st.service, g_st.page, svc);
        return;
    }

    /* SELECT = 只起该路服务, **不碰 DRM**: LVGL 二级画布继续占屏 (waiting) */
    g_st.state = ST_STARTING;
    snprintf(g_st.service, sizeof(g_st.service), "%s", svc);
    snprintf(g_st.page, sizeof(g_st.page), "%s", svc);
    set_msg("starting %s", svc);
    spawn_mode2("wait", svc, svc, ST_WAITING);
    reply(fd, "OK state=%s service=%s page=%s msg=starting %s", state_str(g_st.state),
          g_st.service, g_st.page, svc);
}

static void cmd_stop(int fd, int to_auto)
{
    if (g_child > 0) {
        g_pending = to_auto ? 3 : 2;
        reply(fd, "OK state=%s service=%s page=%s msg=queued %s", state_str(g_st.state),
              g_st.service, g_st.page, to_auto ? "auto" : "stop");
        return;
    }
    /* 只有"确实已经在主界面"才回 already idle; auto 模式下 g_st.state 也是 IDLE,
     * 但屏上是仲裁器的画面, 必须真的切回 ui。 */
    if (g_st.state == ST_IDLE && !strcmp(g_mode, "ui")) {
        reply(fd, "OK state=idle service=none page=none msg=already idle");
        return;
    }
    g_st.state = ST_STOPPING;
    snprintf(g_st.page, sizeof(g_st.page), "none");
    set_msg("stopping");
    spawn_mode(to_auto ? "auto" : "stop", ST_IDLE);
    reply(fd, "OK state=%s service=%s page=none msg=stopping", state_str(g_st.state), g_st.service);
}

static void handle_line(int fd, char *line)
{
    char *cmd, *arg;

    /* 去掉行尾 */
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
    if (!n) { reply(fd, "ERR msg=empty"); return; }

    cmd = line;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    arg = cmd;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) { *arg++ = '\0'; while (*arg == ' ' || *arg == '\t') arg++; }

    vlog("[backend] req: %s %s", cmd, arg);

    if (!strcasecmp(cmd, "STATUS")) {
        cmd_status(fd);
    } else if (!strcasecmp(cmd, "SELECT")) {
        if (!*arg) reply(fd, "ERR msg=SELECT needs a service name");
        else cmd_select(fd, arg);
    } else if (!strcasecmp(cmd, "STOP")) {
        cmd_stop(fd, 0);
    } else if (!strcasecmp(cmd, "AUTO")) {
        cmd_stop(fd, 1);
    } else if (!strcasecmp(cmd, "PING")) {
        reply(fd, "OK state=%s service=%s msg=pong", state_str(g_st.state), g_st.service);
    } else {
        reply(fd, "ERR msg=unknown command %s", cmd);
    }
}

/* 处理一个客户端: 读一行 -> 处理 -> 回复 -> 关闭 */
static void serve_client(int fd)
{
    char buf[256];
    size_t off = 0;
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;) {
        int rc = poll(&pfd, 1, CLIENT_READ_MS);
        ssize_t r;
        if (rc <= 0) break;
        r = recv(fd, buf + off, sizeof(buf) - 1 - off, 0);
        if (r <= 0) break;
        off += (size_t)r;
        buf[off] = '\0';
        if (strchr(buf, '\n') || off + 1 >= sizeof(buf)) break;
    }

    if (off)
        handle_line(fd, buf);
    else
        reply(fd, "ERR msg=no data");

    close(fd);
}

/* ---------------------------------------------------------------- socket */
static int open_socket(const char *path)
{
    struct sockaddr_un addr;
    char dir[128];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "[backend] mkdir %s: %s\n", dir, strerror(errno));
            return -1;
        }
    }

    unlink(path);

    g_listen = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen < 0) {
        fprintf(stderr, "[backend] socket: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[backend] bind %s: %s\n", path, strerror(errno));
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    if (listen(g_listen, 8) < 0) {
        fprintf(stderr, "[backend] listen: %s\n", strerror(errno));
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    chmod(path, 0666);   /* 允许非 root 客户端 (调试 CLI) 连 */

    return 0;
}

/* ---------------------------------------------------------------- 主循环 */
static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--socket PATH] [--mode-cmd PATH] [--no-init] [--verbose]\n"
            "  --socket PATH     Unix socket 路径 (默认 %s)\n"
            "  --mode-cmd PATH   模式切换脚本 (默认 %s)\n"
            "  --no-init         启动时不自动进入 ui 模式 (本机联调用)\n"
            "  --verbose         打印每条请求 / 子进程动作\n",
            argv0, SOCK_DEFAULT, SCRIPT_DEFAULT);
}

int main(int argc, char **argv)
{
    int i, no_init = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            snprintf(g_sock_path, sizeof(g_sock_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--mode-cmd") && i + 1 < argc) {
            snprintf(g_script, sizeof(g_script), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--no-init")) {
            no_init = 1;
        } else if (!strcmp(argv[i], "--verbose")) {
            g_verbose = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "[backend] unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);   /* 用 waitpid(WNOHANG) 自己收 */

    if (open_socket(g_sock_path) < 0)
        return 1;

    fprintf(stderr, "[backend] listening on %s (mode cmd: %s)\n", g_sock_path, g_script);

    /* 启动即接管: 让主界面占屏 (板子上电后应该看到 UI, 而不是紫屏/黑屏) */
    if (!no_init) {
        g_st.state = ST_STARTING;
        snprintf(g_st.service, sizeof(g_st.service), "none");
        snprintf(g_st.page, sizeof(g_st.page), "none");
        set_msg("initializing ui");
        spawn_mode("ui", ST_IDLE);
    } else {
        g_st.state = ST_IDLE;
        set_msg("ready (no-init)");
    }

    for (;;) {
        struct pollfd pfd;
        int rc;

        pfd.fd = g_listen;
        pfd.events = POLLIN;
        rc = poll(&pfd, 1, 200);

        if (rc > 0 && (pfd.revents & POLLIN)) {
            int cfd = accept(g_listen, NULL, NULL);
            if (cfd >= 0) {
                serve_client(cfd);
                continue;   /* 有请求时先把请求处理掉, 再收尾子进程 */
            }
        }

        reap_child();

        /* waiting <-> running 自动循环 (没有宽限期, 一直停在这条路上,
         * 除非用户点「返回主界面」发 STOP):
         *   waiting: 探到真实会话 -> 切 video (停 UI、起 drmplay 刷视频流)
         *   running: 连续 2 拍探不到会话 -> 切回 wait (停 drmplay、重启 UI 回二级页) */
        if (g_child <= 0 &&
            (g_st.state == ST_WAITING || g_st.state == ST_RUNNING)) {
            double t = now_s();
            if (t - g_last_probe >= PROBE_PERIOD_S) {
                g_last_probe = t;
                if (g_st.state == ST_WAITING) {
                    if (video_session_alive(g_st.service)) {
                        vlog("[backend] %s 会话建立, 切 video 让屏给 drmplay", g_st.service);
                        g_st.state = ST_STARTING;
                        set_msg("%s streaming, switching to video", g_st.service);
                        spawn_mode2("video", g_st.service, g_st.service, ST_RUNNING);
                    }
                } else {  /* ST_RUNNING */
                    if (video_session_alive(g_st.service)) {
                        g_probe_miss = 0;
                        set_msg("%s running", g_st.service);
                    } else if (++g_probe_miss >= PROBE_MISS_MAX) {
                        /* 端口刚建/刚拆的瞬间会抖, 连续 2 拍才算真断 */
                        vlog("[backend] %s 视频流断开, 切回二级页", g_st.service);
                        g_probe_miss = 0;
                        g_st.state = ST_STARTING;
                        set_msg("%s stream ended, back to page", g_st.service);
                        spawn_mode2("wait", g_st.service, g_st.service, ST_WAITING);
                    }
                }
            }
        }
    }

    return 0;
}
