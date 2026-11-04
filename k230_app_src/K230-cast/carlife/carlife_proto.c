/*
 * carlife_proto.c —— CarLife 内外层帧 + 极简 protobuf 编解码
 *
 * 外层:  u32 channel | u32 inner_len
 * 内层:  CMD/TOUCH: u16 len | u16 reserved(0) | u32 msg | payload
 *        其它通道:  u32 len | u32 timestamp     | u32 msg | payload
 * 大端。
 */
#include "carlife.h"
#include <string.h>

size_t cl_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    while (v > 127) { out[n++] = (uint8_t)((v & 127) | 128); v >>= 7; }
    out[n++] = (uint8_t)v;
    return n;
}

size_t cl_proto_int(uint8_t *out, int field, uint64_t v)
{
    size_t n = cl_varint(out, ((uint64_t)field << 3) | 0);
    return n + cl_varint(out + n, v);
}

size_t cl_proto_bytes(uint8_t *out, int field, const void *data, size_t len)
{
    size_t n = cl_varint(out, ((uint64_t)field << 3) | 2);
    n += cl_varint(out + n, len);
    if (len) memcpy(out + n, data, len);
    return n + len;
}

size_t cl_proto_str(uint8_t *out, int field, const char *s)
{
    return cl_proto_bytes(out, field, s, strlen(s));
}

static int get_varint(const uint8_t *p, size_t n, size_t *pos, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*pos < n && shift < 70) {
        uint8_t b = p[(*pos)++];
        v |= (uint64_t)(b & 127) << shift;
        if (!(b & 128)) { *out = v; return 0; }
        shift += 7;
    }
    return -1;
}

int cl_proto_get(const uint8_t *p, size_t n, int field, uint64_t *val,
                 const uint8_t **data, size_t *data_len)
{
    size_t pos = 0;
    while (pos < n) {
        uint64_t key;
        if (get_varint(p, n, &pos, &key) != 0) return -1;
        int f = (int)(key >> 3), wire = (int)(key & 7);
        if (f == 0) return -1;
        if (wire == 0) {
            uint64_t v;
            if (get_varint(p, n, &pos, &v) != 0) return -1;
            if (f == field) { *val = v; return 0; }
        } else if (wire == 2) {
            uint64_t len;
            if (get_varint(p, n, &pos, &len) != 0) return -1;
            if (len > n - pos) return -1;
            if (f == field) {
                if (data) *data = p + pos;
                if (data_len) *data_len = (size_t)len;
                return 0;
            }
            pos += (size_t)len;
        } else if (wire == 1) { pos += 8; }
        else if (wire == 5) { pos += 4; }
        else return -1;
    }
    return -1;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

size_t cl_frame(uint8_t *buf, size_t cap, uint16_t channel, uint32_t message,
                uint32_t timestamp, const uint8_t *payload, uint32_t payload_len)
{
    int hdr = (channel == CH_CMD || channel == CH_TOUCH) ? 8 : 12;
    size_t inner = (size_t)hdr + payload_len;
    if (inner > MAX_INNER || cap < 8 + inner) return 0;

    put32(buf, channel);
    put32(buf + 4, (uint32_t)inner);
    if (hdr == 8) {
        put16(buf + 8, (uint16_t)payload_len);
        put16(buf + 10, 0);
        put32(buf + 12, message);
    } else {
        put32(buf + 8, payload_len);
        put32(buf + 12, timestamp);
        put32(buf + 16, message);
    }
    if (payload_len) memcpy(buf + 8 + hdr, payload, payload_len);
    return 8 + inner;
}

void cl_parser_reset(cl_parser *p) { p->len = 0; }

int cl_parser_feed(cl_parser *p, const uint8_t *data, size_t n, cl_packet *out)
{
    if (n > sizeof(p->buf) - p->len) {          /* 一次喂太多: 丢旧的保当前 */
        p->len = 0;
        if (n > sizeof(p->buf)) return -1;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;

    if (p->len < 8) return 0;
    uint16_t channel = (uint16_t)get32(p->buf);
    uint32_t inner_len = get32(p->buf + 4);
    if (channel < CH_CMD || channel > CH_TOUCH) return -1;
    int hdr = (channel == CH_CMD || channel == CH_TOUCH) ? 8 : 12;
    if (inner_len < (uint32_t)hdr || inner_len > MAX_INNER) return -1;
    if (p->len < 8 + inner_len) return 0;

    const uint8_t *inner = p->buf + 8;
    uint32_t declared, msg, ts = 0;
    if (hdr == 8) {
        declared = get16(inner);
        if (get16(inner + 2) != 0) return -1;      /* reserved 必须 0 */
        msg = get32(inner + 4);
    } else {
        declared = get32(inner);
        ts = get32(inner + 4);
        msg = get32(inner + 8);
    }
    if (declared != inner_len - (uint32_t)hdr) return -1;
    if ((msg >> 16) != channel) return -1;

    if (declared > sizeof(p->out)) return -1;
    out->channel = channel;
    out->message = msg;
    out->timestamp = ts;
    if (declared) memcpy(p->out, inner + hdr, declared);   /* 同上: 先拷走再挪缓冲 */
    out->payload = p->out;
    out->payload_len = declared;

    /* 把已消费的字节挪走 (payload 指针在下次 feed 前有效) */
    memmove(p->buf, p->buf + 8 + inner_len, p->len - 8 - inner_len);
    p->len -= 8 + inner_len;
    return 1;
}

int cl_inner_parser_feed(cl_parser *p, const uint8_t *data, size_t n, cl_packet *out)
{
    if (n > sizeof(p->buf) - p->len) {
        p->len = 0;
        if (n > sizeof(p->buf)) return -1;
    }
    memcpy(p->buf + p->len, data, n);
    p->len += n;

    if (p->len < 8) return 0;
    uint32_t declared = get16(p->buf);
    uint32_t msg = get32(p->buf + 4);
    if (declared > 32768) return -1;                 /* 手机侧上限 */
    if (p->len < 8 + declared) return 0;

    out->channel = (uint16_t)(msg >> 16);
    out->message = msg;
    out->timestamp = 0;
    if (declared) memcpy(p->out, p->buf + 8, declared);   /* 先拷走, 免得下面 memmove 覆盖 */
    out->payload = p->out;
    out->payload_len = declared;

    memmove(p->buf, p->buf + 8 + declared, p->len - 8 - declared);
    p->len -= 8 + declared;
    return 1;
}
