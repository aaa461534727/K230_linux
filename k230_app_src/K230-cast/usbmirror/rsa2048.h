#ifndef RSA2048_H
#define RSA2048_H
#include <stdint.h>

/* 用内置(编译期)私钥做 RSA-2048 PKCS#1 v1.5 / SHA-1 签名。
 * 只用于 adb AUTH: 输入是设备给的 20 字节 token, 输出 256 字节签名。
 * 返回 0 = 成功。 */
int rsa2048_sign_sha1(const uint8_t token[20], uint8_t sig[256]);

/* 内置公钥 blob (安卓 ADB_AUTH 用), 长度 524 */
const uint8_t *adb_pubkey_blob(int *len);
const char *adb_pubkey_fp(void);

#endif

