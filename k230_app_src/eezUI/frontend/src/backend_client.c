/**
 * backend_client.c — 见 backend_client.h 的设计说明
 *
 * 线程模型:
 *   main 线程 (LVGL)         后台线程 (be_worker)
 *   ----------------------   ---------------------------------
 *   be_select("miracast") ->  队列 <- 取命令
 *     只加锁入队, 立即返回        连 socket / 发 / 收一行
 *   be_get_status(&st) <-   状态结构 <- 加锁写入
 *
 * 断线处理: 后端没起来 (socket 不存在 / 连不上) 时, 状态置 UNKNOWN、
 * connected=0, 命令丢弃 —— UI 就显示 "backend offline", 不会崩。
 */
#include "backend_client.h"

#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#define DEFAULT_SOCK   "/run/k230-ui/backend.sock"
#define CMD_MAX        32
#define QUEUE_LEN      8
#define POLL_PERIOD_S  0.7   /* 空闲时多久刷新一次 STATUS */
#define CONNECT_MS     400
#define REPLY_MS       1500

/* ---------------- 共享状态 ---------------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;

static char   g_sock[108];
static char   g_queue[QUEUE_LEN][CMD_MAX];
static int    g_qhead, g_qtail, g_qcount;

static be_status_t g_status;
static int    g_running;
static pthread_t g_thread;

/* ---------------- 小工具 ---------------- */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (cap == 0) return;
    if (!src) src = "";
    n = strnlen(src, cap - 1);   /* 明确截断, 不依赖 snprintf 的截断行为 */
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* "state=running" -> 1 并把值写进 out */
static int kv_value(const char *line, const char *key, char *out, size_t cap)
{
    char pat[32];
    const char *p;
    size_t klen;

    snprintf(pat, sizeof(pat), "%s=", key);
    klen = strlen(pat);
    p = line;
    while ((p = strstr(p, pat)) != NULL) {
        /* 必须位于行首或空白之后, 避免误匹配 xxx_state= */
        if (p == line || p[-1] == ' ' || p[-1] == '\t') {
            const char *v = p + klen;
            size_t n = 0;
            while (v[n] && v[n] != ' ' && v[n] != '\r' && v[n] != '\n' && n + 1 < cap) {
                out[n] = v[n];
                n++;
            }
            out[n] = '\0';
            return 1;
        }
        p += klen;
    }
    return 0;
}

static be_state_t parse_state(const char *s)
{
    if (!strcmp(s, "idle"))     return BE_STATE_IDLE;
    if (!strcmp(s, "starting")) return BE_STATE_STARTING;
    if (!strcmp(s, "waiting"))  return BE_STATE_WAITING;
    if (!strcmp(s, "running"))  return BE_STATE_RUNNING;
    if (!strcmp(s, "stopping")) return BE_STATE_STOPPING;
    if (!strcmp(s, "error"))    return BE_STATE_ERROR;
    return BE_STATE_UNKNOWN;
}

/* 解析一行响应写进 g_status (调用者不加锁, 由 worker 直接写) */
static void apply_reply(const char *line)
{
    char buf[64];

    g_status.connected = 1;

    if (!strncmp(line, "ERR", 3)) {
        const char *m = strstr(line, "msg=");
        g_status.state = BE_STATE_ERROR;
        copy_str(g_status.message, sizeof(g_status.message), m ? m + 4 : "backend error");
        return;
    }

    if (kv_value(line, "state", buf, sizeof(buf)))
        g_status.state = parse_state(buf);

    if (kv_value(line, "service", buf, sizeof(buf)))
        copy_str(g_status.service, sizeof(g_status.service), buf);
    else
        copy_str(g_status.service, sizeof(g_status.service), "none");

    if (kv_value(line, "page", buf, sizeof(buf)))
        copy_str(g_status.page, sizeof(g_status.page), buf);
    else
        copy_str(g_status.page, sizeof(g_status.page), "none");

    {
        const char *m = strstr(line, "msg=");
        if (m) {
            copy_str(g_status.message, sizeof(g_status.message), m + 4);
            /* 去掉行尾换行 */
            size_t n = strlen(g_status.message);
            while (n && (g_status.message[n - 1] == '\n' || g_status.message[n - 1] == '\r'))
                g_status.message[--n] = '\0';
        }
    }
}

/* ---------------- socket 一次往返 ---------------- */
/* 返回 0 = 成功拿到一行 (写进 reply), -1 = 失败 (errmsg 说明原因) */
static int do_request(const char *cmd, char *reply, size_t reply_cap, char *errmsg, size_t errcap)
{
    int fd;
    struct sockaddr_un addr;
    struct pollfd pfd;
    size_t len, off;
    int rc;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        copy_str(errmsg, errcap, "socket() failed");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    copy_str(addr.sun_path, sizeof(addr.sun_path), g_sock);

    /* 非阻塞连接 + poll, 避免后端卡住时 UI 线程被拖死 (虽然这里是 worker) */
    pfd.fd = fd;
    pfd.events = POLLOUT;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            copy_str(errmsg, errcap, "connect failed (backend not running?)");
            close(fd);
            return -1;
        }
        rc = poll(&pfd, 1, CONNECT_MS);
        if (rc <= 0) {
            copy_str(errmsg, errcap, "connect timeout");
            close(fd);
            return -1;
        }
        {
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) {
                copy_str(errmsg, errcap, "connect refused (backend not running?)");
                close(fd);
                return -1;
            }
        }
    }

    len = strlen(cmd);
    off = 0;
    while (off < len) {
        ssize_t w = send(fd, cmd + off, len - off, MSG_NOSIGNAL);
        if (w <= 0) {
            copy_str(errmsg, errcap, "send failed");
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    /* 收一行 (遇到 \n 或缓冲区满即返回) */
    off = 0;
    reply[0] = '\0';
    pfd.events = POLLIN;
    for (;;) {
        rc = poll(&pfd, 1, REPLY_MS);
        if (rc <= 0) {
            copy_str(errmsg, errcap, "reply timeout");
            close(fd);
            return -1;
        }
        {
            ssize_t r = recv(fd, reply + off, reply_cap - 1 - off, 0);
            if (r <= 0) {
                copy_str(errmsg, errcap, "connection closed");
                close(fd);
                return -1;
            }
            off += (size_t)r;
            reply[off] = '\0';
        }
        if (strchr(reply, '\n') || off + 1 >= reply_cap)
            break;
    }

    close(fd);
    return 0;
}

/* ---------------- 命令队列 ---------------- */
static void queue_push(const char *cmd)
{
    pthread_mutex_lock(&g_lock);
    if (g_qcount < QUEUE_LEN) {
        copy_str(g_queue[g_qtail], CMD_MAX, cmd);
        g_qtail = (g_qtail + 1) % QUEUE_LEN;
        g_qcount++;
        pthread_cond_signal(&g_cond);
    } else {
        /* 队列满 = 用户连点得比后端回得快, 丢掉最老的一条 */
        g_qhead = (g_qhead + 1) % QUEUE_LEN;
        g_qcount--;
        copy_str(g_queue[g_qtail], CMD_MAX, cmd);
        g_qtail = (g_qtail + 1) % QUEUE_LEN;
        g_qcount++;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

static int queue_pop(char *out, int wait_ms)
{
    int got = 0;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += wait_ms / 1000;
    ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_lock);
    if (g_qcount == 0)
        pthread_cond_timedwait(&g_cond, &g_lock, &ts);
    if (g_qcount > 0) {
        copy_str(out, CMD_MAX, g_queue[g_qhead]);
        g_qhead = (g_qhead + 1) % QUEUE_LEN;
        g_qcount--;
        got = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

/* ---------------- 后台线程 ---------------- */
static void publish(const be_status_t *st)
{
    pthread_mutex_lock(&g_lock);
    g_status = *st;
    pthread_mutex_unlock(&g_lock);
}

static void *be_worker(void *arg)
{
    char cmd[CMD_MAX];
    char reply[512];
    char errmsg[128];
    double last_poll = 0.0;

    (void)arg;

    while (1) {
        be_status_t st;

        pthread_mutex_lock(&g_lock);
        if (!g_running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        st = g_status;
        pthread_mutex_unlock(&g_lock);

        if (!queue_pop(cmd, 100)) {
            /* 没命令就按周期刷 STATUS */
            double t = now_s();
            if (t - last_poll < POLL_PERIOD_S)
                continue;
            last_poll = t;
            copy_str(cmd, CMD_MAX, "STATUS\n");
        }

        if (do_request(cmd, reply, sizeof(reply), errmsg, sizeof(errmsg)) == 0) {
            pthread_mutex_lock(&g_lock);
            apply_reply(reply);
            pthread_mutex_unlock(&g_lock);
        } else {
            st.connected = 0;
            st.state = BE_STATE_UNKNOWN;
            copy_str(st.service, sizeof(st.service), "none");
            copy_str(st.message, sizeof(st.message), errmsg);
            publish(&st);
            sleep_ms(300); /* 后端没起来时别把 CPU 打满 */
        }
    }
    return NULL;
}

/* ---------------- 对外接口 ---------------- */
void be_start(const char *sock_path)
{
    const char *env;

    if (g_running)
        return;

    if (sock_path && *sock_path) {
        copy_str(g_sock, sizeof(g_sock), sock_path);
    } else if ((env = getenv("K230UI_SOCK")) && *env) {
        copy_str(g_sock, sizeof(g_sock), env);
    } else {
        copy_str(g_sock, sizeof(g_sock), DEFAULT_SOCK);
    }

    memset(&g_status, 0, sizeof(g_status));
    g_status.state = BE_STATE_UNKNOWN;
    copy_str(g_status.service, sizeof(g_status.service), "none");
    copy_str(g_status.page, sizeof(g_status.page), "none");
    copy_str(g_status.message, sizeof(g_status.message), "connecting...");
    g_qhead = g_qtail = g_qcount = 0;
    g_running = 1;

    if (pthread_create(&g_thread, NULL, be_worker, NULL) != 0) {
        g_running = 0;
        fprintf(stderr, "[backend_client] pthread_create failed\n");
    }
}

void be_shutdown(void)
{
    if (!g_running)
        return;
    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
}

void be_select(const char *service)
{
    char cmd[CMD_MAX];
    if (!service || !*service) return;
    snprintf(cmd, sizeof(cmd), "SELECT %s\n", service);
    queue_push(cmd);
}

void be_stop_session(void)
{
    queue_push("STOP\n");
}

void be_auto(void)
{
    queue_push("AUTO\n");
}

void be_get_status(be_status_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_status;
    pthread_mutex_unlock(&g_lock);
}

const char *be_state_name(be_state_t s)
{
    switch (s) {
    case BE_STATE_IDLE:     return "idle";
    case BE_STATE_STARTING: return "starting";
    case BE_STATE_WAITING:  return "waiting";
    case BE_STATE_RUNNING:  return "running";
    case BE_STATE_STOPPING: return "stopping";
    case BE_STATE_ERROR:    return "error";
    default:                return "unknown";
    }
}
