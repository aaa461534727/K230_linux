/*
 * carlife.h —— K230 CarLife 车机端 (有线 USB AOA) 公共定义
 *
 * 协议依据: 见 ../docs/PROTOCOL.md (从 CarLife 车机库 wire 格式 + 社区实现整理)
 */
#ifndef CARLIFE_H
#define CARLIFE_H

#include <stdint.h>
#include <stddef.h>

/* ---- 通道 (外层 frame 的第一个 u32) ---- */
enum {
    CH_CMD   = 1,
    CH_VIDEO = 2,
    CH_MEDIA = 3,
    CH_TTS   = 4,
    CH_VR    = 5,
    CH_TOUCH = 6,
};

/* ---- 消息 ID (高 16 位 = 通道) ---- */
#define MSG_HU_VERSION      0x18001
#define MSG_VERSION_MATCH   0x10002
#define MSG_HU_INFO         0x18003
#define MSG_MD_INFO         0x10004
#define MSG_VIDEO_INIT      0x18007
#define MSG_VIDEO_INIT_DONE 0x10008
#define MSG_VIDEO_START     0x18009
#define MSG_VIDEO_PAUSE     0x1800A
#define MSG_VIDEO_RESET     0x1800B
/* 车机侧统计上报: 手机 App 收到后**停掉 120 s 授权计时器** (util/an.a()), 不发就弹
 * 「车机版本未经CarLife授权，即将断开连接！」。payload = CarlifeStatisticsInfo protobuf */
#define MSG_HU_STATISTICS   0x18027
#define MSG_VIDEO_DATA      0x20001
#define MSG_VIDEO_HEARTBEAT 0x20002
#define MSG_MEDIA_INIT      0x30001
#define MSG_MEDIA_STOP      0x30002
#define MSG_MEDIA_DATA      0x30006
#define MSG_TOUCH_ACTION    0x68001

#define MAX_INNER (4u * 1024 * 1024)

typedef struct {
    uint16_t channel;
    uint32_t message;
    uint32_t timestamp;
    const uint8_t *payload;
    uint32_t payload_len;
} cl_packet;

/* ---- 协议编解码 ---- */
size_t cl_varint(uint8_t *out, uint64_t v);
size_t cl_proto_int(uint8_t *out, int field, uint64_t v);
size_t cl_proto_bytes(uint8_t *out, int field, const void *data, size_t len);
size_t cl_proto_str(uint8_t *out, int field, const char *s);
/* 从 payload 里取第 field 个字段 (wire 0 = int; wire 2 时返回长度, 数据在 *data) */
int cl_proto_get(const uint8_t *p, size_t n, int field, uint64_t *val,
                 const uint8_t **data, size_t *data_len);
/* 组一个完整外层帧, 返回总长; buf 至少 12 + payload 字节 */
size_t cl_frame(uint8_t *buf, size_t cap, uint16_t channel, uint32_t message,
                uint32_t timestamp, const uint8_t *payload, uint32_t payload_len);

/* 增量解析器 */
typedef struct {
    uint8_t  buf[64 * 1024];
    size_t   len;
    /* payload 必须单独存一份: 喂进一个包里往往粘着后续包, 解析出一个包后要把已消费
     * 的字节从 buf 前面挪掉 —— 那样会把 payload 覆盖掉 (无线 TCP 粘包必踩, 表现为
     * 手机回"版本不匹配"或 protobuf 解出垃圾)。payload 指向这里, 下次 feed 前有效。 */
    uint8_t  out[64 * 1024];
} cl_parser;
void cl_parser_reset(cl_parser *p);
/* 返回 >0 = 解析出一个包 (payload 指针指向 p->buf, 下次 feed 前有效);
 * 0 = 数据不够; -1 = 协议错误 */
int cl_parser_feed(cl_parser *p, const uint8_t *data, size_t n, cl_packet *out);
/* 无线 (每通道一条 TCP): 流里没有外层 8 字节头, 只有 8 字节内层头
 *   u16 len | u16 reserved | u32 msg —— 长度上限 32768 (手机侧 c.d() 这么校验) */
int cl_inner_parser_feed(cl_parser *p, const uint8_t *data, size_t n, cl_packet *out);

/* ---- USB (libusb) ---- */
typedef struct cl_usb cl_usb;

/* 找到并切到 accessory 模式; 返回 0 = 已下发 AOA53 (随后等重新枚举) */
int cl_usb_open_and_switch(int log_fd);
cl_usb *cl_usb_wait_accessory(int timeout_ms, int log_fd);
int  cl_usb_read(cl_usb *u, uint8_t *buf, size_t cap, int timeout_ms);
int  cl_usb_write(cl_usb *u, const uint8_t *buf, size_t n);
int  cl_usb_send(cl_usb *u, const uint8_t *frame, size_t n);
const char *cl_usb_last_error(void);
void cl_usb_close(cl_usb *u);

#endif
