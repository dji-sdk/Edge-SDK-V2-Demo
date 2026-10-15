/**
 * @file    thing_model_client_transport_lwip.c
 * @brief   lwIP MQTT (+ altcp_tls / mbedTLS) implementation of T_ThingModelTransportVtable.
 *
 *   Platform: FreeRTOS + lwIP-2.1.2 + mbedTLS 2.28 (embedded, e.g. GD32F527).
 *   Standalone cmake target: thing_model_client_transport_lwip.
 *
 * ──────────────────────────────────────────────────────────────────
 * Design highlights
 * ──────────────────────────────────────────────────────────────────
 *
 * 1. Two-phase authentication driven by the session layer
 *    Phase 1: session calls m_connect(use_tls=true, no HMAC)
 *            → this file: altcp_tls opens a temporary TLS connection, EKM is captured into s_ekm by export_keys_cb
 *            → session calls export_keying_material(): derives 32B with mbedtls_ssl_tls_prf
 *            → session calls m_disconnect()
 *    Phase 2: session calls m_connect(use_tls=false, hmac_enabled=true)
 *            → this file: plain TCP connect to 1883, token used as username
 *            → each m_publish automatically appends [ts_be8][hmac32]
 *
 * 2. lwIP NO_SYS=0 + LWIP_TCPIP_CORE_LOCKING=1 concurrency model
 *    The sample project lwipopts.h sets NO_SYS=0 + LWIP_TCPIP_CORE_LOCKING=1, lwIP starts one
 *    tcpip_thread, all lwIP raw APIs must be called while holding the core lock.
 *
 *    This transport's public interface (m_publish/m_subscribe/m_disconnect etc.) **can be called from any
 *    application thread** — internally it LOCK_TCPIP_CORE() before calling mqtt_*, and UNLOCK_TCPIP_CORE() after.
 *    With LWIP_TCPIP_CORE_LOCKING=1, sys_lock_tcpip_core is a recursive mutex, so nested locking on the same thread
 *    is safe (calling m_publish inside a lwIP callback does not deadlock).
 *
 *    The public interface is thread-safe: application threads may call publish concurrently from outside.
 *
 * 3. EKM capture — mbedTLS 2.28 uses mbedtls_ssl_conf_export_keys_ext_cb
 *    Requires enabling MBEDTLS_SSL_EXPORT_KEYS in mbedtls_config.
 *    The sample's mbedtls_config_gd32.h already enables it.
 *    The cb is invoked when the TLS handshake completes, storing master_secret + client_random + server_random
 *    into s_ekm; export_keying_material() later derives with mbedtls_ssl_tls_prf.
 *
 * 4. Auto reconnect
 *    The lwIP MQTT client has no built-in auto reconnect. When cfg->auto_reconnect is enabled, this file
 *    uses a FreeRTOS sw timer on disconnect_cb (rc != ACCEPTED) to re-run mqtt_client_connect after a delay.
 *    Backoff doubles exponentially between min..max.
 *
 * 5. Inbound message flow
 *    lwIP MQTT's inpub callback comes in two steps: publish_cb (gives the topic) → data_cb (may deliver the
 *    payload in multiple fragments, the last one carrying MQTT_DATA_FLAG_LAST). Here we assemble the full payload
 *    then callback the upper-layer msg_cb. The callback is invoked on tcpip_thread; calling m_publish inside the
 *    user callback is safe (recursive core lock).
 *
 * 6. Binary payload support
 *    After HMAC signing, the Phase 2 publish payload is binary, not ASCII.
 *    Inbound messages may also be binary (e.g. some broker diagnostic topics).
 *    The payload buf is not treated as a string; its length is determined by publish_cb's tot_len.
 *
 * ──────────────────────────────────────────────────────────────────
 * Credential passing format — aligned with session.c::ThingModelClientSession_Connect
 * ──────────────────────────────────────────────────────────────────
 *   Phase 1: username = "<app_id>_psdk_<sn>"  or "<app_id>:<app_name>_psdk_<sn>"
 *           password = "<app_key>_<app_license>_<timestamp_ms>"
 *           ci.tls_config = altcp_tls_create_config_client(ca_cert, ca_cert_len)
 *
 *   Phase 2: username = token (64-byte hex, computed by session using EKM)
 *           password = "" (broker validates via HMAC packet-level signature, does not check password)
 *           ci.tls_config = NULL (uses altcp_tcp)
 *
 * ──────────────────────────────────────────────────────────────────
 * CA cert source
 * ──────────────────────────────────────────────────────────────────
 *   cfg->ca_cert_path:
 *     - "embedded"  → use the weak-referenced s_ca_cert_pem[] in this file (provided by GD32 sample)
 *     - other strings → not supported yet (no FS on embedded), returns error
 */

#include "thing_model_client/thing_model_client_transport.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client_log.h"

#include "lwip/apps/mqtt.h"
#include "lwip/altcp_tls.h"
#include "lwip/ip_addr.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/tcpip.h"          /* LOCK_TCPIP_CORE / UNLOCK_TCPIP_CORE */

#include "altcp_tls_mbedtls_structs.h"   /* obtain mbedtls_ssl_config */
#include "mbedtls/ssl.h"
#include "mbedtls/md.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ============================================================
 * Compile-time tunable parameters
 * ============================================================ */
#ifndef TM_LWIP_MAX_TOPIC_BYTES
#define TM_LWIP_MAX_TOPIC_BYTES      128U
#endif
#ifndef TM_LWIP_MAX_PAYLOAD_BYTES
/** 1KB JSON + 40B HMAC signature (SA_SIGNATURE_TOTAL_LEN) */
#define TM_LWIP_MAX_PAYLOAD_BYTES    (4096U + 40U)
#endif
#ifndef TM_LWIP_CONNACK_TIMEOUT_MS
#define TM_LWIP_CONNACK_TIMEOUT_MS   15000U
#endif

/* HMAC packet-level signature header length — aligned with transport_mosq.c */
#define TM_LWIP_HMAC_SIG_LEN         (8U + 32U)

/* ============================================================
 * CA cert embedding — provided by the sample project; runtime weak default is NULL
 * ============================================================ */
__attribute__((weak)) const unsigned char *thing_model_lwip_ca_cert(void) { return NULL; }
__attribute__((weak)) unsigned int thing_model_lwip_ca_cert_len(void) { return 0; }

/* ============================================================
 * EKM context — held per connection (this transport has only one active connection at a time)
 * ============================================================ */
typedef struct {
    unsigned char         master_secret[48];
    unsigned char         randbytes[64];    /* client_random[32] || server_random[32] */
    mbedtls_tls_prf_types tls_prf;
    int                   captured;
} ekm_ctx_t;

static ekm_ctx_t s_ekm;

static int export_keys_cb(void *p_expkey,
                          const unsigned char *ms,
                          const unsigned char *kb,
                          size_t maclen, size_t keylen, size_t ivlen,
                          const unsigned char client_random[32],
                          const unsigned char server_random[32],
                          mbedtls_tls_prf_types tls_prf_type)
{
    (void)kb; (void)maclen; (void)keylen; (void)ivlen;
    ekm_ctx_t *ctx = (ekm_ctx_t *)p_expkey;
    memcpy(ctx->master_secret, ms, 48);
    /* The aircraft secure-access broker derives its session key with the
     * callback argument order used by its existing mbedTLS client path. */
    memcpy(ctx->randbytes,      client_random, 32);
    memcpy(ctx->randbytes + 32, server_random, 32);
    ctx->tls_prf  = tls_prf_type;
    ctx->captured = 1;
    return 0;
}

/* ============================================================
 * Transport impl structure
 * ============================================================ */
struct T_ThingModelTransportImpl {
    /* lwIP MQTT */
    mqtt_client_t           *client;
    ip_addr_t                broker_ip;
    uint16_t                 broker_port;
    bool                     use_tls;
    bool                     auto_reconnect;
    uint16_t                 reconnect_min_sec;
    uint16_t                 reconnect_max_sec;
    uint16_t                 reconnect_curr_sec;   /* current backoff */
    TimerHandle_t            reconnect_timer;
    bool                     reconnect_armed;

    /* TLS config (held by this transport, freed on destroy) */
    struct altcp_tls_config *tls_cfg;
    bool                     ekm_valid;

    /* Pending connection info — mqtt_client_connect requires the ci lifetime to cover the connect period */
    struct mqtt_connect_client_info_t conn_ci;
    char                     ci_client_id[64];
    char                     ci_username [128];
    char                     ci_password [512];

    /* CONNACK sync event */
    void                    *connect_event;
    int                      connect_rc;       /* mqtt_connection_status_t numeric value */
    bool                     connected;

    /* HMAC */
    bool                     hmac_enabled;
    uint8_t                  hmac_key[32];
    const T_ThingModelCryptoVtable *crypto;

    /* Upper-layer callbacks */
    T_ThingModelTransportMsgCb   msg_cb;
    void                        *msg_user;
    T_ThingModelTransportConnCb  conn_cb;
    void                        *conn_user;
    uint32_t                     tx_publish_count, tx_publish_bytes, tx_publish_errors;
    uint32_t                     rx_publish_count, rx_publish_bytes, disconnect_count;
    uint32_t                     last_tx_ms, last_rx_ms;

    /* Inbound message assembly buffer (single client serial receive, all invoked on tcpip_thread) */
    char                     in_topic  [TM_LWIP_MAX_TOPIC_BYTES];
    uint8_t                  in_payload[TM_LWIP_MAX_PAYLOAD_BYTES];
    uint16_t                 in_payload_len;
    uint32_t                 in_tot_len;        /* total length passed by publish_cb */

    /* Subscriptions — re-subscribed after auto reconnect */
    struct {
        char    topic[TM_LWIP_MAX_TOPIC_BYTES];
        uint8_t qos;
        bool    used;
    } subs[16];
};

static T_ThingModelTransportImpl_t *s_last_transport;

/* ============================================================
 * lwIP MQTT callbacks — invoked by tcpip_thread, core lock already held internally
 * ============================================================ */
static void in_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)arg;
    size_t n = strlen(topic);
    if (n >= TM_LWIP_MAX_TOPIC_BYTES) n = TM_LWIP_MAX_TOPIC_BYTES - 1;
    memcpy(t->in_topic, topic, n);
    t->in_topic[n] = '\0';
    t->in_payload_len = 0;
    t->in_tot_len     = tot_len;
}

static void in_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)arg;
    if (data && len) {
        if (t->in_payload_len + len > TM_LWIP_MAX_PAYLOAD_BYTES) {
            LOG_ERROR("[lwip-trans] inbound payload overflow: need=%u buf=%u",
                      (unsigned)(t->in_payload_len + len),
                      (unsigned)TM_LWIP_MAX_PAYLOAD_BYTES);
            t->in_payload_len = 0;
            return;
        }
        memcpy(t->in_payload + t->in_payload_len, data, len);
        t->in_payload_len += len;
    }
    if (flags & MQTT_DATA_FLAG_LAST) {
        t->rx_publish_count++;
        t->rx_publish_bytes += t->in_payload_len;
        t->last_rx_ms = sys_now();
        if (t->msg_cb) {
            t->msg_cb(t->msg_user, t->in_topic, t->in_payload, t->in_payload_len);
        }
        t->in_payload_len = 0;
    }
}

static void pub_done_cb(void *arg, err_t result)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)arg;
    if (result != ERR_OK) {
        if (t) t->tx_publish_errors++;
        LOG_ERROR("[lwip-trans] mqtt_publish completion err=%d", (int)result);
    }
}

static void sub_done_cb(void *arg, err_t result)
{
    (void)arg;
    if (result != ERR_OK) {
        LOG_ERROR("[lwip-trans] mqtt_subscribe completion err=%d", (int)result);
    }
}

/* Forward decl */
static void schedule_reconnect(T_ThingModelTransportImpl_t *t);
static void connection_cb(mqtt_client_t *client, void *arg,
                          mqtt_connection_status_t status);

static void mqtt_connect_in_tcpip(void *arg)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)arg;
    err_t err;

    if (t->client) mqtt_disconnect(t->client);
    err = mqtt_client_connect(t->client, &t->broker_ip, t->broker_port,
                              connection_cb, t, &t->conn_ci);
    if (err != ERR_OK && err != ERR_INPROGRESS) {
        LOG_ERROR("[lwip-trans] mqtt_client_connect err=%d", (int)err);
        t->connect_rc = (int)err;
        ThingModelOsal_Get()->event_signal(t->connect_event);
    }
}

static void connection_cb(mqtt_client_t *client, void *arg,
                          mqtt_connection_status_t status)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)arg;
    t->connect_rc = (int)status;
    if (status == MQTT_CONNECT_ACCEPTED) {
        LOG_INFO("[lwip-trans] CONNACK accepted");
        t->connected = true;
        t->last_tx_ms = t->last_rx_ms = sys_now();  /* reset age for new connection */
        t->reconnect_curr_sec = t->reconnect_min_sec;  /* reset backoff */
        /* This cb is invoked by tcpip_thread, core lock already held — calling lwIP API directly is safe */
        mqtt_set_inpub_callback(client, in_publish_cb, in_data_cb, t);
        /* Re-subscribe (if Phase 2 reconnect) */
        for (unsigned i = 0; i < sizeof(t->subs)/sizeof(t->subs[0]); i++) {
            if (t->subs[i].used) {
                err_t e = mqtt_subscribe(client, t->subs[i].topic,
                                          t->subs[i].qos, sub_done_cb, t);
                if (e != ERR_OK) {
                    LOG_ERROR("[lwip-trans] resub %s err=%d", t->subs[i].topic, (int)e);
                }
            }
        }
    } else {
        LOG_ERROR("[lwip-trans] CONNACK/disconnect status=%d", (int)status);
        t->connected = false;
        t->disconnect_count++;
        if (t->auto_reconnect && !t->use_tls) {
            /* Only Phase 2 auto reconnects, Phase 1 is one-shot */
            schedule_reconnect(t);
        }
    }
    /* Notify the session layer */
    if (t->conn_cb) {
        int rc = (status == MQTT_CONNECT_ACCEPTED) ? 0 : (int)status;
        t->conn_cb(t->conn_user, rc);
    }
    /* Wake up the m_connect synchronous wait */
    if (t->connect_event) {
        ThingModelOsal_Get()->event_signal(t->connect_event);
    }
}

/* ============================================================
 * Auto reconnect — FreeRTOS sw timer
 *
 * reconnect_timer_cb runs in the FreeRTOS timer service task context, **not** on
 * tcpip_thread, so it must LOCK_TCPIP_CORE before entering mqtt_client_connect.
 * ============================================================ */
static void reconnect_timer_cb(TimerHandle_t xt)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)pvTimerGetTimerID(xt);
    if (!t) return;
    t->reconnect_armed = false;
    if (!t->client) return;
    LOG_INFO("[lwip-trans] reconnect attempt (backoff was %us)", t->reconnect_curr_sec);
    err_t err = tcpip_callback(mqtt_connect_in_tcpip, t);
    if (err != ERR_OK) {
        LOG_ERROR("[lwip-trans] reconnect tcpip_callback err=%d", (int)err);
        schedule_reconnect(t);  /* schedule again on failure */
    }
}

static void schedule_reconnect(T_ThingModelTransportImpl_t *t)
{
    if (!t->auto_reconnect) return;
    if (t->reconnect_armed) return;
    /* Exponential backoff */
    if (t->reconnect_curr_sec == 0) t->reconnect_curr_sec = t->reconnect_min_sec;
    else {
        uint32_t doubled = (uint32_t)t->reconnect_curr_sec * 2;
        if (doubled > t->reconnect_max_sec) doubled = t->reconnect_max_sec;
        t->reconnect_curr_sec = (uint16_t)doubled;
    }
    if (!t->reconnect_timer) {
        t->reconnect_timer = xTimerCreate("tm_recon",
                                          pdMS_TO_TICKS(t->reconnect_curr_sec * 1000U),
                                          pdFALSE, t, reconnect_timer_cb);
    } else {
        xTimerChangePeriod(t->reconnect_timer,
                           pdMS_TO_TICKS(t->reconnect_curr_sec * 1000U), 0);
    }
    if (!t->reconnect_timer) {
        LOG_ERROR("[lwip-trans] reconnect timer create failed");
        return;
    }
    t->reconnect_armed = true;
    xTimerStart(t->reconnect_timer, 0);
    LOG_INFO("[lwip-trans] reconnect scheduled in %u s", t->reconnect_curr_sec);
}

/* ============================================================
 * vtable: create / destroy
 * ============================================================ */
static T_ThingModelTransportImpl_t *m_create(void)
{
    T_ThingModelTransportImpl_t *t = (T_ThingModelTransportImpl_t *)pvPortMalloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->reconnect_min_sec = 1;
    t->reconnect_max_sec = 30;
    t->reconnect_curr_sec = 0;
    s_last_transport = t;
    return t;
}

static void m_destroy(T_ThingModelTransportImpl_t *t)
{
    if (!t) return;
    if (s_last_transport == t) s_last_transport = NULL;
    if (t->reconnect_timer) {
        xTimerStop(t->reconnect_timer, 0);
        xTimerDelete(t->reconnect_timer, 0);
    }
    if (t->client) {
        LOCK_TCPIP_CORE();
        mqtt_disconnect(t->client);
        mqtt_client_free(t->client);
        UNLOCK_TCPIP_CORE();
    }
    if (t->tls_cfg) altcp_tls_free_config(t->tls_cfg);
    if (t->connect_event) ThingModelOsal_Get()->event_destroy(t->connect_event);
    vPortFree(t);
}

/* ============================================================
 * vtable: set_message_cb / set_conn_cb
 * ============================================================ */
static void m_set_message_cb(T_ThingModelTransportImpl_t *t, T_ThingModelTransportMsgCb cb, void *u)
{
    t->msg_cb   = cb;
    t->msg_user = u;
}
static void m_set_conn_cb(T_ThingModelTransportImpl_t *t, T_ThingModelTransportConnCb cb, void *u)
{
    t->conn_cb   = cb;
    t->conn_user = u;
}

/* ============================================================
 * vtable: connect
 *
 * Flow:
 *   1. Resolve broker_address to ip_addr_t
 *   2. If use_tls: use ca_cert (obtained from the thing_model_lwip_ca_cert weak function)
 *      to build altcp_tls_config, register the EKM cb
 *   3. Create a new mqtt_client (if none exists), assemble conn_ci, mqtt_client_connect (holding core lock)
 *   4. Synchronously wait for the CONNACK event, up to TM_LWIP_CONNACK_TIMEOUT_MS
 *
 * Note: when Phase 2 calls this function, t->client already exists (created in Phase 1), so it disconnects then connects.
 *    We let the mqtt_client_t be reused — free tls_cfg + reset ci.
 * ============================================================ */
static T_ThingModelReturnCode m_connect(T_ThingModelTransportImpl_t *t,
                                          const T_ThingModelTransportConfig *cfg)
{
    if (!t || !cfg) return THING_MODEL_RC_INVALID_ARG;
    if (!cfg->host || !cfg->client_id) return THING_MODEL_RC_INVALID_ARG;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (!o) return THING_MODEL_RC_INVALID_ARG;

    /* Free the previous round's tls_cfg (needed in Phase 2) */
    if (t->tls_cfg) {
        altcp_tls_free_config(t->tls_cfg);
        t->tls_cfg = NULL;
    }

    /* Resolve broker IP — only supports dotted-quad (embedded has no resolver) */
    if (!ipaddr_aton(cfg->host, &t->broker_ip)) {
        LOG_ERROR("[lwip-trans] ipaddr_aton failed: '%s' (dotted-quad required)", cfg->host);
        return THING_MODEL_RC_INVALID_ARG;
    }
    t->broker_port    = cfg->port;
    t->use_tls        = cfg->use_tls;
    t->auto_reconnect = cfg->auto_reconnect;
    if (cfg->reconnect_delay_min_sec) t->reconnect_min_sec = cfg->reconnect_delay_min_sec;
    if (cfg->reconnect_delay_max_sec) t->reconnect_max_sec = cfg->reconnect_delay_max_sec;
    t->reconnect_curr_sec = 0;

    /* HMAC */
    t->hmac_enabled = cfg->hmac_enabled;
    t->crypto       = cfg->crypto;
    if (cfg->hmac_enabled && cfg->hmac_key) {
        memcpy(t->hmac_key, cfg->hmac_key, 32);
    }

    /* TLS configuration */
    if (cfg->use_tls) {
        const unsigned char *ca     = thing_model_lwip_ca_cert();
        unsigned int         ca_len = thing_model_lwip_ca_cert_len();
        if (!ca || ca_len == 0) {
            LOG_ERROR("[lwip-trans] CA cert not provided — "
                      "the sample project must implement thing_model_lwip_ca_cert() / thing_model_lwip_ca_cert_len()");
            return THING_MODEL_RC_INVALID_ARG;
        }
        t->tls_cfg = altcp_tls_create_config_client(ca, ca_len);
        if (!t->tls_cfg) {
            LOG_ERROR("[lwip-trans] altcp_tls_create_config_client failed");
            return THING_MODEL_RC_AUTH_FAILED;
        }
        /* Register EKM capture — derived by export_keying_material() after Phase 1 */
        memset(&s_ekm, 0, sizeof(s_ekm));
        mbedtls_ssl_conf_export_keys_ext_cb(
            (mbedtls_ssl_config *)t->tls_cfg,
            export_keys_cb, &s_ekm);
        t->ekm_valid = false;
    }

    /* Create client (or reuse) — mqtt_client_new internally calls memp_malloc, must hold core lock */
    if (!t->client) {
        LOCK_TCPIP_CORE();
        t->client = mqtt_client_new();
        UNLOCK_TCPIP_CORE();
        if (!t->client) {
            LOG_ERROR("[lwip-trans] mqtt_client_new failed");
            return THING_MODEL_RC_OUT_OF_MEMORY;
        }
    }

    /* Prepare ci (must have persistent storage duration — copy into t, until disconnect) */
    memset(&t->conn_ci, 0, sizeof(t->conn_ci));
    snprintf(t->ci_client_id, sizeof(t->ci_client_id), "%s", cfg->client_id);
    t->conn_ci.client_id = t->ci_client_id;
    if (cfg->username) {
        snprintf(t->ci_username, sizeof(t->ci_username), "%s", cfg->username);
        t->conn_ci.client_user = t->ci_username;
    }
    if (cfg->password) {
        snprintf(t->ci_password, sizeof(t->ci_password), "%s", cfg->password);
        t->conn_ci.client_pass = t->ci_password;
    } else {
        t->conn_ci.client_pass = NULL;
    }
    t->conn_ci.keep_alive = cfg->keepalive_sec ? cfg->keepalive_sec : 60;
    t->conn_ci.tls_config = t->tls_cfg;   /* NULL = altcp_tcp */

    /* Event used to wait for CONNACK */
    if (!t->connect_event) t->connect_event = o->event_create();
    t->connect_rc = -1;
    t->connected  = false;

    LOG_INFO("[lwip-trans] mqtt_client_connect %s:%u (tls=%d)",
             cfg->host, cfg->port, cfg->use_tls ? 1 : 0);
    err_t err = tcpip_callback(mqtt_connect_in_tcpip, t);
    if (err != ERR_OK) {
        LOG_ERROR("[lwip-trans] tcpip_callback err=%d", (int)err);
        return THING_MODEL_RC_TRANSPORT_ERROR;
    }

    /* Synchronously wait for CONNACK */
    int rc = o->event_wait(t->connect_event, TM_LWIP_CONNACK_TIMEOUT_MS);
    if (rc != THING_MODEL_RC_OK) {
        LOG_ERROR("[lwip-trans] CONNACK timeout %u ms", TM_LWIP_CONNACK_TIMEOUT_MS);
        return THING_MODEL_RC_TIMEOUT;
    }
    if (t->connect_rc != MQTT_CONNECT_ACCEPTED) {
        LOG_ERROR("[lwip-trans] CONNACK rejected rc=%d", t->connect_rc);
        return THING_MODEL_RC_AUTH_FAILED;
    }
    if (cfg->use_tls && !s_ekm.captured) {
        LOG_ERROR("[lwip-trans] EKM cb not triggered — MBEDTLS_SSL_EXPORT_KEYS may be disabled in the mbedTLS config");
        return THING_MODEL_RC_AUTH_FAILED;
    }
    t->ekm_valid = s_ekm.captured ? true : false;
    return THING_MODEL_RC_OK;
}

/* ============================================================
 * vtable: disconnect
 * ============================================================ */
static T_ThingModelReturnCode m_disconnect(T_ThingModelTransportImpl_t *t)
{
    if (!t) return THING_MODEL_RC_INVALID_ARG;
    /* Disable auto reconnect (an active disconnect should not reconnect) */
    bool save_auto = t->auto_reconnect;
    t->auto_reconnect = false;
    if (t->reconnect_timer) {
        xTimerStop(t->reconnect_timer, 0);
        t->reconnect_armed = false;
    }
    if (t->client) {
        LOCK_TCPIP_CORE();
        mqtt_disconnect(t->client);
        UNLOCK_TCPIP_CORE();
    }
    t->connected = false;
    /* When switching Phase 1 → Phase 2 the session actively calls disconnect, then calls
     * connect again to enter Phase 2, whose cfg carries auto_reconnect. Restore
     * save_auto here to avoid changing t's state (the next m_connect resets it). */
    t->auto_reconnect = save_auto;
    return THING_MODEL_RC_OK;
}

/* ============================================================
 * HMAC signature — [payload][ts_be8][hmac32]
 * ============================================================ */
static int sign_payload(T_ThingModelTransportImpl_t *t,
                        const uint8_t *in, size_t in_len, uint64_t ts_ms,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (in_len + TM_LWIP_HMAC_SIG_LEN > out_cap) return -1;
    if (!t->crypto || !t->crypto->hmac_sha256) return -1;
    memcpy(out, in, in_len);
    for (int i = 7; i >= 0; i--) {
        out[in_len + i] = (uint8_t)(ts_ms & 0xFF);
        ts_ms >>= 8;
    }
    if (t->crypto->hmac_sha256(t->hmac_key, 32,
                                out, in_len + 8,
                                out + in_len + 8) != 0) {
        return -1;
    }
    *out_len = in_len + TM_LWIP_HMAC_SIG_LEN;
    return 0;
}

/* ============================================================
 * vtable: publish — calls lwIP directly, callable from any application thread
 *
 * Flow:
 *   1. Capacity check (HMAC mode must reserve 40B)
 *   2. Payload preparation: when HMAC enabled, allocate the signing buffer from the OSAL heap
 *   3. LOCK_TCPIP_CORE -> mqtt_publish -> UNLOCK_TCPIP_CORE
 *
 * The HMAC buffer uses the OSAL heap to avoid a large payload overflowing the FreeRTOS application task stack.
 *
 * Note: m_publish returns synchronously — once mqtt_publish has enqueued into lwIP's internal ringbuf it returns ERR_OK immediately,
 *     the actual network send is completed asynchronously by tcpip_thread, and pub_done_cb reports the result.
 *     The return value here reflects "queued successfully" rather than "send completed", consistent with transport_mosq.c semantics.
 * ============================================================ */
static T_ThingModelReturnCode m_publish(T_ThingModelTransportImpl_t *t, const char *topic,
                                         const void *payload, size_t len, int qos, bool retain)
{
    if (!t || !topic || !t->client) return THING_MODEL_RC_INVALID_ARG;
    size_t tlen = strlen(topic);
    if (tlen >= TM_LWIP_MAX_TOPIC_BYTES) return THING_MODEL_RC_INVALID_ARG;

    /* Capacity check — HMAC mode must reserve 40B */
    size_t need = len + (t->hmac_enabled ? TM_LWIP_HMAC_SIG_LEN : 0);
    if (need > TM_LWIP_MAX_PAYLOAD_BYTES) {
        LOG_ERROR("[lwip-trans] payload too large %u > %u",
                  (unsigned)need, (unsigned)TM_LWIP_MAX_PAYLOAD_BYTES);
        return THING_MODEL_RC_INVALID_ARG;
    }

    uint8_t *heap_buf = NULL;
    const void *send_buf = payload;
    size_t      send_len = len;

    if (t->hmac_enabled && payload && len > 0) {
        heap_buf = ThingModelMem_Alloc(TM_LWIP_MAX_PAYLOAD_BYTES);
        if (!heap_buf) return THING_MODEL_RC_OUT_OF_MEMORY;
        size_t out_len = 0;
        uint64_t ts = ThingModelOsal_Get()->time_ms();
        if (sign_payload(t, (const uint8_t *)payload, len, ts,
                          heap_buf, TM_LWIP_MAX_PAYLOAD_BYTES, &out_len) != 0) {
            LOG_ERROR("[lwip-trans] HMAC sign failed");
            ThingModelMem_Free(heap_buf);
            return THING_MODEL_RC_UNKNOWN;
        }
        send_buf = heap_buf;
        send_len = out_len;
    }

    if (!t->connected) {
        /* Not connected, reject directly, let the upper layer decide whether to retry/drop.
         * (After auto-reconnect succeeds, the application thread's next publish will naturally go out.) */
        ThingModelMem_Free(heap_buf);
        return THING_MODEL_RC_NOT_CONNECTED;
    }

    LOCK_TCPIP_CORE();
    err_t err = mqtt_publish(t->client, topic, send_buf, send_len,
                              (uint8_t)qos, retain ? 1 : 0, pub_done_cb, NULL);
    UNLOCK_TCPIP_CORE();

    if (err != ERR_OK) {
        /* Mostly ERR_MEM — pbuf/ringbuf not enough; let the upper layer decide the retry strategy */
        t->tx_publish_errors++;
        LOG_ERROR("[lwip-trans] mqtt_publish err=%d", (int)err);
        ThingModelMem_Free(heap_buf);
        return THING_MODEL_RC_TRANSPORT_ERROR;
    }
    t->tx_publish_count++;
    t->tx_publish_bytes += (uint32_t)send_len;
    t->last_tx_ms = sys_now();
    ThingModelMem_Free(heap_buf);
    return THING_MODEL_RC_OK;
}

/* ============================================================
 * vtable: subscribe / unsubscribe
 *
 * NO_SYS=0 + CORE_LOCKING=1: it is safe for an application thread to hold the core lock
 * then call lwIP mqtt_subscribe. lwIP's internal sub_done_cb is called back on tcpip_thread,
 * notified via sub_done_cb (here it only logs, does not block).
 * ============================================================ */
static void remember_sub(T_ThingModelTransportImpl_t *t, const char *filter, uint8_t qos)
{
    for (unsigned i = 0; i < sizeof(t->subs)/sizeof(t->subs[0]); i++) {
        if (t->subs[i].used && strcmp(t->subs[i].topic, filter) == 0) {
            t->subs[i].qos = qos;
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(t->subs)/sizeof(t->subs[0]); i++) {
        if (!t->subs[i].used) {
            snprintf(t->subs[i].topic, sizeof(t->subs[i].topic), "%s", filter);
            t->subs[i].qos = qos;
            t->subs[i].used = true;
            return;
        }
    }
    LOG_ERROR("[lwip-trans] subs table full, %s cannot be remembered → lost after reconnect", filter);
}

static void forget_sub(T_ThingModelTransportImpl_t *t, const char *filter)
{
    for (unsigned i = 0; i < sizeof(t->subs)/sizeof(t->subs[0]); i++) {
        if (t->subs[i].used && strcmp(t->subs[i].topic, filter) == 0) {
            t->subs[i].used = false;
            return;
        }
    }
}

static T_ThingModelReturnCode m_subscribe(T_ThingModelTransportImpl_t *t, const char *filter, int qos)
{
    if (!t || !filter || !t->client) return THING_MODEL_RC_INVALID_ARG;
    if (!t->connected) return THING_MODEL_RC_NOT_CONNECTED;
    remember_sub(t, filter, (uint8_t)qos);
    LOCK_TCPIP_CORE();
    err_t err = mqtt_subscribe(t->client, filter, (uint8_t)qos, sub_done_cb, t);
    UNLOCK_TCPIP_CORE();
    return (err == ERR_OK) ? THING_MODEL_RC_OK : THING_MODEL_RC_TRANSPORT_ERROR;
}

static T_ThingModelReturnCode m_unsubscribe(T_ThingModelTransportImpl_t *t, const char *filter)
{
    if (!t || !filter || !t->client) return THING_MODEL_RC_INVALID_ARG;
    forget_sub(t, filter);
    if (!t->connected) return THING_MODEL_RC_OK;   /* if not connected, forget is enough */
    LOCK_TCPIP_CORE();
    err_t err = mqtt_unsubscribe(t->client, filter, sub_done_cb, t);
    UNLOCK_TCPIP_CORE();
    return (err == ERR_OK) ? THING_MODEL_RC_OK : THING_MODEL_RC_TRANSPORT_ERROR;
}

/* ============================================================
 * vtable: export_keying_material
 *   Run the TLS-PRF using the master_secret + randomness captured in Phase 1.
 *   label = "MQTT_SECURE_ACCESS_V1" (aligned with the broker-side secure_access_common.h)
 * ============================================================ */
static T_ThingModelReturnCode m_export_keying_material(T_ThingModelTransportImpl_t *t, uint8_t out[32])
{
    if (!t || !out) return THING_MODEL_RC_INVALID_ARG;
    if (!t->ekm_valid || !s_ekm.captured) {
        LOG_ERROR("[lwip-trans] EKM not captured — must be called right after the Phase 1 connect");
        return THING_MODEL_RC_AUTH_FAILED;
    }
    static const char SA_LABEL[] = "MQTT_SECURE_ACCESS_V1";
    int r = mbedtls_ssl_tls_prf(s_ekm.tls_prf,
                                s_ekm.master_secret, 48,
                                SA_LABEL,
                                s_ekm.randbytes, 64,
                                out, 32);
    if (r != 0) {
        LOG_ERROR("[lwip-trans] mbedtls_ssl_tls_prf failed: %d", r);
        return THING_MODEL_RC_AUTH_FAILED;
    }
    return THING_MODEL_RC_OK;
}

/* ============================================================
 * vtable instance + factory
 * ============================================================ */
static const T_ThingModelTransportVtable LWIP_VT = {
    .create                  = m_create,
    .destroy                 = m_destroy,
    .set_message_cb          = m_set_message_cb,
    .set_conn_cb             = m_set_conn_cb,
    .connect                 = m_connect,
    .disconnect              = m_disconnect,
    .publish                 = m_publish,
    .subscribe               = m_subscribe,
    .unsubscribe             = m_unsubscribe,
    .export_keying_material  = m_export_keying_material,
};

const T_ThingModelTransportVtable *ThingModelTransport_LwipMbedtls(void)
{
    return &LWIP_VT;
}
