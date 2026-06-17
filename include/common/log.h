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
#ifndef _LOG_H_
#define _LOG_H_

#include <stdio.h>
#include <time.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

void log_init(log_level_t level);

void _log_debug(const char* function, int line, const char* format, ...);
void _log_info(const char* function, int line, const char* format, ...);
void _log_warn(const char* function, int line, const char* format, ...);
void _log_error(const char* function, int line, const char* format, ...);

#define LOG_DEBUG(fmt, ...) _log_debug(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  _log_info(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  _log_warn(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) _log_error(__FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)

#endif // _LOG_H_