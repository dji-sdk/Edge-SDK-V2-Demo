/**
 * @file    thing_model_client_log.h
 * @brief   Minimal logging macros.
 *
 * Default level INFO. Two ways to change the level at runtime:
 *   1. Environment variable THING_MODEL_LOG_LEVEL=debug|info|warn|error (case-insensitive)
 *      — read once at the first log output after process start, not re-read afterwards
 *   2. Code: ThingModelLog_SetLevel(THING_MODEL_LOG_DEBUG)
 *      — after an explicit set, the env var is ignored
 *
 * The DEBUG level prints the full JSON of the outgoing publish and incoming message,
 * for debugging the wire format / reconciling with the broker docs.
 */
#ifndef THING_MODEL_CLIENT_LOG_H
#define THING_MODEL_CLIENT_LOG_H

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THING_MODEL_LOG_DEBUG = 0,
    THING_MODEL_LOG_INFO  = 1,
    THING_MODEL_LOG_WARN  = 2,
    THING_MODEL_LOG_ERROR = 3,
} E_ThingModelLogLevel;

void ThingModelLog_SetLevel(E_ThingModelLogLevel lvl);
void ThingModelLog_Print(E_ThingModelLogLevel lvl, const char *file, int line, const char *fmt, ...);

/** wire capture switch: environment variable THING_MODEL_WIRE_LOG=1 turns it on (0/empty/unset=off).
 *  Once on, the runtime prints the "full raw text" of every sent/received MQTT message — not truncated,
 *  including long RID/ADS-B packets (~20KB). Independent of THING_MODEL_LOG_LEVEL: no need to enable debug,
 *  and not subject to the DEBUG log's 1024B truncation limit. */
int  ThingModelLog_WireEnabled(void);
/** Print one wire raw text; dir is the direction marker (e.g. "→PUB" / "←RCV").
 *  Returns immediately when the wire switch is off, so the caller need not check itself. */
void ThingModelLog_Wire(const char *dir, const char *topic, const void *payload, size_t len);

#define LOG_DEBUG(...) ThingModelLog_Print(THING_MODEL_LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ThingModelLog_Print(THING_MODEL_LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ThingModelLog_Print(THING_MODEL_LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ThingModelLog_Print(THING_MODEL_LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

/** Record the "last error" (thread-local storage), for ThingModelClientSession_GetLastError to read.
 *  Internal helper — any failure path in the runtime may call it; fmt is a printf-style short description.
 *  impl is in session.c. */
void thing_model_record_error(int32_t rc, int32_t transport_rc, int32_t errno_copy,
                              const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_LOG_H */
