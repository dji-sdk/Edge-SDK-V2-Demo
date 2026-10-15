/**
 * @file    thing_model_client_internal.h
 * @brief   Session internal struct + listener linked list (not exported).
 */
#ifndef THING_MODEL_CLIENT_INTERNAL_H
#define THING_MODEL_CLIENT_INTERNAL_H

#include "thing_model_client/thing_model_client_session.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client/thing_model_client_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Centralized buffer size definitions — overridable at compile time
 * (cmake -DTHING_MODEL_DEVICE_SN_LEN=128 ...)
 * On snprintf truncation, LOG_WARN is emitted instead of failing silently.
 * ============================================================ */
#ifndef THING_MODEL_DEVICE_SN_LEN
#define THING_MODEL_DEVICE_SN_LEN       64
#endif
#ifndef THING_MODEL_BROKER_ADDR_LEN
#define THING_MODEL_BROKER_ADDR_LEN     128
#endif
#ifndef THING_MODEL_TOPIC_LEN
#define THING_MODEL_TOPIC_LEN           256
#endif
#ifndef THING_MODEL_USERNAME_LEN
#define THING_MODEL_USERNAME_LEN        256
#endif
#ifndef THING_MODEL_PASSWORD_LEN
#define THING_MODEL_PASSWORD_LEN        512
#endif

/* Truncation-check helper: emit WARN when snprintf returns >= cap */
#define THING_MODEL_CHECK_TRUNC(rc, cap, what)                          \
    do { if ((rc) >= (int)(cap)) {                                      \
        LOG_WARN("%s truncated: needed %d bytes, buffer %zu",           \
                 (what), (rc), (size_t)(cap));                          \
    } } while (0)

typedef struct ListenerEntry {
    struct ListenerEntry *next;
    int32_t            handle;
    T_ThingModelSessionMsgCb  cb;
    void              *user;
} ListenerEntry_t;

/* Subscription persistence: appended on Subscribe, removed on Unsubscribe;
 * in the auto-reconnect scenario, when on_connect fires on a reconnect (not the first connection),
 * the session walks this list and re-sends the SUBSCRIBE packets, ensuring EVENT/OSD/STATE
 * pushes can still be restored after a network drop. */
typedef struct SubscriptionEntry {
    struct SubscriptionEntry *next;
    char     topic[THING_MODEL_TOPIC_LEN];
    int      qos;
} SubscriptionEntry_t;

struct ThingModelClientSession {
    char     device_sn[THING_MODEL_DEVICE_SN_LEN];
    char     broker_addr[THING_MODEL_BROKER_ADDR_LEN];
    uint16_t broker_port;
    uint16_t keepalive_sec;
    uint32_t default_timeout_ms;
    E_ThingModelTopicStyle topic_style;
    void (*on_disconnect)(void *u, int reason);
    void (*on_connect)(void *u, bool is_reconnect);
    void (*on_reconnect)(void *u);
    void *on_disc_user;

    /* Reconnect config (passed in from cfg, forwarded to transport) */
    bool     auto_reconnect;
    uint16_t reconnect_delay_min_sec;
    uint16_t reconnect_delay_max_sec;

    uint8_t  hmac_key[32];

    const T_ThingModelTransportVtable *trans_vt;
    T_ThingModelTransportImpl_t       *trans;
    const T_ThingModelCryptoVtable    *crypto_vt;

    uint32_t  next_seq;
    void     *seq_lock;

    ListenerEntry_t *listeners;
    void            *listener_lock;
    int32_t          next_listener_handle;

    /* Subscription persistence — when auto_reconnect=true, on_connect walks this list to re-subscribe after a reconnect */
    SubscriptionEntry_t *subs;
    void                *subs_lock;
    bool                 phase2_connected_once;   /* false = first connection; subsequent on_connect is treated as a "reconnect" */

    /* ============================================================
     * Persisted access material — needed so the reconnect worker can re-run the FULL two-phase
     * authentication (fresh Phase-1 TLS → fresh RFC5705 keying material → fresh Phase-2 token),
     * rather than replaying a stale token. cfg strings are caller-owned and may not outlive the
     * session, so we keep heap copies here (freed in Disconnect).
     * ============================================================ */
    char    *app_id;
    char    *app_name;
    char    *app_key;
    char    *app_license;
    char    *ca_cert_path;
    uint16_t broker_port_tls;
    char     orig_device_sn[THING_MODEL_DEVICE_SN_LEN];  /* unmutated SN for auth identity (device_sn may be rewritten to the account owner under the dock build profile) */
#if THING_MODEL_PROFILE_DOCK
    bool     plaintext_access;
    char    *username_suffix;
#endif

    /* ============================================================
     * Session-layer reconnect state: a full two-phase re-auth is performed on an unexpected drop
     * (a stale Phase-2 token would be rejected by the broker after a client-id collision, so
     * re-auth is required). Driven by a dedicated worker thread; the transport
     * conn callback merely detects the drop and wakes it.
     * ============================================================ */
    volatile bool connected;         /* steady-state connected; dedupes the drop callback storm */
    volatile bool auth_in_progress;  /* while true, swallow transport conn callbacks (Phase-1 setup/teardown + Phase-2 connect are internal plumbing) */
    volatile bool shutting_down;     /* set by Disconnect so the worker exits */
    volatile bool reconn_pending;    /* a drop occurred; the worker should re-auth */
    void    *reconn_wake;            /* binary event: wakes the worker (drop signal or shutdown) */
    void    *reconn_lock;            /* guards the reconn_pending set + wake */
    void    *reconn_thread;          /* worker handle; NULL when auto_reconnect is off */
};

/* thing_model_record_error is implemented in session.c, declared in log.h (so transport /
 * secure_access etc. can use it without including internal.h) */

#ifdef __cplusplus
}
#endif
#endif
