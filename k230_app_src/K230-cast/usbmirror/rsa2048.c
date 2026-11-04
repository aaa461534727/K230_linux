/* rsa2048.c —— 自带的 RSA-2048 PKCS#1 v1.5 签名 (只用 SHA-1 + 私钥模幂)
 *
 * 为什么自己实现: 板子是 buildroot/ubuntu 裸环境, 不保证有 libcrypto 头文件,
 * 也不想 fork openssl 命令行 (会话建立时每轮都要签一次)。这里只需要 32 个 64 位
 * 肢的 Montgomery 模幂, gcc 的 __int128 就够, 无任何外部依赖。
 */
#include <string.h>
#include "rsa2048.h"
#include "adb_key.h"

#define NL 32 /* 2048 / 64 */

static uint64_t g_n[NL], g_d[NL], g_rr[NL];
static int g_ready;

static void bytes_to_limbs(const unsigned char *b, uint64_t *out)
{
    for (int i = 0; i < NL; i++) {
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | b[i * 8 + k];
        out[i] = v;
    }
}

static void init_once(void)
{
    if (g_ready) return;
    bytes_to_limbs(AB_N, g_n);
    bytes_to_limbs(AB_D, g_d);
    bytes_to_limbs(AB_RR, g_rr);
    g_ready = 1;
}

/* r = a*b*R^-1 mod n  (CIOS Montgomery) */
static void mont_mul(const uint64_t *a, const uint64_t *b, uint64_t *r)
{
    uint64_t t[NL + 2];
    memset(t, 0, sizeof t);

    for (int i = 0; i < NL; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < NL; j++) {
            unsigned __int128 uv = (unsigned __int128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)uv;
            carry = (uint64_t)(uv >> 64);
        }
        unsigned __int128 uv = (unsigned __int128)t[NL] + carry;
        t[NL] = (uint64_t)uv;
        t[NL + 1] = (uint64_t)(uv >> 64);

        uint64_t m = t[0] * AB_N0INV;
        uv = (unsigned __int128)m * g_n[0] + t[0];
        carry = (uint64_t)(uv >> 64);
        for (int j = 1; j < NL; j++) {
            uv = (unsigned __int128)m * g_n[j] + t[j] + carry;
            t[j - 1] = (uint64_t)uv;
            carry = (uint64_t)(uv >> 64);
        }
        uv = (unsigned __int128)t[NL] + carry;
        t[NL - 1] = (uint64_t)uv;
        t[NL] = t[NL + 1] + (uint64_t)(uv >> 64);
        t[NL + 1] = 0;
    }

    /* 条件减一次 n: 结果 < 2n */
    int ge = (t[NL] != 0);
    if (!ge) {
        for (int j = NL - 1; j >= 0; j--) {
            if (t[j] != g_n[j]) { ge = (t[j] > g_n[j]); break; }
        }
    }
    if (ge) {
        uint64_t borrow = 0;
        for (int j = 0; j < NL; j++) {
            uint64_t sub = g_n[j] + borrow;
            uint64_t nb = (sub < g_n[j]) || (t[j] < sub);
            r[j] = t[j] - sub;
            borrow = nb;
        }
    } else {
        memcpy(r, t, NL * sizeof(uint64_t));
    }
}

static void limbs_to_bytes(const uint64_t *l, unsigned char *out)
{
    for (int i = 0; i < NL; i++)
        for (int k = 0; k < 8; k++) out[i * 8 + k] = (unsigned char)(l[i] >> (8 * k));
}

/* PKCS#1 的 EM/签名是**大端**整数, 而 adb_key.h 里的 n/d/rr 是小端存储, 故分开两套转换 */
static void be_to_limbs(const unsigned char *b, uint64_t *out)
{
    unsigned char tmp[256];
    for (int i = 0; i < 256; i++) tmp[i] = b[255 - i];   /* 翻转成小端 */
    bytes_to_limbs(tmp, out);
}

static void limbs_to_be(const uint64_t *l, unsigned char *out)
{
    unsigned char tmp[256];
    limbs_to_bytes(l, tmp);
    for (int i = 0; i < 256; i++) out[i] = tmp[255 - i];
}

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint8_t blk[64];
    size_t total = len;
    /* 单块足够 (token 只有 20 字节): 20 + 0x80 + 0 + len*8 */
    memset(blk, 0, sizeof blk);
    memcpy(blk, msg, len);
    blk[len] = 0x80;
    uint64_t bits = (uint64_t)total * 8;
    for (int k = 0; k < 8; k++) blk[56 + k] = (uint8_t)(bits >> (8 * (7 - k)));

    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | blk[i * 4 + 3];
    for (int i = 16; i < 80; i++) {
        uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(hs[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(hs[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(hs[i] >> 8);
        out[i * 4 + 3] = (uint8_t)hs[i];
    }
}

/* PKCS#1 v1.5: EM = 00 01 FF..FF 00 || DigestInfo(SHA1) */
static const uint8_t DIGEST_INFO_SHA1[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14
};

int rsa2048_sign_sha1(const uint8_t token[20], uint8_t sig[256])
{
    init_once();

    uint8_t em[256];
    uint8_t hash[20];
    sha1(token, 20, hash);

    em[0] = 0x00;
    em[1] = 0x01;
    /* 256 - 2 - 1 - (15+20) = 218 字节 0xFF */
    memset(em + 2, 0xFF, 256 - 3 - 35);
    em[256 - 36] = 0x00;
    memcpy(em + 256 - 35, DIGEST_INFO_SHA1, 15);
    memcpy(em + 256 - 20, hash, 20);

    uint64_t x[NL], acc[NL], one[NL];

    /* x = em*R mod n (进 Montgomery 域) */
    be_to_limbs(em, x);
    mont_mul(x, g_rr, x);

    /* acc = R mod n (= 1 进域) */
    memset(one, 0, sizeof one);
    one[0] = 1;
    mont_mul(one, g_rr, acc);

    /* 平方乘, 从最高位开始 */
    for (int bit = 2047; bit >= 0; bit--) {
        mont_mul(acc, acc, acc);
        if ((g_d[bit / 64] >> (bit % 64)) & 1) mont_mul(acc, x, acc);
    }
    /* 出 Montgomery 域: sig = mont(acc, 1) */
    memset(one, 0, sizeof one);
    one[0] = 1;
    mont_mul(acc, one, acc);

    limbs_to_be(acc, sig);
    return 0;
}

const uint8_t *adb_pubkey_blob(int *len)
{
    if (len) *len = (int)sizeof(AB_BLOB);
    return AB_BLOB;
}

const char *adb_pubkey_fp(void) { return AB_FP; }

