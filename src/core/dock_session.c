/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * dock_session implementation.
 ********************************************************************
 */
#include "edge_dock/dock_session.h"
#include "edge_dock/dock_id_provider.h"
#include "edge_dock/broker_discovery.h"
#include "edge_dock/auth.h"        /* default reconnect hooks: auth_report_capability_dt5 / _dt7 */
#include "dock_app_info.h"

#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client/thing_model_client_transport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* ---- Reconnect hook slots -------------------------------------------------------------------
 * The runtime hands a connection callback nothing but cfg.user, so each session needs somewhere to
 * park {session, hook}. A bounded TABLE rather than one static: dt5 and dt7 are two distinct
 * devices (see dock_session.h) and an integrator may legitimately run both at once — a singleton
 * would make one session's reconnect publish the other device's capability set, silently.
 * Zero allocation: connect() claims a slot, disconnect() releases it by session pointer.
 *
 * PTHREAD_MUTEX_INITIALIZER rather than an OSAL mutex: static init, so there is no "who initialises
 * the lock" race of its own. dock_session_connect() already hardcodes ThingModelOsal_RegisterPosix(),
 * so pthread adds no portability constraint that isn't there already. */
#define DOCK_SESSION_MAX_SLOTS 4

typedef struct {
    ThingModelClientSession_t *session;   /* NULL until dock_session_connect() publishes it */
    int  (*hook)(ThingModelClientSession_t *);
    bool in_use;
    bool deferred;                        /* a reconnect fired before session was published */
} DockSessionSlot;

static DockSessionSlot s_slots[DOCK_SESSION_MAX_SLOTS];
static pthread_mutex_t s_slots_lock = PTHREAD_MUTEX_INITIALIZER;

/* Runs on the runtime's reconnect worker thread, right after session_resubscribe(): no session lock
 * is held there and ThingModelClientSession_Publish() takes none either, so publishing from here
 * cannot deadlock. Keep it short — this thread also owns the next drop. */
static void dock_session_reconnect_cb(void *user)
{
    DockSessionSlot *slot = (DockSessionSlot *)user;
    if (!slot) return;

    pthread_mutex_lock(&s_slots_lock);
    ThingModelClientSession_t *s = slot->session;
    int (*hook)(ThingModelClientSession_t *) = slot->hook;
    /* Connect() spawns the reconnect worker BEFORE it returns, so a drop can in principle land
     * before dock_session_connect() published the handle. Record the debt instead of dropping the
     * report on the floor — that would reproduce, in miniature, the very bug this hook fixes.
     * dock_session_connect() pays it as soon as it publishes. */
    if (!s) slot->deferred = true;
    pthread_mutex_unlock(&s_slots_lock);

    if (!s || !hook) return;
    fprintf(stderr, "[session] reconnected - re-reporting capability "
                    "(the gateway drops a device's registration together with its MQTT session)\n");
    (void)hook(s);
}

static DockSessionSlot *dock_session_slot_acquire(int (*hook)(ThingModelClientSession_t *))
{
    DockSessionSlot *slot = NULL;
    pthread_mutex_lock(&s_slots_lock);
    for (int i = 0; i < DOCK_SESSION_MAX_SLOTS; i++) {
        if (!s_slots[i].in_use) {
            slot = &s_slots[i];
            slot->in_use   = true;
            slot->session  = NULL;
            slot->hook     = hook;
            slot->deferred = false;
            break;
        }
    }
    pthread_mutex_unlock(&s_slots_lock);
    if (!slot) {
        fprintf(stderr, "[session] WARNING: more than %d live sessions — this one will NOT "
                        "re-report its capability after a reconnect\n", DOCK_SESSION_MAX_SLOTS);
    }
    return slot;
}

/* Publish the handle. Returns true if a reconnect already fired and still owes a report. */
static bool dock_session_slot_publish(DockSessionSlot *slot, ThingModelClientSession_t *s)
{
    if (!slot) return false;
    pthread_mutex_lock(&s_slots_lock);
    slot->session  = s;
    bool owed      = slot->deferred;
    slot->deferred = false;
    pthread_mutex_unlock(&s_slots_lock);
    return owed;
}

static void dock_session_slot_free(DockSessionSlot *slot)
{
    if (!slot) return;
    pthread_mutex_lock(&s_slots_lock);
    memset(slot, 0, sizeof(*slot));
    pthread_mutex_unlock(&s_slots_lock);
}

static void dock_session_slot_release(ThingModelClientSession_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s_slots_lock);
    for (int i = 0; i < DOCK_SESSION_MAX_SLOTS; i++) {
        if (s_slots[i].in_use && s_slots[i].session == s) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            break;
        }
    }
    pthread_mutex_unlock(&s_slots_lock);
}

void dock_session_default_opts(DockSessionOpts *opts)
{
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->app_id              = DOCK_APP_ID;
    opts->app_key             = DOCK_APP_KEY;
    opts->app_license         = DOCK_APP_LICENSE;
    opts->app_name            = DOCK_APP_NAME;
    /* The SN env override lives HERE, not in dock_session_connect(), so that an explicit
     * opts.device_sn assignment by integrator code still takes priority over the environment. */
    const char *env_sn        = getenv("DOCK_DEVICE_SN");
    opts->device_sn           = (env_sn && env_sn[0]) ? env_sn : DOCK_DEVICE_SN;
    opts->broker_ip           = NULL;              /* use discovery */
    opts->broker_port         = DOCK_BROKER_PORT;
    opts->use_discovery       = true;
    opts->discovery_timeout_ms = 15000;
    opts->fallback_ip         = DOCK_BROKER_FALLBACK_IP;
    opts->plaintext_access    = true;   /* ESDK 2.0 third-party devices default to plaintext access */
    opts->username_suffix     = NULL;
    /* Re-registration after a silent reconnect is ON by default: without it a reconnected dt5 sits
     * at status:2 and the gateway drops its sdr/network/charging replies with no error anywhere. */
    opts->on_reconnect        = auth_report_capability_dt5;
}

void dock_session_default_opts_rid(DockSessionOpts *opts)
{
    if (!opts) return;
    dock_session_default_opts(opts);
    /* dt7 is its own device: own SN (own account = own client_id = own topic owner) + "@rid". */
    const char *env_sn    = getenv("DOCK_RID_SN");
    opts->device_sn       = (env_sn && env_sn[0]) ? env_sn : DOCK_RID_DEVICE_SN;
    opts->username_suffix = "@rid";   /* the suffix RID/ADS-B telemetry requires, see the header */
    /* MUST override: dock_session_default_opts() above installed the dt5 hook, and re-declaring
     * device_type=5 on the RID account after a reconnect is worse than not reporting at all. */
    opts->on_reconnect    = auth_report_capability_dt7;
}

/* The account "<app_id>_esdk_<sn>" must fit in DOCK_ACCOUNT_MAX bytes; a longer one is truncated,
 * which makes the topic owner segment wrong and gets every publish rejected. Nothing surfaces at
 * connect time, and the dt7 "_rid" suffix costs 4 more bytes, so check it up front and warn — the
 * limit should be visible while configuring, not after deploying to hardware. */
#define DOCK_ACCOUNT_MAX 64
static void dock_session_warn_if_account_truncates(const DockSessionOpts *opts)
{
    const char *sn  = opts->device_sn ? opts->device_sn : "";
    const char *app = opts->app_id    ? opts->app_id    : "";
    size_t acc_len  = strlen(app) + 6 /* "_esdk_" */ + strlen(sn);
    if (strlen(sn) >= DOCK_ACCOUNT_MAX || acc_len >= DOCK_ACCOUNT_MAX) {
        fprintf(stderr,
                "[session] WARNING: account \"%s_esdk_%s\" is %zu bytes but the limit is %d — "
                "it will be truncated and the broker will reject every topic. "
                "Shorten DOCK_DEVICE_SN / DOCK_RID_SN.\n",
                app, sn, acc_len, DOCK_ACCOUNT_MAX);
    }
}

/* Resolve the broker address the same way for connect() and for resolve_credentials().
 * Note the env precedence differs from the SN overrides in dock_session_default_opts*(): these sit
 * on the connect path, so they also override an explicit opts assignment. */
static void dock_session_resolve_broker(const DockSessionOpts *opts,
                                        char *ip, size_t ip_cap, int *port)
{
    const char *env_ip   = getenv("DOCK_BROKER_IP");
    const char *env_port = getenv("DOCK_BROKER_PORT");

    *port = opts->broker_port;
    if (env_port && env_port[0]) {
        *port = atoi(env_port);
    }
    if (env_ip && env_ip[0]) {
        snprintf(ip, ip_cap, "%s", env_ip);
    } else if (opts->broker_ip && opts->broker_ip[0]) {
        snprintf(ip, ip_cap, "%s", opts->broker_ip);
    } else if (opts->use_discovery &&
               dock_discover_broker(ip, ip_cap, opts->discovery_timeout_ms)) {
        /* discovery succeeded, ip is filled */
    } else {
        snprintf(ip, ip_cap, "%s", opts->fallback_ip ? opts->fallback_ip : "127.0.0.1");
        fprintf(stderr, "[session] using fallback broker IP: %s\n", ip);
    }
}

/* Assemble the plaintext-access username suffix, honouring the environment override. Shared so a
 * resolver and a connect cannot disagree about which suffix a dt7 session actually uses. */
static const char *dock_session_effective_suffix(const DockSessionOpts *opts)
{
    const char *env_suffix = getenv("DOCK_USERNAME_SUFFIX");
    if (env_suffix && env_suffix[0]) {
        /* The environment overriding the "@rid" that dock_session_default_opts_rid() set would
         * mis-identify the dt7 session with no other symptom, so make the override visible. */
        if (opts->username_suffix && strcmp(env_suffix, opts->username_suffix) != 0) {
            fprintf(stderr, "[session] WARNING: DOCK_USERNAME_SUFFIX=\"%s\" overrides the configured "
                            "suffix \"%s\"\n", env_suffix, opts->username_suffix);
        }
        return env_suffix;
    }
    return opts->username_suffix;
}

int dock_session_resolve_credentials(const DockSessionOpts *opts, DockSessionCreds_t *out)
{
    if (!opts || !out) return THING_MODEL_RC_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const char *app_id  = opts->app_id      ? opts->app_id      : "";
    const char *app_key = opts->app_key     ? opts->app_key     : "";
    const char *lic     = opts->app_license ? opts->app_license : "";
    const char *sn      = opts->device_sn   ? opts->device_sn   : "";

    /* Mirrors the runtime's plaintext-access path: client_id is the account, the suffix goes on
     * the username only, and the password is the two credentials joined by an underscore. */
    snprintf(out->account,  sizeof(out->account),  "%s_esdk_%s", app_id, sn);
    const char *suffix = dock_session_effective_suffix(opts);
    snprintf(out->username, sizeof(out->username), "%s%s", out->account, suffix ? suffix : "");
    snprintf(out->password, sizeof(out->password), "%s_%s", app_key, lic);

    dock_session_resolve_broker(opts, out->broker_ip, sizeof(out->broker_ip), &out->broker_port);
    return THING_MODEL_RC_OK;
}

ThingModelClientSession_t *dock_session_connect(const DockSessionOpts *opts)
{
    DockSessionOpts local;
    if (!opts) {
        dock_session_default_opts(&local);
        opts = &local;
    }

    /* 1. OSAL + bid/tid provider */
    ThingModelOsal_RegisterPosix();
    dock_session_warn_if_account_truncates(opts);
    dock_id_provider_register(opts->device_sn);

    /* 2. Resolve broker address (env override takes priority, handy for pointing a build at a
     *    different broker without recompiling). */
    char broker_ip[64] = {0};
    int  broker_port   = opts->broker_port;
    dock_session_resolve_broker(opts, broker_ip, sizeof(broker_ip), &broker_port);

    /* 3. Assemble config */
    ThingModelClientSession_Config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.app_id         = opts->app_id;
    cfg.app_name       = opts->app_name;
    cfg.app_key        = opts->app_key;
    cfg.app_license    = opts->app_license;
    cfg.device_sn      = opts->device_sn;
    cfg.broker_address = broker_ip;
    cfg.broker_port    = (uint16_t)broker_port;
    cfg.keepalive_sec  = 30;
    cfg.default_timeout_ms = 5000;
    cfg.topic_style    = THING_MODEL_TOPIC_STYLE_V2;   /* production default */
    cfg.auto_reconnect = true;
    cfg.reconnect_delay_min_sec = 1;
    cfg.reconnect_delay_max_sec = 30;

    /* ESDK 2.0 plaintext access + RID @rid suffix (env DOCK_USERNAME_SUFFIX can override). */
    cfg.plaintext_access = opts->plaintext_access;
    cfg.username_suffix  = dock_session_effective_suffix(opts);

    /* Optionally inject a custom transport (CI loopback mock). */
    if (opts->transport_vtable) {
        cfg.transport_vtable = (const T_ThingModelTransportVtable *)opts->transport_vtable;
    }

    /* Register the reconnect hook. on_reconnect, not on_connect: the runtime fires on_connect for
     * the FIRST connect too, from inside Connect() before it hands back a handle the hook could
     * publish on. on_reconnect fires only on the path we care about. */
    DockSessionSlot *slot = opts->on_reconnect ? dock_session_slot_acquire(opts->on_reconnect) : NULL;
    if (slot) {
        cfg.on_reconnect = dock_session_reconnect_cb;
        cfg.user         = slot;
    }

    fprintf(stderr, "[session] connecting %s:%d (sn=%s) ...\n",
            broker_ip, broker_port, opts->device_sn);

    ThingModelClientSession_t *s = ThingModelClientSession_Connect(&cfg);
    if (!s) {
        const T_ThingModelLastError *e = ThingModelClientSession_GetLastError();
        fprintf(stderr, "[session] connection failed: rc=%d transport_rc=%d %s\n",
                e ? e->rc : -1, e ? e->transport_rc : 0, e ? e->message : "");
        dock_session_slot_free(slot);
        dock_id_provider_unregister();
        return NULL;
    }
    fprintf(stderr, "[session] connected\n");
    if (dock_session_slot_publish(slot, s)) {
        /* A drop landed in the window between Connect() spawning the reconnect worker and this
         * line, so the worker deferred its report. Pay it now, from this thread. */
        fprintf(stderr, "[session] reconnected during connect - re-reporting capability\n");
        (void)opts->on_reconnect(s);
    }
    return s;
}

void dock_session_disconnect(ThingModelClientSession_t *session)
{
    /* Release the hook slot first so no reconnect can pick the handle up again. This cannot strand
     * a hook mid-publish: Disconnect() joins the reconnect worker before it touches the transport,
     * so a hook already running completes on a still-live session. */
    dock_session_slot_release(session);
    if (session) {
        ThingModelClientSession_Disconnect(session);
    }
    dock_id_provider_unregister();
}
