/* adb_cmd.c —— 用板上的最小 adb host 跑一条命令, 把手机回的内容原样打出来。
 * 排查「通道开得起来但手机不吐数据」时用: 先确认 exec:/shell: 本身通不通。
 *
 * 用法: ./adb_cmd "exec:echo hi"        # exec 服务 (无 PTY, 适合二进制)
 *       ./adb_cmd "shell:ls -l /system/bin/screenrecord"
 *       ./adb_cmd -v "exec:screenrecord --help"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "adb.h"

static long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
    int verbose = 0, wait_ms = 8000, argi = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")) { verbose = 1; argi++; }
    if (argc > argi + 1) wait_ms = atoi(argv[argi + 1]);
    if (argc <= argi) { fprintf(stderr, "用法: %s [-v] <服务名, 如 exec:echo hi> [等待秒*1000]\n", argv[0]); return 2; }

    adb_dev d;
    int rc = adb_connect(&d, 180000, verbose);
    if (rc != 0) { fprintf(stderr, "连不上: %s\n", adb_strerror(rc)); return 10; }

    uint32_t remote = 0;
    rc = adb_open_channel(&d, 1, argv[argi], &remote, 8000);
    if (rc != 0) { fprintf(stderr, "开通道失败 %d\n", rc); adb_disconnect(&d); return 11; }
    fprintf(stderr, "✓ 通道已开 (remote=%u), 读 %d ms\n", remote, wait_ms);

    long total = 0, t0 = now_ms();
    while (now_ms() - t0 < wait_ms) {
        uint32_t cmd, a0, a1; uint8_t *data; int len;
        rc = adb_recv_packet(&d, 1000, &cmd, &a0, &a1, &data, &len);
        if (rc == -2) continue;
        if (rc < 0) { fprintf(stderr, "读错误 %d\n", rc); break; }
        if (cmd == ADB_CMD_WRTE && a1 == 1) {
            write(STDOUT_FILENO, data, len);
            total += len;
            adb_send_okay(&d, a1, a0);
        } else if (cmd == ADB_CMD_CLSE && a1 == 1) {
            adb_free_packet(data);
            fprintf(stderr, "通道关闭, 共 %ld B\n", total);
            break;
        } else if (cmd == ADB_CMD_OKAY) {
            /* ignore */
        }
        adb_free_packet(data);
    }
    fprintf(stderr, "结束: %ld B / %.1f s\n", total, (now_ms() - t0) / 1000.0);
    adb_disconnect(&d);
    return 0;
}
