/* log.h — 极简日志 (带墙钟时间, 与 v4l2_dec/drmplay 的日志风格一致, 便于同一份 log 对齐)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CARPLAY_LOG_H
#define CARPLAY_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static inline void cp_log(const char *tag, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    va_list ap;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] [%s] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define LOGI(...) cp_log("carplay", __VA_ARGS__)
#define LOGE(...) cp_log("carplay!", __VA_ARGS__)

#endif
