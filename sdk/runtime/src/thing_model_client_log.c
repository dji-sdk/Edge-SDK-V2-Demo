#include "thing_model_client_log.h"
#include "thing_model_client/thing_model_client_osal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#  include <strings.h>          /* strcasecmp */
#  include <time.h>             /* clock_gettime */
#  include <unistd.h>           /* isatty / fileno */
#  include <pthread.h>          /* static-initialized output mutex */
#  define TM_LOG_HAS_POSIX_TIME 1
#  define TM_LOG_HAS_GETENV     1
#  define TM_LOG_HAS_ISATTY     1
#else
#  define TM_LOG_HAS_POSIX_TIME 0
#  define TM_LOG_HAS_GETENV     0
#  define TM_LOG_HAS_ISATTY     0
#endif

/* Output serialization. A whole log line is composed into one buffer and emitted under
 * this lock, so concurrent tasks/threads cannot interleave or tear a line apart.
 * POSIX: a static-initialized pthread mutex (no init order dependency).
 * RTOS: an OSAL mutex, created lazily on first use (LOG may run before other init). */
#if TM_LOG_HAS_POSIX_TIME
static pthread_mutex_t s_out_lock = PTHREAD_MUTEX_INITIALIZER;
static void _out_lock(void)   { pthread_mutex_lock(&s_out_lock); }
static void _out_unlock(void) { pthread_mutex_unlock(&s_out_lock); }
#else
static void *s_out_lock = NULL;
static void _out_lock(void)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (!o || !o->mutex_lock) return;
    if (!s_out_lock && o->mutex_create) s_out_lock = o->mutex_create();
    if (s_out_lock) o->mutex_lock(s_out_lock);
}
static void _out_unlock(void)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (o && o->mutex_unlock && s_out_lock) o->mutex_unlock(s_out_lock);
}
#endif

static E_ThingModelLogLevel s_level = THING_MODEL_LOG_INFO;
static int s_level_inited = 0;

/* ANSI color — enabled only on host and when stderr is a tty; also supports THING_MODEL_LOG_COLOR=
 *   auto (default, isatty auto-detect)
 *   always (force on, keeps ANSI escapes even when redirected to a file)
 *   never  (force off, log-file friendly or dumb terminals that do not support ANSI) */
#if TM_LOG_HAS_ISATTY
typedef enum { COLOR_AUTO=0, COLOR_ALWAYS, COLOR_NEVER } color_mode_t;
static int s_color_inited = 0;
static int s_color_on     = 0;       /* whether actually on (computed from auto + isatty) */

static void _init_color(void)
{
    color_mode_t mode = COLOR_AUTO;
    const char *v = getenv("THING_MODEL_LOG_COLOR");
    if (v && *v) {
        if      (strcasecmp(v, "always") == 0) mode = COLOR_ALWAYS;
        else if (strcasecmp(v, "never")  == 0) mode = COLOR_NEVER;
        /* auto / others -> COLOR_AUTO */
    }
    if (mode == COLOR_ALWAYS) s_color_on = 1;
    else if (mode == COLOR_NEVER) s_color_on = 0;
    else /* AUTO */ s_color_on = isatty(fileno(stderr)) ? 1 : 0;
    s_color_inited = 1;
}

/* Color table (in E_ThingModelLogLevel order: DEBUG/INFO/WARN/ERROR):
 *   DEBUG = gray (90)
 *   INFO  = green (32)
 *   WARN  = yellow (33)
 *   ERROR = red (31)
 * Returns an empty string when no color. */
static const char *_color_for(int lvl)
{
    if (!s_color_on) return "";
    switch (lvl) {
        case THING_MODEL_LOG_DEBUG: return "\033[90m";
        case THING_MODEL_LOG_INFO:  return "\033[32m";
        case THING_MODEL_LOG_WARN:  return "\033[33m";
        case THING_MODEL_LOG_ERROR: return "\033[31m";
        default: return "";
    }
}
static const char *_color_reset(void) { return s_color_on ? "\033[0m" : ""; }
#endif /* TM_LOG_HAS_ISATTY */

#if TM_LOG_HAS_GETENV
/* On the first LOG output, check the environment variable THING_MODEL_LOG_LEVEL.
 * A user calling ThingModelLog_SetLevel() takes effect immediately and marks it inited, skipping the env check. */
static void _init_level_from_env(void)
{
    const char *v = getenv("THING_MODEL_LOG_LEVEL");
    if (v && *v) {
        if      (strcasecmp(v, "debug") == 0) s_level = THING_MODEL_LOG_DEBUG;
        else if (strcasecmp(v, "info")  == 0) s_level = THING_MODEL_LOG_INFO;
        else if (strcasecmp(v, "warn")  == 0) s_level = THING_MODEL_LOG_WARN;
        else if (strcasecmp(v, "error") == 0) s_level = THING_MODEL_LOG_ERROR;
        /* Illegal value: silent, keep the default INFO */
    }
    s_level_inited = 1;
}
#endif

void ThingModelLog_SetLevel(E_ThingModelLogLevel lvl)
{
    s_level = lvl;
    s_level_inited = 1;          /* after an explicit set, env no longer takes effect */
}

#if TM_LOG_HAS_POSIX_TIME
/* Linux keeps a copy of every log line in a file, so remote/headless runs (no console attached, or a
 * front-end that scrapes a file instead of the tty) still get the full timestamped trail — the same
 * lines as the console, just plain (never ANSI-colored).
 *   path: env THING_MODEL_LOG_FILE overrides ("0"/"off" disables the file entirely).
 *   default (env unset): "/var/log/psdk_iot.log" — the ops-standard, persistent location that an
 *     auto-started service (systemd/rc.local, whose CWD is "/") should use. A relative default would
 *     land in "/" there, which is wrong. If that path is not writable (e.g. a non-root dev run from a
 *     terminal), fall back to "psdk_iot.log" in the cwd so development still gets a file.
 * Opened lazily on the first log under the shared output lock (no extra sync), append mode, and
 * line-buffered so `tail -f` follows in real time. */
static FILE *s_logf        = NULL;
static int   s_logf_inited = 0;
static FILE *_log_file(void)
{
    if (!s_logf_inited) {
        s_logf_inited = 1;
        const char *path = getenv("THING_MODEL_LOG_FILE");
        if (path && (!strcmp(path, "0") || !strcasecmp(path, "off"))) return NULL;
        if (path && path[0]) {
            s_logf = fopen(path, "a");                    /* explicit path: honor it, no fallback */
        } else {
            s_logf = fopen("/var/log/psdk_iot.log", "a"); /* deploy default (root/systemd) */
            if (!s_logf) s_logf = fopen("psdk_iot.log", "a");  /* non-root dev fallback: cwd */
        }
        if (s_logf) setvbuf(s_logf, NULL, _IOLBF, 0);   /* line-buffered: tail -f follows live */
    }
    return s_logf;
}
#endif

void ThingModelLog_Print(E_ThingModelLogLevel lvl, const char *file, int line, const char *fmt, ...)
{
#if TM_LOG_HAS_GETENV
    if (!s_level_inited) _init_level_from_env();
#endif
    if (lvl < s_level) return;
    static const char *prefix[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };

#if TM_LOG_HAS_ISATTY
    if (!s_color_inited) _init_color();
    const char *clr   = _color_for(lvl);
    const char *creset = _color_reset();
#else
    const char *clr    = "";
    const char *creset = "";
#endif

    /* Column 2 (file:line): __FILE__ is a full path — take the basename, join with the
     * line number into one string. Fixed display width TM_LOG_SITE_W so column 3 (the
     * message) starts at the same offset on every line. When over-long, keep the TAIL
     * (right-truncate) so the ".c:line" — the useful part for locating — is preserved. */
    #define TM_LOG_SITE_W 28
    const char *base = file;
    for (const char *p = file; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    char raw[64];
    snprintf(raw, sizeof(raw), "%s:%d", base, line);
    char site[TM_LOG_SITE_W + 1];
    int  rawlen = (int)strlen(raw);
    if (rawlen > TM_LOG_SITE_W)
        snprintf(site, sizeof(site), "%s", raw + (rawlen - TM_LOG_SITE_W));  /* keep tail */
    else
        snprintf(site, sizeof(site), "%-*s", TM_LOG_SITE_W, raw);            /* pad right */

    /* Compose the whole line into one buffer, then emit it with a single write — a log line is
     * never split across multiple I/O calls, so concurrent tasks cannot interleave. The buffer
     * is static (not on the stack) to keep this function's stack footprint tiny on RTOS, where
     * it runs on deep call chains (tcpip -> mqtt -> fanout -> handler -> LOG) with small task
     * stacks; the output lock also guards the shared buffer for the whole compose+emit span. */
    static char out[1152];
    int    n = 0;

    _out_lock();

#if TM_LOG_HAS_POSIX_TIME
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    n = snprintf(out, sizeof(out), "[%02d:%02d:%02d.%03ld %s]   %s   ",
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec/1000000,
                 prefix[lvl], site);
#else
    /* RTOS / embedded: use OSAL.time_ms to get ms since boot, no wall-clock.
     * On embedded, stderr is often retargeted to UART or simply equivalent to stdout — printf as fallback.
     * No ANSI color (TM_LOG_HAS_ISATTY=0). */
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    uint64_t t_ms = o ? o->time_ms() : 0;
    unsigned long s = (unsigned long)(t_ms / 1000U);
    unsigned ms = (unsigned)(t_ms % 1000U);
    n = snprintf(out, sizeof(out), "[%lu.%03u %s]   %s   ", s, ms, prefix[lvl], site);
#endif

    if (n < 0) n = 0;
    if (n > (int)sizeof(out) - 1) n = (int)sizeof(out) - 1;

    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(out + n, sizeof(out) - (size_t)n, fmt, ap);
    va_end(ap);
    if (m > 0) {
        n += m;
        if (n > (int)sizeof(out) - 1) n = (int)sizeof(out) - 1;  /* truncated */
    }

#if TM_LOG_HAS_POSIX_TIME
    /* Console (stderr): color wraps the line, reset before '\n' so it can't leak to the next line.
     * clr/creset are "" when color is off, so the fputs calls are then harmless no-ops. */
    fputs(clr, stderr);
    fwrite(out, 1, (size_t)n, stderr);
    fputs(creset, stderr);
    fputc('\n', stderr);
    /* Linux also keeps a plain copy in a file (same line, no color) — see _log_file(). */
    FILE *lf = _log_file();
    if (lf) { fwrite(out, 1, (size_t)n, lf); fputc('\n', lf); }
#else
    printf("%s\r\n", out);
#endif
    _out_unlock();
}

/* ---- wire capture (THING_MODEL_WIRE_LOG=1): full raw sent/received text, not truncated ----
 * Universal debug aid, off by default. Portable across host (stderr) and RTOS (retargeted stdout);
 * on embedded without getenv it stays disabled. */
static int s_wire = -1;

int ThingModelLog_WireEnabled(void)
{
#if TM_LOG_HAS_GETENV
    if (s_wire < 0) {
        const char *v = getenv("THING_MODEL_WIRE_LOG");
        s_wire = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
    }
#else
    if (s_wire < 0) s_wire = 0;   /* no getenv on embedded → wire capture off */
#endif
    return s_wire;
}

void ThingModelLog_Wire(const char *dir, const char *topic, const void *payload, size_t len)
{
    if (!ThingModelLog_WireEnabled()) return;
#if TM_LOG_HAS_POSIX_TIME
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld WIRE] %s %s (%lu B)\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec/1000000,
            dir ? dir : "", topic ? topic : "", (unsigned long)len);
    if (payload && len) fwrite(payload, 1, len, stderr);   /* full raw text, not truncated */
    fputc('\n', stderr);
    fflush(stderr);
#else
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    uint64_t t_ms = o ? o->time_ms() : 0;
    printf("[%lu.%03u WIRE] %s %s (%lu B)\r\n",
           (unsigned long)(t_ms/1000U), (unsigned)(t_ms%1000U),
           dir ? dir : "", topic ? topic : "", (unsigned long)len);
    if (payload && len) fwrite(payload, 1, len, stdout);
    printf("\r\n");
#endif
}
