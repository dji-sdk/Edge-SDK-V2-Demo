/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * All information contained herein is, and remains, the property of DJI.
 * The intellectual and technical concepts contained herein are proprietary
 * to DJI and may be covered by U.S. and foreign patents, patents in process,
 * and protected by trade secret or copyright law.  Dissemination of this
 * information, including but not limited to data and other proprietary
 * material(s) incorporated within the information, in any form, is strictly
 * prohibited without the express written consent of DJI.
 *
 * If you receive this source code without DJI's authorization, you may not
 * further disseminate the information, and you must immediately remove the
 * source code and notify DJI of its removal. DJI reserves the right to pursue
 * legal actions against you for any loss(es) or damage(s) caused by your
 * failure to do so.
 *
 *********************************************************************
 */
#include "common/log.h"
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>

static log_level_t g_log_level = LOG_LEVEL_DEBUG;

static void get_timestamp(char *buffer, size_t buffer_size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    time_t now = tv.tv_sec;
    struct tm *tm_info = localtime(&now);

    snprintf(buffer, buffer_size, "[%02d:%02d:%02d.%06ld]",
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec, tv.tv_usec);
}

static const char* get_level_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG:
            return "DEBUG";
        case LOG_LEVEL_INFO:
            return "INFO ";
        case LOG_LEVEL_WARN:
            return "WARN ";
        case LOG_LEVEL_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

void log_init(log_level_t level) {
    g_log_level = level;
}

static void _log_print_with_location(log_level_t level, const char* function, int line, const char *format, va_list args) {
    if (level < g_log_level) {
        return;
    }

    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    const char* functionname = strrchr(function, '/');
    if (!functionname) functionname = function;
    else ++functionname;

    printf("%s [%s] [%s:%d] ", timestamp, get_level_string(level), functionname, line);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
}

void _log_debug(const char* function, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    _log_print_with_location(LOG_LEVEL_DEBUG, function, line, format, args);
    va_end(args);
}

void _log_info(const char* function, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    _log_print_with_location(LOG_LEVEL_INFO, function, line, format, args);
    va_end(args);
}

void _log_warn(const char* function, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    _log_print_with_location(LOG_LEVEL_WARN, function, line, format, args);
    va_end(args);
}

void _log_error(const char* function, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    _log_print_with_location(LOG_LEVEL_ERROR, function, line, format, args);
    va_end(args);
}