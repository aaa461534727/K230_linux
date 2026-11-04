/* sig_dump.c —— 把板上算出的签名打出来, 回主机验 (定位「手机每次都弹授权」是不是签名不对) */
#include <stdio.h>
#include <string.h>
#include "rsa2048.h"
int main(void)
{
    unsigned char token[20];
    for (int i = 0; i < 20; i++) token[i] = (unsigned char)i;
    unsigned char sig[256];
    rsa2048_sign_sha1(token, sig);
    for (int i = 0; i < 256; i++) printf("%02x", sig[i]);
    printf("\n");
    printf("fp=%s\n", adb_pubkey_fp());
    return 0;
}
