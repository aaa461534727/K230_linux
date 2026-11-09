/**
 * k230-ui-backend.c —— LVGL 主界面的后端守护进程
 *
 * 它解决什么问题:
 *   主界面 (k230-ui) 和投屏显示 (k230-screen/drmplay) 抢同一个 DRM master,
 *   不能同时跑; 而"谁该上屏"又要看用户点了哪个按钮、会话什么时候结束。
 *   本守护进程 = 唯一的决策者:
 *     · 对外只开一个 Unix socket (文本行协议), UI 只跟它说话, 不直接碰 systemd;
 *     · 收到 SELECT/STOP 就调 k230-ui-mode.sh 做"停对方 -> 起自己"的屏幕让位,
 *       脚本是子进程异步跑, 期间照样能应答 STATUS (UI 不会卡);
 *     · 运行中每 2 s 探一次会话 (看 /proc/net/tcp 里对应端口的 ESTABLISHED 连接),
 *       手机断开就自动收尾并切回主界面。
 *
 * 协议 (一行一条, 见 frontend/src/backend_client.h):
 *   请求  STATUS | SELECT <svc> | STOP | AUTO
 *   响应  OK state=<idle|starting|running|stopping|error|unknown> service=<svc|none> msg=<文本>
 *         ERR msg=<文本>
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

#define PROBE_PERIOD_S 2.0   /* 运行中多久探一次会话 */
#define CLIENT_READ_MS 500   /* 读客户端一行最多等多久 */
/* 刚切进某一路后的"宽限期": 这段时间内探不到会话**不算断开**。
 * 为什么必须有: 用户点了 Miracast 之后, 手机上找设备/连 P2P 要十几秒,
 * CarPlay/CarLife 还要手机先连热点再建会话, 动辄 30~60 s。
 * 没有宽限期的话, 按钮点下去 4 s 就自动弹回主界面 (= 按钮看起来没反应)。 */
#define GRACE_DEFAULT_S 60.0

/* ---------------------------------------------------------------- 状态机 */
typedef enum {
    ST_UNKNOWN = 0,
    ST_IDLE,
    ST_STARTING,
    ST_RUNNING,
    ST_STOPPING,
    ST_ERROR
} state_t;

static const char *state_str(state_t s)
{
    switch (s) {
    case ST_IDLE:     return "idle";
    case ST_STARTING: return "starting";
    case ST_RUNNING:  return "running";
    case ST_STOPPING: return "stopping";
    case ST_ERROR:    return "error";
    default:          return "unknown";
    }
}

static struct {
    state_t state;
    char    service[24];   /* miracast / airplay / carlife / carplay / none */
    char    message[128];
} g_st = { ST_UNKNOWN, "none", "booting" };

/* 子进程 (模式脚本) 跟踪 */
static pid_t g_child = 0;
static char  g_child_next[24];   /* 脚本跑完后要进入的 service 名 ("" = 回 idle) */
static char  g_pending_svc[24];  /* 忙的时候收到的下一个请求 (最后一条胜出) */
/* 板上真正处在哪个模式: ui / miracast / airplay / carlife / carplay / auto
 * 为什么不能只靠 g_st.state: AUTO 跑完后 g_st.state 也是 IDLE (界面不在屏上, 屏交给仲裁器),
 * 但此时屏上**不是主界面**。2026-11-03 板上踩过: 从 auto 按 STOP 被当成 "already idle"
 * 丢弃, 用户按了回主界面, 屏上还是仲裁器的画面。 */
static char  g_mode[24] = "ui";
static int   g_pending = 0;      /* 0 无 / 1 = select / 2 = stop / 3 = auto */

/* 配置 */
static char  g_sock_path[108] = SOCK_DEFAULT;
static char  g_script[256] = SCRIPT_DEFAULT;
static int   g_verbose = 0;
static int   g_listen = -1;
static double g_last_probe = 0;
static int    g_probe_miss = 0;   /* 连续几次探不到会话 (2 次才算真断) */
static double g_run_start = 0;    /* 进入 RUNNING 的时刻 (宽限期起点) */
static double g_grace_s = GRACE_DEFAULT_S;

/* 每个模式自己的宽限期。
 * ★ 2026-11-05 实测踩坑: 点 AirPlay 后 **65 s** 就被判"会话结束"弹回主界面,
 *   而 AirPlay 的真实流程是「板子起热点/广播 → 手机连 Wi-Fi → 手机连**蓝牙**反控
 *   → 手机上点屏幕镜像」, 用户手上还要在手机里点好几下, 60 s 根本不够;
 *   更糟的是弹回主界面会顺手把蓝牙关掉 ⇒ 手机上 K230-CAST 直接消失, 表现为
 *   "点了 AirPlay 手机连不上蓝牙"。
 * ★★ 2026-11-05 第二次踩坑 (主人实测"安卓苹果都连不上"): 300 s 依然太短 ——
 *   板端时间线 16:33:53 mode=airplay → 16:38:58 mode=ui, 正好 5 分钟到点自动
 *   回主界面并 **关掉蓝牙** (Powered: no), btmon 该时段零 HCI 事件;
 *   用户从板子走到手机、翻设置、点 K230-CAST 时板子蓝牙早没了 ⇒ iOS 必弹
 *   "配对不成功"。AirPlay/CarPlay 的宽限期改为 **30 分钟** (1800 s), 并允许
 *   用环境变量覆盖: K230UI_GRACE_AIRPLAY / K230UI_GRACE_CARPLAY (秒)。 */
static double g_grace_airplay = 1800.0;
static double g_grace_carplay = 1800.0;

static double grace_for(const char *svc)
{
    if (!strncmp(svc, "airplay", 7))
        return g_grace_airplay;
    if (!strncmp(svc, "carplay", 7))
        return g_grace_carplay;
    return g_grace_s;
}

/* 环境变量读一个"秒"值, 非法/<=0 就保留默认值 */
static void grace_from_env(void)
{
    const struct { const char *name; double *dst; } envs[] = {
        { "K230UI_GRACE_AIRPLAY", &g_grace_airplay },
        { "K230UI_GRACE_CARPLAY", &g_grace_carplay },
        { "K230UI_GRACE_DEFAULT", &g_grace_s },
    };
    size_t i;
    for (i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
        const char *v = getenv(envs[i].name);
        char *end = NULL;
        double d;
        if (!v || !*v) continue;
        d = strtod(v, &end);
        if (end && *end == '\0' && d > 0.0 && d < 86400.0) *envs[i].dst = d;
    }
}

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

/* 返回值: 1 = 会话还在, 0 = 会话已结束 */
/* 蓝牙反控通道有没有连上 (读 k230-bt-hid 写的状态文件: `对端=94:BF:..` / `对端=-`)。
 * 为什么 AirPlay 要把它算进"会话活着": 用户点 AirPlay 之后的第一步就是连蓝牙,
 * 这时 AirPlay 的 TCP 端口 (7100/7101) 还没建 ⇒ 只看端口会把正在配对的用户踢回主界面
 * (连蓝牙一起关掉, 手机上的设备就消失了)。 */
static int bt_hid_connected(void)
{
    char buf[512];
    FILE *f = fopen("/run/k230-bt-hid.status", "r");
    const char *k;
    const char *v;
    size_t n;

    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    k = strstr(buf, "对端=");
    if (!k)
        return 0;
    v = k + strlen("对端=");
    while (*v == ' ' || *v == '\t')
        v++;
    return *v != '-' && *v != '\n' && *v != '\0';
}

static int session_alive(const char *svc)
{
    static const int p_miracast[] = { 7236 };
    static const int p_airplay[]  = { 7100, 7101 };
    static const int p_carplay[]  = { 7000 };
    static const int p_carlife[]  = { 7240, 8240, 9240, 9241, 9242, 9340, 9440 };

    if (!strcmp(svc, "miracast"))
        return conn_cnt(p_miracast, 1) > 0;
    if (!strcmp(svc, "airplay"))
        return conn_cnt(p_airplay, 2) > 0 || bt_hid_connected();
    if (!strcmp(svc, "carplay"))
        return file_exists("/run/airplay-active") || conn_cnt(p_carplay, 1) > 0;
    if (!strcmp(svc, "carlife"))
        return conn_cnt(p_carlife, 7) > 0;
    return 1;  /* 不认识的 service: 不要误杀 */
}

/* ---------------------------------------------------------------- 跑模式脚本 */
/* mode_arg: "ui" / "stop" / "auto" / "select <svc>"
 * next_svc: 脚本成功后进入的 service ("" = idle) */
static int spawn_mode2(const char *mode, const char *arg, const char *next_svc)
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
    /* 记下目标模式 (脚本成功与否后面由 reap_child 兜) */
    if (!strcmp(mode, "select") && arg && *arg)
        snprintf(g_mode, sizeof(g_mode), "%s", arg);
    else if (!strcmp(mode, "auto"))
        snprintf(g_mode, sizeof(g_mode), "auto");
    else
        snprintf(g_mode, sizeof(g_mode), "ui");
    return 0;
}

/* 单参数便捷版 */
static int spawn_mode(const char *mode, const char *next_svc)
{
    return spawn_mode2(mode, "", next_svc);
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
        } else if (g_child_next[0]) {
            g_st.state = ST_RUNNING;
            snprintf(g_st.service, sizeof(g_st.service), "%s", g_child_next);
            set_msg("%s running", g_child_next);
            g_last_probe = now_s();
            g_run_start = now_s();
            g_probe_miss = 0;
        } else {
            g_st.state = ST_IDLE;
            snprintf(g_st.service, sizeof(g_st.service), "none");
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
            set_msg("starting %s", svc);
            spawn_mode2("select", svc, svc);
        } else if (g_pending == 2 || g_pending == 3) {
            /* 注意: 不能要求 state != IDLE。
             * 2026-11-03 板上踩过: AUTO 跑完状态归 IDLE (屏交给了仲裁器), 此时排队的
             * STOP 被当成"已经在 idle 了"丢掉 ⇒ 用户按了回主界面, 屏上还是投屏/仲裁器画面。 */
            /* 3 = AUTO: 不能降级成 stop, 否则用户想交还仲裁器却停在主界面 */
            g_st.state = ST_STOPPING;
            set_msg("stopping");
            spawn_mode(g_pending == 3 ? "auto" : "stop", "");
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
    reply(fd, "OK state=%s service=%s mode=%s msg=%s", state_str(g_st.state),
          g_st.service[0] ? g_st.service : "none", g_mode,
          g_st.message[0] ? g_st.message : "-");
}

static void cmd_select(int fd, const char *svc)
{
    int busy = (g_child > 0);

    if (!is_known_service(svc)) {
        reply(fd, "ERR msg=unknown service %s", svc);
        return;
    }

    /* 点的就是当前跑着的那一路 -> 当作"停" */
    if (!busy && g_st.state == ST_RUNNING && !strcmp(g_st.service, svc)) {
        g_st.state = ST_STOPPING;
        set_msg("stopping %s", svc);
        spawn_mode("stop", "");
        reply(fd, "OK state=%s service=%s msg=stopping %s", state_str(g_st.state),
              g_st.service, svc);
        return;
    }

    if (busy) {
        /* 正在切: 记住最后一次点击, 切完再执行 */
        snprintf(g_pending_svc, sizeof(g_pending_svc), "%s", svc);
        g_pending = 1;
        reply(fd, "OK state=%s service=%s msg=queued %s", state_str(g_st.state),
              g_st.service, svc);
        return;
    }

    g_st.state = ST_STARTING;
    snprintf(g_st.service, sizeof(g_st.service), "%s", svc);
    set_msg("starting %s", svc);
    spawn_mode2("select", svc, svc);
    reply(fd, "OK state=%s service=%s msg=starting %s", state_str(g_st.state),
          g_st.service, svc);
}

static void cmd_stop(int fd, int to_auto)
{
    if (g_child > 0) {
        g_pending = to_auto ? 3 : 2;
        reply(fd, "OK state=%s service=%s msg=queued %s", state_str(g_st.state),
              g_st.service, to_auto ? "auto" : "stop");
        return;
    }
    /* 只有"确实已经在主界面"才回 already idle; auto 模式下 g_st.state 也是 IDLE,
     * 但屏上是仲裁器的画面, 必须真的切回 ui。 */
    if (g_st.state == ST_IDLE && !strcmp(g_mode, "ui")) {
        reply(fd, "OK state=idle service=none msg=already idle");
        return;
    }
    g_st.state = ST_STOPPING;
    set_msg("stopping");
    spawn_mode(to_auto ? "auto" : "stop", "");
    reply(fd, "OK state=%s service=%s msg=stopping", state_str(g_st.state), g_st.service);
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

    grace_from_env();

    if (open_socket(g_sock_path) < 0)
        return 1;

    fprintf(stderr, "[backend] listening on %s (mode cmd: %s)\n", g_sock_path, g_script);

    /* 启动即接管: 让主界面占屏 (板子上电后应该看到 UI, 而不是紫屏/黑屏) */
    if (!no_init) {
        g_st.state = ST_STARTING;
        snprintf(g_st.service, sizeof(g_st.service), "none");
        set_msg("initializing ui");
        spawn_mode("ui", "");
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

        /* 运行中: 定期探会话, 手机断开就自动回主界面 */
        if (g_st.state == ST_RUNNING && g_child <= 0) {
            double t = now_s();
            if (t - g_last_probe >= PROBE_PERIOD_S) {
                g_last_probe = t;
                if (session_alive(g_st.service)) {
                    g_probe_miss = 0;
                    if (strstr(g_st.message, "running") == NULL)
                        set_msg("%s running", g_st.service);
                } else if (t - g_run_start < grace_for(g_st.service)) {
                    /* 宽限期内: 用户还在手机上找设备, 不要弹回主界面 */
                    g_probe_miss = 0;
                    set_msg("%s waiting for device", g_st.service);
                } else if (++g_probe_miss >= 2) {
                    /* 连续 2 拍探不到才算真断 —— 端口刚建/刚拆的瞬间会抖 */
                    vlog("[backend] %s 会话结束, 回主界面", g_st.service);
                    g_probe_miss = 0;
                    g_st.state = ST_STOPPING;
                    set_msg("%s ended", g_st.service);
                    spawn_mode("stop", "");
                }
            }
        }
    }

    return 0;
}
