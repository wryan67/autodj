#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void log_at(const char *cat, const char *level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char body[1024];
    va_list ap;
    int ms;

    if (!cat || !cat[0]) {
        cat = "autodj";
    }
    if (!level) {
        level = "info";
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    ms = (int)(ts.tv_nsec / 1000000L);

    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    flockfile(stderr);
    fprintf(stderr,
            "[%s] %04d-%02d-%02d %02d:%02d:%02d.%03d %s  %s\n",
            cat,
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec,
            ms,
            level,
            body);
    fflush(stderr);
    funlockfile(stderr);
}
