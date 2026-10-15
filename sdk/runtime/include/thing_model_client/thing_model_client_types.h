/**
 * @file    thing_model_client_types.h
 * @brief   Common type definitions: return codes + generic reply struct.
 */
#ifndef THING_MODEL_CLIENT_TYPES_H
#define THING_MODEL_CLIENT_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Version number — SemVer (https://semver.org)
 *   MAJOR  breaking change     (API/ABI incompatible)
 *   MINOR  backward-compatible feature addition
 *   PATCH  backward-compatible bug fix
 * When the generator (protoc-gen-tm) emits code, it embeds the generation-time version into <SVC>_GeneratorVersion();
 * at startup the runtime checks it matches its own major version, and errors on mismatch (API incompatible).
 * ============================================================ */
#define THING_MODEL_VERSION_MAJOR  0
#define THING_MODEL_VERSION_MINOR  3
#define THING_MODEL_VERSION_PATCH  0
#define THING_MODEL_VERSION_STRING "0.3.0"

/** Encoded as a uint32 for easy numeric comparison: (MAJOR << 16) | (MINOR << 8) | PATCH */
#define THING_MODEL_VERSION_NUMBER \
    ((THING_MODEL_VERSION_MAJOR << 16) | (THING_MODEL_VERSION_MINOR << 8) | THING_MODEL_VERSION_PATCH)

typedef int32_t T_ThingModelReturnCode;

#define THING_MODEL_RC_OK                       0
#define THING_MODEL_RC_INVALID_ARG              -1
#define THING_MODEL_RC_OUT_OF_MEMORY            -2
#define THING_MODEL_RC_TIMEOUT                  -3
#define THING_MODEL_RC_TRANSPORT_ERROR          -4
#define THING_MODEL_RC_AUTH_FAILED              -5
#define THING_MODEL_RC_BUSY                     -6
#define THING_MODEL_RC_NOT_REGISTERED           -7
#define THING_MODEL_RC_PAYLOAD_TOO_LARGE        -8
#define THING_MODEL_RC_DECODE_ERROR             -9
#define THING_MODEL_RC_CANCELED                 -10
#define THING_MODEL_RC_NOT_CONNECTED            -11
#define THING_MODEL_RC_VERSION_MISMATCH         -12
#define THING_MODEL_RC_UNKNOWN                  -99

#define THING_MODEL_IS_OK(rc) ((rc) == THING_MODEL_RC_OK)

/* ============================================================
 * Version number helpers
 * ============================================================ */
/** Get the runtime version string, e.g. "0.2.0" */
static inline const char *ThingModelVersion_Get(void) {
    return THING_MODEL_VERSION_STRING;
}

/** Get the runtime version number (for comparison) */
static inline uint32_t ThingModelVersion_GetNumber(void) {
    return THING_MODEL_VERSION_NUMBER;
}

/** Check whether the generated-code version (return value of <Cap>_GeneratorVersion()) is compatible with the runtime major version.
 *  SemVer: a different MAJOR = API/ABI incompatible, returns false. */
static inline bool ThingModelVersion_Compatible(const char *generated_version) {
    if (!generated_version) return false;
    int major = 0;
    const char *p = generated_version;
    while (*p >= '0' && *p <= '9') { major = major * 10 + (*p - '0'); p++; }
    return major == THING_MODEL_VERSION_MAJOR;
}

/**
 * Topic naming style.
 *
 *   V2 (consistent with the PSDK Things-Model docs):
 *     topic:  thing/product/<sn>/<channel>   (no leading /, owner fixed to product, no cap segment)
 *     method: "<cap>_<base>"                 (e.g. camera_start_shoot_photo)
 *     channels: services / services_reply / property/get / events / osd / state
 *
 *   V1 (alternative topic layout):
 *     topic:  /thing/<owner>/<sn>/<cap>/<channel>   (cap segment used for demux)
 *     method: bare name "<base>"                     (e.g. start_shoot_photo)
 *     channels: service / service_reply / properties/get / event / osd / state
 *
 * **Default**: V2. Both V1 and V2 are supported by the runtime and generated
 * code; the default is V2.
 *
 * Within a single session all cap clients use the same style; selected by
 * ThingModelClientSession_Config_t.topic_style.
 */
typedef enum {
    THING_MODEL_TOPIC_STYLE_V2 = 0,   /* default (struct zero-init) */
    THING_MODEL_TOPIC_STYLE_V1 = 1,   /* alternative topic layout                       */
} E_ThingModelTopicStyle;

/** 9 MQTT channels; generated code uses these to index the topic table. */
typedef enum {
    THING_MODEL_TOPIC_SERVICE         = 0,
    THING_MODEL_TOPIC_SERVICE_REPLY   = 1,
    THING_MODEL_TOPIC_PROP_GET        = 2,
    THING_MODEL_TOPIC_PROP_GET_REPLY  = 3,
    THING_MODEL_TOPIC_PROP_SET        = 4,
    THING_MODEL_TOPIC_PROP_SET_REPLY  = 5,
    THING_MODEL_TOPIC_EVENT           = 6,
    THING_MODEL_TOPIC_OSD             = 7,
    THING_MODEL_TOPIC_STATE           = 8,
    THING_MODEL_TOPIC_KIND_MAX        = 9,
} E_ThingModelTopicKind;

/** Common fields of a service/property reply. */
typedef struct {
    int32_t result;
    char    result_message[128];
} T_ThingModelCommonReply;

/** Runtime representation of a bytes field: a heap-allocated binary buffer + length.
 *  Generated code uses it for proto `bytes` fields (single) and `repeated bytes` (elements).
 *  Convention: the `data` pointer is malloc'd by the Decode function, and the business side calls the corresponding <Cap>_Free<Msg>() to release it;
 *  on the business-side Encode, it mallocs and fills len itself, and Encode does not touch these two fields. */
typedef struct {
    uint8_t *data;
    size_t   len;
} T_ThingModelBytes;

/** Async reply callback: status==0 means success, otherwise refer to THING_MODEL_RC_*. */
typedef void (*T_ThingModelAsyncReplyCb)(void *user, T_ThingModelReturnCode status, const T_ThingModelCommonReply *reply);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_TYPES_H */
