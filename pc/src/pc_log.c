#include "pc_log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Stop growing the file if something in the game turns out to be chatty; a
 * startup problem is always in the first few kilobytes anyway. */
#define PC_LOG_MAX_BYTES (4u * 1024u * 1024u)

static FILE*    s_log;
static unsigned s_written;
static int      s_full;

void pc_log_init(void) {
    time_t now;
    if (s_log) return;
    s_log = fopen("aclog.txt", "w");
    if (!s_log) return;
    now = time(NULL);
    fprintf(s_log, "Animal Crossing PC (multiplayer fork) log\n");
    fprintf(s_log, "started %s", ctime(&now));
    fprintf(s_log, "----------------------------------------\n");
    fflush(s_log);
}

void pc_log_vprintf(const char* fmt, va_list args) {
    int n;
    if (!s_log || s_full) return;
    n = vfprintf(s_log, fmt, args);
    if (n > 0) {
        s_written += (unsigned)n;
        if (s_written > PC_LOG_MAX_BYTES) {
            fprintf(s_log, "\n[log] size limit reached; no more output\n");
            s_full = 1;
        }
    }
    /* Flushed per line on purpose: the last line before a crash is the
     * interesting one, and a buffered tail would be exactly what we lose. */
    fflush(s_log);
}

void pc_log_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    pc_log_vprintf(fmt, args);
    va_end(args);
}

void pc_log_shutdown(void) {
    if (!s_log) return;
    fprintf(s_log, "[log] clean shutdown\n");
    fclose(s_log);
    s_log = NULL;
}
