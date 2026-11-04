/* adb.c —— 最小 adb host (libusb + ADB 协议), 只为「取手机视频流」服务
 *
 * 为什么不用系统 adb 命令:
 *   ① 板子是 buildroot/ubuntu 裸环境, adb 版本不可控 (buildroot 里的 android-tools
 *      是 2013 年的 4.2.2, 没有 exec-out);
 *   ② 我们要的是**裸 H.264 字节流**直通 v4l2_dec —— 必须用 "exec:" 服务, 不能经
 *      "shell:" (PTY 会把 \n 改成 \r\n, 码流就废了);
 *   ③ 少一层进程/管道, 单核板上省一次拷贝。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "adb.h"
#include "rsa2048.h"

#define ADB_VERSION 0x01000001u
#define HDR_SIZE 24

static int g_verbose;
/* 本次进程内是否已经成功授权过一次。成功过之后, 重连先多试几次**纯签名**
 * (真 adb host 也是先签名), 只有连续被拒才发公钥 —— 发公钥就是手机上那个弹窗,
 * 能少发一次就少打扰主人一次。 */
static int g_auth_ok_seen;

#define LOG(...) do { fprintf(stderr, "[adb] " __VA_ARGS__); } while (0)
#define VLOG(...) do { if (g_verbose) fprintf(stderr, "[adb] " __VA_ARGS__); } while (0)

/* ★ 2026-11-03 实测 (ZTE 19d2:ffcf): adb 的 data_check 是**逐字节求和**,
 * 不是 crc32。用 crc32 时手机对 CNXN 一个字节都不回 (连 AUTH TOKEN 都没有),
 * 换成和立刻回应。这与 AOSP adb 的 ComputeChecksum() 一致:
 *     unsigned sum = 0; for (i) sum += data[i];
 * crc32 只出现在更上层的协议里, 传输层从来不用。 */
static uint32_t adb_checksum(const uint8_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return s;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *adb_strerror(int rc)
{
    switch (rc) {
    case 0: return "ok";
    case -1: return "未找到 adb 设备 (手机插好了吗? USB 调试开了吗?)";
    case -2: return "等待超时";
    case -3: return "设备未授权 (请解锁手机点『允许 USB 调试』)";
    case -4: return "USB 传输错误 (线/hub 供电?)";
    case -5: return "协议错误";
    default: return "未知错误";
    }
}

/* ---------- USB 传输 ---------- */

static int bulk_in(adb_dev *d, void *buf, int len, int timeout_ms)
{
    int got = 0;
    if (d->test_fd >= 0) {   /* 自测: 从 socketpair 读 */
        while (got < len) {
            ssize_t n = read(d->test_fd, (uint8_t *)buf + got, len - got);
            if (n <= 0) return -4;
            got += (int)n;
        }
        return got;
    }
    while (got < len) {
        int n = 0;
        int rc = libusb_bulk_transfer(d->h, d->ep_in, (uint8_t *)buf + got, len - got, &n, timeout_ms);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            if (got == 0) return -2;
            continue; /* 已经读了一半, 继续等, 否则流会错位 */
        }
        if (rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
            return -4;
        }
        if (rc < 0) return -4;
        if (n <= 0) continue;
        got += n;
    }
    return got;
}

static int bulk_out(adb_dev *d, const void *buf, int len)
{
    int done = 0;
    if (d->test_fd >= 0) {   /* 自测: 写到 socketpair */
        while (done < len) {
            ssize_t n = write(d->test_fd, (const uint8_t *)buf + done, len - done);
            if (n <= 0) return -4;
            done += (int)n;
        }
        return 0;
    }
    while (done < len) {
        int n = 0;
        int rc = libusb_bulk_transfer(d->h, d->ep_out, (uint8_t *)buf + done, len - done, &n, 5000);
        if (rc < 0) return -4;
        done += n;
    }
    return 0;
}

/* ★ 2026-11-03 实测 (ZTE 19d2:ffcf / sn 2f28d75a): 24B 头与载荷必须在**两个
 * USB 传输**里发。合成一个 31 B 的 URB 时, adbd 一个字节都不回 (IN 端点一直
 * NAK, 连 AUTH TOKEN 都没有), 而拆两步立刻收到 AUTH TOKEN=20B。
 * 这也和真正的 adb host 一致 (它本来就是 write(hdr) 然后 write(payload))。
 * 拆开写对 socketpair 自测无影响 (流式)。 */
static int bulk_out_split(adb_dev *d, const uint8_t *hdr, const void *data, int len)
{
    int rc = bulk_out(d, hdr, HDR_SIZE);
    VLOG("  URB#1 hdr 24 rc=%d\n", rc);
    if (rc < 0) return rc;
    if (len > 0) { rc = bulk_out(d, data, len); VLOG("  URB#2 data %d rc=%d\n", len, rc); }
    return rc;
}

int adb_send_packet(adb_dev *d, uint32_t local_id, uint32_t remote_id, const void *data, int len)
{
    uint8_t *pkt = malloc(HDR_SIZE + (len > 0 ? len : 0));
    if (!pkt) return -5;
    uint32_t cmd = ADB_CMD_WRTE;
    put32(pkt + 0, cmd);
    put32(pkt + 4, local_id);
    put32(pkt + 8, remote_id);
    put32(pkt + 12, (uint32_t)len);
    put32(pkt + 16, len > 0 ? adb_checksum((const uint8_t *)data, len) : 0);
    put32(pkt + 20, cmd ^ 0xFFFFFFFFu);
    if (len > 0) memcpy(pkt + HDR_SIZE, data, len);
    pthread_mutex_lock(&d->tx);
    int rc = bulk_out_split(d, pkt, pkt + HDR_SIZE, len);
    pthread_mutex_unlock(&d->tx);
    free(pkt);
    if (rc < 0) LOG("WRTE 发送失败 %d\n", rc);
    return rc;
}

static int send_raw(adb_dev *d, uint32_t cmd, uint32_t a0, uint32_t a1, const void *data, int len)
{
    uint8_t *pkt = malloc(HDR_SIZE + (len > 0 ? len : 0));
    if (!pkt) return -5;
    put32(pkt + 0, cmd);
    put32(pkt + 4, a0);
    put32(pkt + 8, a1);
    put32(pkt + 12, (uint32_t)len);
    put32(pkt + 16, len > 0 ? adb_checksum((const uint8_t *)data, len) : 0);
    put32(pkt + 20, cmd ^ 0xFFFFFFFFu);
    if (len > 0) memcpy(pkt + HDR_SIZE, data, len);
    pthread_mutex_lock(&d->tx);
    int rc = bulk_out_split(d, pkt, pkt + HDR_SIZE, len);
    pthread_mutex_unlock(&d->tx);
    free(pkt);
    VLOG("→ 0x%08x a0=%u a1=%u len=%d rc=%d\n", cmd, a0, a1, len, rc);
    return rc;
}

int adb_send_okay(adb_dev *d, uint32_t local_id, uint32_t remote_id)
{
    return send_raw(d, ADB_CMD_OKAY, local_id, remote_id, NULL, 0);
}

int adb_send_close(adb_dev *d, uint32_t local_id, uint32_t remote_id)
{
    return send_raw(d, ADB_CMD_CLSE, local_id, remote_id, NULL, 0);
}

void adb_free_packet(uint8_t *p) { free(p); }

int adb_recv_packet(adb_dev *d, int timeout_ms, uint32_t *cmd, uint32_t *a0, uint32_t *a1,
                    uint8_t **data, int *len)
{
    uint8_t hdr[HDR_SIZE];
    int rc = bulk_in(d, hdr, HDR_SIZE, timeout_ms);
    if (rc < 0) return rc;

    uint32_t c = get32(hdr + 0);
    uint32_t magic = get32(hdr + 20);
    uint32_t dl = get32(hdr + 12);
    if (magic != (c ^ 0xFFFFFFFFu)) { LOG("bad magic\n"); return -5; }
    if (dl > 8u * 1024u * 1024u) { LOG("payload too big %u\n", dl); return -5; }

    *cmd = c;
    *a0 = get32(hdr + 4);
    *a1 = get32(hdr + 8);
    *len = (int)dl;
    VLOG("← 0x%08x a0=%u a1=%u len=%u\n", c, *a0, *a1, dl);
    *data = NULL;
    if (dl) {
        *data = malloc(dl);
        if (!*data) return -5;
        rc = bulk_in(d, *data, (int)dl, timeout_ms > 0 ? timeout_ms : 1);
        if (rc < 0) { free(*data); *data = NULL; *len = 0; return rc; }
    }
    return 0;
}

/* ---------- 设备发现 ---------- */

static int match_adb_iface(libusb_device *dev, int *iface_out, uint8_t *in, uint8_t *out)
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return 0;

    int found = 0;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface *it = &cfg->interface[i];
        for (int a = 0; a < it->num_altsetting && !found; a++) {
            const struct libusb_interface_descriptor *id = &it->altsetting[a];
            if (id->bInterfaceClass != 0xFF || id->bInterfaceSubClass != 0x42 ||
                id->bInterfaceProtocol != 0x01)
                continue;
            uint8_t ei = 0, eo = 0;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                if ((ep->bmAttributes & 0x03) != 0x02) continue; /* bulk */
                if (ep->bEndpointAddress & 0x80) ei = ep->bEndpointAddress;
                else                             eo = ep->bEndpointAddress;
            }
            if (!ei || !eo) continue;
            *iface_out = id->bInterfaceNumber;
            *in = ei; *out = eo;
            found = 1;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

/* 应答一次 A_AUTH(TOKEN): 前两次回签名, 之后回公钥 (公钥要在手机上点允许)。
 * ★ 抽成函数是因为**握手之后还会再来 TOKEN**: 实测这台手机 CNXN 已经回
 *   device:: 了, 但紧接着发 OPEN 又收到 A_AUTH(TOKEN), 老代码在 open_channel
 *   里把它当无关包丢掉 => 通道永远等不到 OKAY (表现为「开通道失败 -2」)。 */
static void adb_auth_respond(adb_dev *d, const uint8_t token[20], int *sig_sent, int *key_sent)
{
    if (g_verbose) {
        char hx[64]; int n = 0;
        for (int i = 0; i < 20; i++) n += snprintf(hx + n, sizeof hx - n, "%02x", token[i]);
        VLOG("token=%s\n", hx);
    }
    if (*sig_sent < (g_auth_ok_seen ? 3 : 1)) {
        uint8_t sig[256];
        rsa2048_sign_sha1(token, sig);
        send_raw(d, ADB_CMD_AUTH, ADB_AUTH_SIGNATURE, 0, sig, sizeof sig);
        (*sig_sent)++;
        VLOG("已发签名 (%d)\n", *sig_sent);
    } else if (!*key_sent) {
        int blen = 0;
        const uint8_t *blob = adb_pubkey_blob(&blen);
        uint8_t *buf = malloc(blen + 32);
        memcpy(buf, blob, blen);
        int p = blen;
        buf[p++] = 0;
        const char *u = "k230@k230";
        memcpy(buf + p, u, strlen(u) + 1);
        p += (int)strlen(u) + 1;
        send_raw(d, ADB_CMD_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, buf, p);
        free(buf);
        *key_sent = 1;
        LOG("已发送公钥 (指纹 %s) —— **请在手机上点『允许』并勾选『一律允许』**\n", adb_pubkey_fp());
    }
}

static int adb_handshake(adb_dev *d, int timeout_ms);

int adb_connect(adb_dev *d, int timeout_ms, int verbose)
{
    memset(d, 0, sizeof *d);
    d->test_fd = -1;
    pthread_mutex_init(&d->tx, NULL);
    g_verbose = verbose;
    d->maxdata = 256 * 1024;

    if (libusb_init(&d->ctx) != 0) return -1;

    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(d->ctx, &list);
    libusb_device *found_dev = NULL;
    int iface = -1; uint8_t ei = 0, eo = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (match_adb_iface(list[i], &iface, &ei, &eo)) { found_dev = list[i]; break; }
    }
    if (!found_dev) { if (list) libusb_free_device_list(list, 1); libusb_exit(d->ctx); d->ctx = NULL; return -1; }

    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(found_dev, &dd);
    d->vid = dd.idVendor; d->pid = dd.idProduct;

    int rc = libusb_open(found_dev, &d->h);
    libusb_free_device_list(list, 1);
    /* ★ 2026-11-03 实测: 为了拿序列号先 open/close 一个临时 handle, 之后再 open
     * 主 handle, 这台 ZTE 的 adbd 就**彻底不应答** CNXN (IN 端点只 NAK, 连 AUTH
     * TOKEN 都不发); 改成只 open 一次、序列号用同一个 handle 读 ⇒ 立刻有回应。
     * 所以这里绝不能为了读字符串去开第二个 handle。 */
    if (rc != 0) { libusb_exit(d->ctx); d->ctx = NULL; return -4; }

    if (dd.iSerialNumber)
        libusb_get_string_descriptor_ascii(d->h, dd.iSerialNumber, (unsigned char *)d->serial, sizeof d->serial - 1);
    libusb_set_auto_detach_kernel_driver(d->h, 1);
    rc = libusb_claim_interface(d->h, iface);
    if (rc != 0) {
        LOG("claim interface %d 失败: %s\n", iface, libusb_error_name(rc));
        adb_disconnect(d);
        return -4;
    }
    /* 有些手机 (实测 ZTE 系) 的 adb 接口当前 alt setting != 0, 端点没使能 ⇒
     * host 发什么都没人接。显式切到 alt 0 (失败不致命, 只记日志)。 */
    { int arc = libusb_set_interface_alt_setting(d->h, iface, 0);
      VLOG("SET_INTERFACE(%d,alt=0) = %d (%s)\n", iface, arc, libusb_error_name(arc)); }
    /* 上一轮失败/被拔插可能留下 halt 的端点 (STALL), 清一下再握手 —— 实测有手机
     * 因为 IN 端点残留 halt 而表现为「发了 CNXN 永远没回复」。 */
    libusb_clear_halt(d->h, ei);
    libusb_clear_halt(d->h, eo);
    d->iface = iface; d->ep_in = ei; d->ep_out = eo;
    LOG("设备 %04x:%04x sn=%s iface=%d ep_in=0x%02x ep_out=0x%02x\n",
        d->vid, d->pid, d->serial[0] ? d->serial : "(none)", iface, ei, eo);

    return adb_handshake(d, timeout_ms);
}

static int adb_handshake(adb_dev *d, int timeout_ms)
{
    int rc;
    /* CNXN: 声明我们是 host */
    const char banner[] = "host::";
    rc = send_raw(d, ADB_CMD_CNXN, ADB_VERSION, (uint32_t)d->maxdata, banner, (int)sizeof banner);
    if (rc < 0) { adb_disconnect(d); return rc; }

    /* 等 CNXN; 中间可能来 AUTH */
    int sig_sent = 0, key_sent = 0;
    time_t deadline = time(NULL) + timeout_ms / 1000;
    for (;;) {
        uint32_t cmd, a0, a1; uint8_t *data; int len;
        rc = adb_recv_packet(d, 5000, &cmd, &a0, &a1, &data, &len);
        if (rc == -2) { /* 超时: 继续等, 但别无限等 */
            if (time(NULL) > deadline) { adb_disconnect(d); return -3; }
            continue;
        }
        if (rc < 0) { adb_disconnect(d); return rc; }

        if (cmd == ADB_CMD_CNXN) {
            if (len > 0) LOG("设备: %.*s\n", len, (char *)data);
            adb_free_packet(data);
            g_auth_ok_seen = 1;
            return 0;
        }
        if (cmd == ADB_CMD_AUTH && a0 == ADB_AUTH_TOKEN) {
            if (len != 20) { adb_free_packet(data); continue; }
            adb_auth_respond(d, data, &sig_sent, &key_sent);
            d->auth_sig_sent = sig_sent; d->auth_key_sent = key_sent;
            adb_free_packet(data);
            if (key_sent) deadline = time(NULL) + timeout_ms / 1000;
            continue;
        }
        adb_free_packet(data);
    }
}

int adb_connect_test(adb_dev *d, int fd, int timeout_ms, int verbose)
{
    memset(d, 0, sizeof *d);
    pthread_mutex_init(&d->tx, NULL);
    g_verbose = verbose;
    d->maxdata = 32 * 1024;
    d->test_fd = fd;
    d->iface = -1;
    return adb_handshake(d, timeout_ms);
}

void adb_disconnect(adb_dev *d)
{
    if (d->h) {
        libusb_release_interface(d->h, d->iface);
        libusb_close(d->h);
        d->h = NULL;
    }
    pthread_mutex_destroy(&d->tx);
    if (d->ctx) { libusb_exit(d->ctx); d->ctx = NULL; }
    free(d->rbuf); d->rbuf = NULL;
}

int adb_open_channel(adb_dev *d, uint32_t local_id, const char *service, uint32_t *remote_id, int timeout_ms)
{
    int rc = send_raw(d, ADB_CMD_OPEN, local_id, 0, service, (int)strlen(service) + 1);
    if (rc < 0) return rc;
    int sig_sent = d->auth_sig_sent, key_sent = d->auth_key_sent;
    time_t deadline = time(NULL) + timeout_ms / 1000;
    for (;;) {
        uint32_t cmd, a0, a1; uint8_t *data; int len;
        rc = adb_recv_packet(d, 5000, &cmd, &a0, &a1, &data, &len);
        if (rc == -2) { if (time(NULL) > deadline) return -2; continue; }
        if (rc < 0) return rc;
        /* ★ 握手之后手机还会再要一次鉴权 (实测这台 ZTE 就是): 处理掉并把 OPEN 重发 */
        if (cmd == ADB_CMD_AUTH && a0 == ADB_AUTH_TOKEN) {
            if (len == 20) adb_auth_respond(d, data, &sig_sent, &key_sent);
            adb_free_packet(data);
            deadline = time(NULL) + timeout_ms / 1000;
            if (send_raw(d, ADB_CMD_OPEN, local_id, 0, service, (int)strlen(service) + 1) < 0) return -4;
            continue;
        }
        if (cmd == ADB_CMD_CNXN) {   /* 手机重新宣告就绪: 重发 OPEN */
            adb_free_packet(data);
            deadline = time(NULL) + timeout_ms / 1000;
            if (send_raw(d, ADB_CMD_OPEN, local_id, 0, service, (int)strlen(service) + 1) < 0) return -4;
            continue;
        }
        /* ADB 约定: arg0 = 发送方自己的 channel id, arg1 = 对端的 channel id
         * => 判断"这个包是不是给我的"要看 arg1 */
        if (cmd == ADB_CMD_OKAY && a1 == local_id) {
            if (remote_id) *remote_id = a0;
            adb_free_packet(data);
            return 0;
        }
        if (cmd == ADB_CMD_CLSE && a1 == local_id) {
            adb_free_packet(data);
            LOG("服务被拒绝/关闭: %s\n", service);
            return -5;
        }
        VLOG("(等 OKAY) 收到 cmd=0x%08x a0=%u a1=%u local=%u\n", cmd, a0, a1, local_id);
        adb_free_packet(data); /* 其它通道的包 (握手后不该有) 丢弃 */
    }
}

