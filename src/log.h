#ifndef AUTODJ_LOG_H
#define AUTODJ_LOG_H

void log_at(const char *cat, const char *level, const char *fmt, ...);

#define log_info(cat, ...) log_at((cat), "info", __VA_ARGS__)
#define log_warn(cat, ...) log_at((cat), "warn", __VA_ARGS__)
#define log_error(cat, ...) log_at((cat), "error", __VA_ARGS__)

#endif
