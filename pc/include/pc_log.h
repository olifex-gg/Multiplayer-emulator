#ifndef PC_LOG_H
#define PC_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic log written to aclog.txt next to the executable.
 *
 * The port hides the console window by default (an unbuffered terminal write
 * per line tanks the frame rate on Windows), which also means a player who
 * hits a problem has nothing to send back. The log file costs almost nothing,
 * survives a crash because every line is flushed, and is the only way to see
 * what happened on someone else's machine. */
void pc_log_init(void);
void pc_log_printf(const char* fmt, ...);
void pc_log_vprintf(const char* fmt, va_list args);
void pc_log_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_LOG_H */
