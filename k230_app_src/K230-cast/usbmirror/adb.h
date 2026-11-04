#ifndef ADB_H
#define ADB_H
#include <stdint.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>

/* 最小 adb host 实现: 只做「找设备 -> CNXN/AUTH -> OPEN(service) -> 收发」。
 * 不做 adb server、不做 sync、不做多设备。 */

#define ADB_CMD_SYNC 0x434e5953u
#define ADB_CMD_CNXN 0x4e584e43u
#define ADB_CMD_OPEN 0x4e45504fu
#define ADB_CMD_OKAY 0x59414b4fu
#define ADB_CMD_CLSE 0x45534c43u
/* ⚠ 小端: 线上字节是 'W','R','T','E' => 0x45545257。写反成 0x57525457 时
 * 收到的包会全部"对不上", 表现为通道开着却永远 0 字节 (2026-11-03 真机踩过;
 * 自测用的 mock 也因为同样写错而假通过, 所以两边一起改)。 */
#define ADB_CMD_WRTE 0x45545257u
#define ADB_CMD_AUTH 0x48545541u

#define ADB_AUTH_TOKEN 1
#define ADB_AUTH_SIGNATURE 2
#define ADB_AUTH_RSAPUBLICKEY 3

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *h;
    int iface;
    uint8_t ep_in, ep_out;
    int maxdata;   /* 本端愿意接收的单包 payload 上限 */
    int vid, pid;
    char serial[64];
    uint8_t *rbuf; /* 复用接收缓冲 */
    int rbuf_cap;
    pthread_mutex_t tx; /* 多线程写同一 bulk OUT 必须串行 */
    int test_fd;
    int auth_sig_sent, auth_key_sent;   /* 鉴权进度: 握手与后续通道共享 */
} adb_dev;

/* 找一台运行 adb 的设备并完成握手。timeout_ms 是「等用户在手机上点允许」的耐心。
 * 返回 0 成功; <0 失败 (见 adb_strerror)。 */
int adb_connect(adb_dev *d, int timeout_ms, int verbose);

/* 自测入口: 用一个已连好的 socketpair fd 假冒 adbd, 跑同一套握手/通道逻辑。 */
int adb_connect_test(adb_dev *d, int fd, int timeout_ms, int verbose);
void adb_disconnect(adb_dev *d);

/* 打开一个 service 通道 (如 "exec:screenrecord ...", "shell:")。local_id 由调用方给。 */
int adb_open_channel(adb_dev *d, uint32_t local_id, const char *service, uint32_t *remote_id, int timeout_ms);

int adb_send_packet(adb_dev *d, uint32_t local_id, uint32_t remote_id, const void *data, int len);
int adb_send_okay(adb_dev *d, uint32_t local_id, uint32_t remote_id);
int adb_send_close(adb_dev *d, uint32_t local_id, uint32_t remote_id);

/* 收一个包。*data 需要调用方 adb_free_packet() 释放 (len 为 0 时可能是 NULL)。 */
int adb_recv_packet(adb_dev *d, int timeout_ms, uint32_t *cmd, uint32_t *a0, uint32_t *a1,
                    uint8_t **data, int *len);
void adb_free_packet(uint8_t *p);

const char *adb_strerror(int rc);
#endif

