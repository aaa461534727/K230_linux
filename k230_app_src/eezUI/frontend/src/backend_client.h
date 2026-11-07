/**
 * backend_client.h — UI 与后端守护进程 (k230-ui-backend) 之间唯一的通信层
 *
 * 设计原则 (为什么这么绕):
 *   LVGL 主循环是单线程的 (LV_USE_OS = LV_OS_NONE), 在 main 线程里阻塞等 socket
 *   = 界面卡死 / 触摸迟滞。所以这里起一个后台线程专门跑 socket:
 *     - UI 线程只往队列里丢命令 (select/stop/auto), 立刻返回;
 *     - 后台线程发请求、收状态, 写进一把互斥锁保护的状态结构;
 *     - UI 侧用 lv_timer 每 500ms 取一次快照刷标签。
 *
 * 协议: Unix domain socket, 纯文本一行一条 (UTF-8 无关, 纯 ASCII)
 *   请求   STATUS | SELECT <service> | STOP | AUTO
 *   响应   OK state=<idle|starting|waiting|running|stopping|error> service=<name|none> page=<name|none> msg=<任意文本>
 *          ERR msg=<任意文本>
 *   service 取值: miracast | airplay | carlife | carplay
 *   page    = 后端认为当前该显示的二级画布 (waiting/running 时 = service; idle 时 = none)
 *
 * 状态语义:
 *   idle      主界面 (LVGL 占屏, 无投屏服务)
 *   starting  正在跑模式切换脚本
 *   waiting   待投屏: 该路服务已起, 但**屏仍归 LVGL 二级画布** (不刷黑)
 *   running   投屏中: 屏归 drmplay 视频流 (此时 k230ui 被停, 不显示)
 *   stopping  正在收尾回主界面
 */
#ifndef K230UI_BACKEND_CLIENT_H
#define K230UI_BACKEND_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BE_STATE_UNKNOWN = 0,
    BE_STATE_IDLE,
    BE_STATE_STARTING,
    BE_STATE_WAITING,
    BE_STATE_RUNNING,
    BE_STATE_STOPPING,
    BE_STATE_ERROR
} be_state_t;

typedef struct {
    be_state_t state;
    char service[24]; /* miracast / airplay / carlife / carplay / none */
    char page[24];    /* 当前该显示的二级画布 / none */
    char message[128];
    int  connected;   /* 1 = 最近一次请求拿到了响应 */
} be_status_t;

/* 启动后台线程; sock_path 为 NULL 时用 $K230UI_SOCK 或默认 /run/k230-ui/backend.sock */
void be_start(const char *sock_path);

/* 停后台线程 (板端退出前调用; 模拟器里可不调) */
void be_shutdown(void);

/* 下面三个都是"丢进队列立刻返回", 不阻塞 UI */
void be_select(const char *service); /* SELECT <service>: 启动某一路投屏 */
void be_stop_session(void);          /* STOP: 停掉当前会话, 回到空闲 */
void be_auto(void);                  /* AUTO: 交还给开机待命仲裁器 */

/* 取状态快照 (线程安全) */
void be_get_status(be_status_t *out);

/* 状态枚举 -> 给 UI 显示用的短名 */
const char *be_state_name(be_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* K230UI_BACKEND_CLIENT_H */
