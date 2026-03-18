#include "log.h"

#include <time.h>
#include <stdarg.h>
#include <string.h>

static FILE *g_logf = NULL;

static void ts(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

int log_init(const char *path) {
    if (g_logf) return 0;
    g_logf = fopen(path, "a");
    return g_logf ? 0 : -1;
}

void log_close() {
    if (g_logf) {
        fclose(g_logf);
        g_logf = NULL;
    }
}

static void vlog_write(const char *lvl, const char *fmt, va_list ap) {
    char t[32]; ts(t, sizeof(t));
    
    // Always log to terminal (stdout/stderr)
    FILE *terminal = strcmp(lvl, "ERROR") == 0 ? stderr : stdout;
    fprintf(terminal, "[%s] %s ", t, lvl);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    vfprintf(terminal, fmt, ap_copy);
    fprintf(terminal, "\n");
    fflush(terminal);
    va_end(ap_copy);
    
    // Also log to file if available
    if (!g_logf) return; // Only terminal logging if no file
    fprintf(g_logf, "[%s] %s ", t, lvl);
    vfprintf(g_logf, fmt, ap);
    fprintf(g_logf, "\n");
    fflush(g_logf);
}

void log_info(const char *fmt, ...) {
    va_list ap; 
    va_start(ap, fmt);
    vlog_write("INFO", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap; 
    va_start(ap, fmt);
    vlog_write("ERROR", fmt, ap);
    va_end(ap);
}
