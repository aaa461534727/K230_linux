/*
 * ts_demux_test.c — ts_demux 的离线自测 (host 编译, 不需要板子/手机)
 *
 * 目的: 证明 "RTP PT=33 (MPEG-TS) → H.264 ES" 这条路径是对的。
 *
 * 编译/跑:
 *   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 -c:v libx264 \
 *          -profile:v high -g 30 -bf 0 -pix_fmt yuv420p -f mpegts /tmp/t.ts
 *   gcc -O2 -Wall -o /tmp/ts_demux_test ts_demux_test.c ../sink/ts_demux.c
 *   /tmp/ts_demux_test /tmp/t.ts /tmp/out.h264
 *   ffprobe -v error -count_frames -select_streams v:0 \
 *           -show_entries stream=nb_read_frames,width,height -of default=nw=1 /tmp/out.h264
 */
#include "../sink/ts_demux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_out;
static unsigned long g_nals, g_bytes;

static void on_nal(void *user, const unsigned char *nal, int len)
{
    (void)user;
    static const unsigned char sc[4] = { 0, 0, 0, 1 };
    fwrite(sc, 1, 4, g_out);
    fwrite(nal, 1, (size_t)len, g_out);
    g_nals++;
    g_bytes += (unsigned long)len;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <in.ts> <out.h264>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror("open in");
        return 2;
    }
    g_out = fopen(argv[2], "wb");
    if (!g_out) {
        perror("open out");
        return 2;
    }

    ts_demux_t *d = ts_demux_new(on_nal, NULL);
    if (!d) {
        fprintf(stderr, "ts_demux_new 失败\n");
        return 2;
    }

    /* 按真实 RTP 的样子喂: 每包 7×188 = 1316 字节 (Miracast 常见封装) */
    unsigned char buf[7 * 188];
    int n;
    long reads = 0;
    while ((n = (int)fread(buf, 1, sizeof(buf), in)) > 0) {
        ts_demux_feed(d, buf, n);
        reads++;
    }
    ts_demux_flush(d);      /* 会话结束: 把尾巴也吐出来 */
    fclose(in);
    fclose(g_out);

    ts_demux_stats_t st;
    ts_demux_get_stats(d, &st);
    printf("reads=%ld ts_pkts=%lu pes=%lu nals_out=%lu es_bytes=%lu\n",
           reads, st.ts_pkts, st.pes_pkts, g_nals, st.bytes);
    printf("pat_seen=%d pmt_seen=%d pmt_pid=%d video_pid=%d cc_errors=%lu\n",
           st.pat_seen, st.pmt_seen, st.pmt_pid, st.video_pid, st.cc_errors);

    int ok = st.pat_seen && st.pmt_seen && st.video_pid > 0 && g_nals > 0;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    ts_demux_free(d);
    return ok ? 0 : 1;
}
