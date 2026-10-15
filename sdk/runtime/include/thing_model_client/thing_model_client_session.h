/**
 * @file    thing_model_client_session.h
 * @brief   Session entry point + core runtime API.
 *
 *   The runtime provides only:
 *     • Two-phase authentication + long-connection lifecycle
 *     • publish / subscribe / raw message listener fanout
 *     • OSAL + topic-style access (for use by generated code)
 */
#ifndef THING_MODEL_CLIENT_SESSION_H
#define THING_MODEL_CLIENT_SESSION_H

#include "thing_model_client_types.h"
#include "thing_model_client_osal.h"
#include "thing_model_client_transport.h"
#include "thing_model_client_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ThingModelClientSession ThingModelClientSession_t;

typedef struct {
    const char *app_id;
    const char *app_name;       /**< application name, e.g. "psdkappfortest"; falls back to the old username format when NULL/empty */
    const char *app_key;
    const char *app_license;
    const char *device_sn;

    const char *broker_address;
    uint16_t    broker_port_tls;
    uint16_t    broker_port;
    const char *ca_cert_path;

#if THING_MODEL_PROFILE_DOCK
    /** Plaintext access mode.
     *  true  = skip the two-phase TLS+HMAC, and do a single plaintext MQTT connection to broker_port (default 1883),
     *          username = app_id "_esdk_" device_sn (+ username_suffix),
     *          password = app_key "_" app_license. Aligned with third-party devices.
     *  false = (default) two-phase secure-access: Phase1 TLS keying export → Phase2 plaintext + HMAC. */
    bool        plaintext_access;
    /** plaintext-access username suffix, e.g. RID devices use "@rid" to raise the single-packet limit (default 2048B).
     *  Must match the device_type declared in the capability set. NULL/"" = no suffix. Only takes effect with plaintext_access. */
    const char *username_suffix;
#endif /* THING_MODEL_PROFILE_DOCK */

    uint16_t    keepalive_sec;
    uint32_t    default_timeout_ms;

    /** topic style, default V1 (backward compatible). See E_ThingModelTopicStyle. */
    E_ThingModelTopicStyle topic_style;

    /** Auto-reconnect (on disconnect, the libmosquitto background thread reconnects per the backoff strategy). Default false. */
    bool      auto_reconnect;
    /** Minimum reconnect backoff (seconds), default 1. */
    uint16_t  reconnect_delay_min_sec;
    /** Maximum reconnect backoff (seconds), default 30. Exponentially doubles from min up to max. */
    uint16_t  reconnect_delay_max_sec;

    /** Optional — inject a custom transport (MQTT) implementation; NULL = default mosquitto
     *  (requires linking the thing_model_client_transport_mosquitto target). */
    const T_ThingModelTransportVtable *transport_vtable;

    /** Optional — inject a custom crypto (HMAC) implementation; NULL = default openssl
     *  (requires linking the thing_model_client_crypto_openssl target). */
    const T_ThingModelCryptoVtable *crypto_vtable;

    void (*on_disconnect)(void *user, int reason);
    /** Callback after a connection (both first connect and reconnect) succeeds. is_reconnect=true means a reconnect.
     *  Calling thread: transport background thread (don't do long operations in it). May be NULL. */
    void (*on_connect)(void *user, bool is_reconnect);
    /** Callback after a reconnect succeeds (syntactic sugar equivalent to on_connect(user, true), for convenient scenario distinction).
     *  Whichever of on_connect and on_reconnect exists is called; if both exist, both are called on reconnect. */
    void (*on_reconnect)(void *user);
    void *user;
} ThingModelClientSession_Config_t;

/* ----- Error-code accessor ----- *
 * When an API returns < 0, you can obtain the detailed cause of the most recent failure (transport rc + errno
 * + short text), for reconciling with the broker log or writing monitoring metrics.
 * Note: thread-local storage — under concurrent multi-threaded calls, each thread gets its own last error. */
typedef struct {
    int32_t     rc;                     /* same as the RC returned by the API */
    int32_t     transport_rc;           /* mosquitto rc / 0 means not a transport error */
    int32_t     errno_copy;             /* copy of errno / 0 means none */
    char        message[128];           /* short description, e.g. "CONNACK timeout 10s" */
} T_ThingModelLastError;

/** Get the detailed info of the most recent failure. Returns rc=THING_MODEL_RC_OK if there was never a failure. */
const T_ThingModelLastError *ThingModelClientSession_GetLastError(void);

/* ----- Lifecycle ----- */
ThingModelClientSession_t *ThingModelClientSession_Connect(const ThingModelClientSession_Config_t *cfg);
void                ThingModelClientSession_Disconnect(ThingModelClientSession_t *session);

/* ----- Accessors ----- */
const char         *ThingModelClientSession_GetSn         (const ThingModelClientSession_t *s);
E_ThingModelTopicStyle     ThingModelClientSession_GetTopicStyle (const ThingModelClientSession_t *s);
const T_ThingModelOsal    *ThingModelClientSession_GetOsal       (const ThingModelClientSession_t *s);
uint32_t            ThingModelClientSession_NextSeq       (ThingModelClientSession_t *s);

/* ----- Wire ops ----- */
T_ThingModelReturnCode ThingModelClientSession_Publish    (ThingModelClientSession_t *s,
                                             const char *topic,
                                             const char *json_payload, int qos);
T_ThingModelReturnCode ThingModelClientSession_Subscribe  (ThingModelClientSession_t *s,
                                             const char *topic_filter, int qos);
T_ThingModelReturnCode ThingModelClientSession_Unsubscribe(ThingModelClientSession_t *s,
                                             const char *topic_filter);

/* ----- Raw message listener ----- *
 * Each capability client registers one listener; after the session receives a PUBLISH it fans out to all listeners,
 * each of which matches against its own pending / push table.
 */
typedef void (*T_ThingModelSessionMsgCb)(void *user, const char *topic,
                                  const char *payload, size_t len);
int32_t ThingModelClientSession_AddListener   (ThingModelClientSession_t *s,
                                        T_ThingModelSessionMsgCb cb, void *user);
void    ThingModelClientSession_RemoveListener(ThingModelClientSession_t *s, int32_t handle);

#ifdef __cplusplus
}
#endif
#endif /* THING_MODEL_CLIENT_SESSION_H */
