/**
 * @file    thing_model_client_session.c
 * @brief   Session lifecycle + publish/subscribe + listener fanout.
 *
 *   Apart from two-phase authentication, this file only does the minimal "message bus" function:
 *   on receiving a PUBLISH → invoke all registered listeners; the rest of the dispatch logic is handled by the client-side message handlers.
 */
#include "thing_model_client_internal.h"
#include "thing_model_client_secure_access.h"
#include "thing_model_client_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_TIMEOUT_MS 5000

/* ---------- GetLastError ---------- *
 * Thread-local storage; each thread maintains its own last error independently, no lock needed.
 * GCC/clang uses __thread, C11 uses _Thread_local — both are supported here. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local T_ThingModelLastError s_last_error;
#else
static __thread T_ThingModelLastError s_last_error;
#endif

void thing_model_record_error(int32_t rc, int32_t transport_rc, int32_t errno_copy,
                              const char *fmt, ...)
{
    s_last_error.rc           = rc;
    s_last_error.transport_rc = transport_rc;
    s_last_error.errno_copy   = errno_copy;
    if (fmt) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(s_last_error.message, sizeof(s_last_error.message), fmt, ap);
        va_end(ap);
    } else {
        s_last_error.message[0] = '\0';
    }
}

const T_ThingModelLastError *ThingModelClientSession_GetLastError(void)
{
    return &s_last_error;
}

/* ---------- transport callback trampolines ---------- */

static void session_on_message(void *user, const char *topic,
                               const void *payload, size_t len)
{
    ThingModelClientSession_t *s = user;
    const T_ThingModelOsal *o = ThingModelOsal_Get();

    /* At DEBUG level, print the wire JSON; long payloads are truncated to avoid flooding.
     * How the user enables it: export THING_MODEL_LOG_LEVEL=debug, or
     *                          ThingModelLog_SetLevel(THING_MODEL_LOG_DEBUG). */
    if (len <= 1024) {
        LOG_DEBUG("RCV topic=%s json=%.*s", topic, (int)len, (const char *)payload);
    } else {
        LOG_DEBUG("RCV topic=%s json=%.*s ... [%zu more bytes]",
                  topic, 1024, (const char *)payload, len - 1024);
    }
    /* wire capture (THING_MODEL_WIRE_LOG=1): full raw received text, not truncated. */
    ThingModelLog_Wire("←RCV", topic, payload, len);

    /* Both the listener traversal and cb invocation are done inside the lock.
     * Reason: copying the pointer out of the lock and then traversing would cause a use-after-free on a concurrent RemoveListener;
     * while the listener implementation only does short operations like posting to
     * the pending/push table, and never calls back into this listener_lock.
     * The callback the user passed to Camera_On<Event>(...) ultimately runs in the out-of-lock path of the push delivery,
     * and does not block here. */
    o->mutex_lock(s->listener_lock);
    for (ListenerEntry_t *e = s->listeners; e; e = e->next) {
        e->cb(e->user, topic, (const char *)payload, len);
    }
    o->mutex_unlock(s->listener_lock);
}

/* Walk the persisted subscription list and re-send the SUBSCRIBE packets.
 * SUBSCRIBE is not auto-retained by mosquitto, so after a reconnect we must replay them,
 * ensuring EVENT/OSD/STATE pushes continue once the connection is restored. */
static void session_resubscribe(ThingModelClientSession_t *s)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    o->mutex_lock(s->subs_lock);
    int n = 0;
    for (SubscriptionEntry_t *e = s->subs; e; e = e->next) {
        if (s->trans_vt->subscribe(s->trans, e->topic, e->qos) == THING_MODEL_RC_OK)
            n++;
    }
    o->mutex_unlock(s->subs_lock);
    LOG_INFO("[RECONNECT] re-subscribed %d topic(s) after reconnect", n);
}

/* Transport connection callback (runs on the transport loop thread).
 *
 * All the connect/disconnect *success* wiring (firing on_connect, flipping connected/
 * phase2_connected_once, re-subscribing) is owned by the auth driver — Connect() for the
 * initial connection and reconnect_worker() for a reconnect — NOT here. This callback only:
 *   • swallows the transport chatter emitted while an (re)auth handshake is in progress
 *     (Phase-1 TLS connect+teardown, Phase-2 connect, and any CONNACK-reject during the
 *     brief window before the worker stops libmosquitto's own loop); and
 *   • on a genuine steady-state drop, fires the user on_disconnect once and wakes the
 *     reconnect worker to run a full two-phase re-auth. */
static void session_on_conn(void *user, int rc)
{
    ThingModelClientSession_t *s = user;
    const T_ThingModelOsal *o = ThingModelOsal_Get();

    /* Internal plumbing during a (re)auth handshake — the driver reports success/failure
     * via session_do_auth()'s return code, so ignore the raw callbacks entirely. */
    if (s->auth_in_progress) return;

    /* Dedupe the drop under reconn_lock — paired with the worker's success-commit and Connect's
     * initial commit. Doing this under the lock closes the race where a drop lands exactly as a
     * (re)connect commits: it is then either swallowed (auth_in_progress still set) or seen here
     * as a fresh drop (connected already true), never lost. It also collapses the callback storm
     * from libmosquitto's brief self-reconnect attempts to a single handling. */
    o->mutex_lock(s->reconn_lock);
    if (!s->connected) { o->mutex_unlock(s->reconn_lock); return; }
    s->connected = false;
    bool shutting = s->shutting_down;
    bool wake = (s->auto_reconnect && !s->shutting_down && s->reconn_wake);
    if (wake) s->reconn_pending = true;
    o->mutex_unlock(s->reconn_lock);

    /* Intentional teardown (Disconnect() set shutting_down before dropping the transport):
     * this is not a real connection loss — don't emit the misleading "scheduling re-auth"
     * WARN or fire the drop callback. */
    if (shutting) return;

    LOG_WARN("[RECONNECT] connection dropped (rc=%d) — scheduling full two-phase re-auth", rc);
    if (s->on_disconnect) s->on_disconnect(s->on_disc_user, rc ? rc : -1);
    if (wake) o->event_signal(s->reconn_wake);
}

/* ---------- Authentication (shared by initial Connect + reconnect worker) ---------- */

#ifndef PHASE1_CONNECT_MAX_RETRIES
#define PHASE1_CONNECT_MAX_RETRIES 5
#endif
#ifndef PHASE1_CONNECT_RETRY_DELAY_MS
#define PHASE1_CONNECT_RETRY_DELAY_MS 2000
#endif

/* Run the FULL access authentication using the material persisted on `s`.
 * Called for the initial connect and, on every later drop, by reconnect_worker() — each call
 * runs a fresh Phase-1 TLS handshake → fresh RFC5705 keying material → fresh Phase-2 token
 * (DOCK profile: a single plaintext connect). This is exactly what recovers from a client-id
 * collision: replaying the cached Phase-2 token alone is rejected by the broker once the
 * collision overwrote the broker-side keying material for this SN.
 *
 * Fires no user callbacks and touches no connected/reconnect state — the caller owns that.
 * The caller MUST set s->auth_in_progress=true around this call so the transport conn callbacks
 * emitted here (Phase-1 connect+teardown, Phase-2 connect) are swallowed by session_on_conn.
 * Returns THING_MODEL_RC_OK once a live Phase-2 (or plaintext) session is established. */
static T_ThingModelReturnCode session_do_auth(ThingModelClientSession_t *s)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->shutting_down) return THING_MODEL_RC_UNKNOWN;

#if THING_MODEL_PROFILE_DOCK
    if (s->plaintext_access) {
        /* ====== Plaintext access: single plaintext MQTT ======
         * username = app_id "_esdk_" device_sn (+ username_suffix),
         * password = app_key "_" app_license, client_id = account (no suffix).
         * No TLS, no HMAC, connect directly to broker_port (default 1883). */
        char account[256], user_buf[288], pass_buf[1024];
        snprintf(account, sizeof(account), "%s_esdk_%s",
                 s->app_id ? s->app_id : "", s->orig_device_sn);
        snprintf(pass_buf, sizeof(pass_buf), "%s_%s",
                 s->app_key ? s->app_key : "",
                 s->app_license ? s->app_license : "");

        const char *e_bare = getenv("DOCK_BARE_SN");
        bool bare_sn = (e_bare && e_bare[0] == '1');
        const char *id_base = bare_sn ? s->orig_device_sn : account;

        if (s->username_suffix && s->username_suffix[0])
            snprintf(user_buf, sizeof(user_buf), "%s%s", id_base, s->username_suffix);
        else
            snprintf(user_buf, sizeof(user_buf), "%s", id_base);

        /* topic owner = client_id. In account mode override s->device_sn=account, but only once
         * (on the first auth) — device_sn is the reused owner; recomputing on reconnect from an
         * already-overwritten value would double-prefix it. */
        if (!bare_sn && !s->phase2_connected_once)
            snprintf(s->device_sn, sizeof(s->device_sn), "%s", account);

        T_ThingModelTransportConfig tcfg = {
            .host           = s->broker_addr,
            .port           = s->broker_port,
            .use_tls        = false,
            .client_id      = id_base,
            .username       = user_buf,
            .password       = pass_buf,
            .keepalive_sec  = s->keepalive_sec,
            .clean_session  = true,
            .hmac_enabled   = false,
            /* Reconnect is owned by the session-layer worker, not libmosquitto. */
            .auto_reconnect = false,
        };
        LOG_INFO("plaintext connect %s:%u user=%s", tcfg.host, tcfg.port, user_buf);
        T_ThingModelReturnCode rc = s->trans_vt->connect(s->trans, &tcfg);
        if (!THING_MODEL_IS_OK(rc)) {
            LOG_ERROR("plaintext connect failed: %d", rc);
            thing_model_record_error(rc, 0, 0, "plaintext connect failed: rc=%d", rc);
            return rc;
        }
        LOG_INFO("plaintext connected, session ready");
        return THING_MODEL_RC_OK;
    }
#endif /* THING_MODEL_PROFILE_DOCK */

    /* ====== Phase 1: TLS auth ====== */
    {
        char user_buf[256], pass_buf[512];
        /* Username format:
         *   with app_name:    "{app_id}:{app_name}_psdk_{device_id}"
         *   without app_name: "{app_id}_psdk_{device_id}"   (backward compatible)
         * app_name identifies the application in the username. */
        if (s->app_name && s->app_name[0]) {
            snprintf(user_buf, sizeof(user_buf), "%s:%s_psdk_%s",
                     s->app_id, s->app_name, s->orig_device_sn);
        } else {
            snprintf(user_buf, sizeof(user_buf), "%s_psdk_%s",
                     s->app_id, s->orig_device_sn);
        }
        uint64_t ts = o->time_ms();
        /* newlib-nano on GD32 does not implement %llu; the secure-access
         * timestamp is monotonic milliseconds and fits uint32_t before the
         * FreeRTOS tick wrap. */
        snprintf(pass_buf, sizeof(pass_buf), "%s_%s_%lu",
                 s->app_key, s->app_license ? s->app_license : "",
                 (unsigned long)(uint32_t)ts);
        LOG_INFO("Phase1 ts_ms=%lu", (unsigned long)(uint32_t)ts);

        T_ThingModelTransportConfig tcfg = {
            .host           = s->broker_addr,
            .port           = s->broker_port_tls ? s->broker_port_tls : 8883,
            .use_tls        = true,
            .ca_cert_path   = s->ca_cert_path,
            .client_id      = s->orig_device_sn,
            .username       = user_buf,
            .password       = pass_buf,
            .keepalive_sec  = s->keepalive_sec,
            .clean_session  = true,
            .hmac_enabled   = false,
        };
        LOG_INFO("Phase1 TLS connect %s:%u", tcfg.host, tcfg.port);

        /* Phase 1 retry — the RTOS may boot faster than the aircraft broker, the first TCP/TLS will fail
         * (status=256 DISCONNECTED, the aircraft port is not listening yet). Retry up to N times,
         * with increasing intervals, giving the aircraft enough time to come up. The host also benefits (when the broker is temporarily unreachable). */
        T_ThingModelReturnCode rc = THING_MODEL_RC_UNKNOWN;
        for (int attempt = 1; attempt <= PHASE1_CONNECT_MAX_RETRIES; attempt++) {
            if (s->shutting_down) return THING_MODEL_RC_UNKNOWN;   /* bail fast on teardown */
            rc = s->trans_vt->connect(s->trans, &tcfg);
            if (THING_MODEL_IS_OK(rc)) break;

            LOG_WARN("Phase1 connect attempt %d/%d failed rc=%d, retrying in %d ms ...",
                     attempt, PHASE1_CONNECT_MAX_RETRIES, (int)rc,
                     PHASE1_CONNECT_RETRY_DELAY_MS);
            if (attempt < PHASE1_CONNECT_MAX_RETRIES) {
                s->trans_vt->disconnect(s->trans);   /* ensure clean before retry */
                if (o) o->sleep_ms(PHASE1_CONNECT_RETRY_DELAY_MS);
            }
        }
        if (!THING_MODEL_IS_OK(rc)) {
            LOG_ERROR("Phase1 connect failed after %d retries: %d",
                      PHASE1_CONNECT_MAX_RETRIES, (int)rc);
            thing_model_record_error(rc, 0, 0,
                "Phase1 (TLS) connect failed after %d retries: rc=%d",
                PHASE1_CONNECT_MAX_RETRIES, (int)rc);
            return rc;
        }

        rc = s->trans_vt->export_keying_material(s->trans, s->hmac_key);
        if (!THING_MODEL_IS_OK(rc)) { LOG_ERROR("export_keying_material failed: %d", rc); return rc; }

        s->trans_vt->disconnect(s->trans);
        LOG_INFO("Phase1 done, hmac key exported");
    }

    /* ====== Phase 2: plain MQTT with HMAC ====== */
    {
        char token_buf[80];
        if (ThingModelSA_ComputeAuthToken(s->crypto_vt, s->hmac_key, s->orig_device_sn,
                                          token_buf, sizeof(token_buf)) != 0) {
            LOG_ERROR("compute auth token failed"); return THING_MODEL_RC_AUTH_FAILED;
        }
        T_ThingModelTransportConfig tcfg = {
            .host           = s->broker_addr,
            .port           = s->broker_port,
            .use_tls        = false,
            .client_id      = s->orig_device_sn,
            .username       = token_buf,
            .password       = "",
            .keepalive_sec  = s->keepalive_sec,
            .clean_session  = true,
            .hmac_enabled   = true,
            .hmac_key       = s->hmac_key,
            .crypto         = s->crypto_vt,
            /* Reconnect is owned by the session-layer worker (reconnect_worker), which re-runs
             * this whole two-phase flow. We do NOT hand reconnect to libmosquitto: it would only
             * replay this token on the plaintext socket, which the broker rejects after the
             * Phase-1 keying material is invalidated (e.g. a same-SN client-id collision). */
            .auto_reconnect = false,
        };
        LOG_INFO("Phase2 plain connect %s:%u", tcfg.host, tcfg.port);
        T_ThingModelReturnCode rc = s->trans_vt->connect(s->trans, &tcfg);
        if (!THING_MODEL_IS_OK(rc)) {
            LOG_ERROR("Phase2 connect failed: %d", rc);
            thing_model_record_error(rc, 0, 0, "Phase2 (plain MQTT + HMAC) connect failed: rc=%d", rc);
            return rc;
        }
        LOG_INFO("Phase2 connected, session ready");
    }
    return THING_MODEL_RC_OK;
}

/* Reconnect worker thread — owns full two-phase re-auth on any unexpected drop.
 * Woken by session_on_conn() when the steady-state connection drops; runs re-auth attempts with
 * interruptible exponential backoff until success or Disconnect() sets shutting_down. Each attempt
 * first tears down the old transport connection (libmosquitto's own loop may have half-reconnected
 * with the stale token) before running session_do_auth() from scratch. */
static void reconnect_worker(void *arg)
{
    ThingModelClientSession_t *s = arg;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    const uint16_t min_s = s->reconnect_delay_min_sec ? s->reconnect_delay_min_sec : 1;
    const uint16_t max_s = s->reconnect_delay_max_sec ? s->reconnect_delay_max_sec : 30;

    while (!s->shutting_down) {
        /* Idle until a drop wakes us; the 500ms cap just bounds how long teardown waits. */
        (void)o->event_wait(s->reconn_wake, 500);
        if (s->shutting_down) break;
        if (!s->reconn_pending) continue;

        uint32_t delay_s = min_s;
        while (!s->shutting_down && s->reconn_pending) {
            /* Interruptible backoff — Disconnect() signals reconn_wake to break out promptly. */
            (void)o->event_wait(s->reconn_wake, delay_s * 1000u);
            if (s->shutting_down) break;

            /* Stop whatever libmosquitto's loop thread did on its own before a clean re-auth. */
            s->trans_vt->disconnect(s->trans);

            LOG_INFO("[RECONNECT] full re-auth attempt (backoff %us) ...", delay_s);
            s->auth_in_progress = true;
            T_ThingModelReturnCode rc = session_do_auth(s);

            if (THING_MODEL_IS_OK(rc)) {
                /* Commit connected + clear pending together with the auth_in_progress release, all
                 * under reconn_lock. A drop landing at this instant is then never lost: it is either
                 * swallowed while auth_in_progress is still set, or handled as a fresh drop by
                 * session_on_conn once connected=true is visible. */
                o->mutex_lock(s->reconn_lock);
                s->connected        = true;
                s->reconn_pending   = false;
                s->auth_in_progress = false;
                o->mutex_unlock(s->reconn_lock);
                session_resubscribe(s);
                LOG_INFO("[RECONNECT] re-auth succeeded, session restored");
                if (s->on_connect)   s->on_connect(s->on_disc_user, true);
                if (s->on_reconnect) s->on_reconnect(s->on_disc_user);
                break;
            }
            s->auth_in_progress = false;
            LOG_WARN("[RECONNECT] re-auth attempt failed rc=%d, backing off", (int)rc);
            delay_s = (delay_s * 2u > max_s) ? max_s : delay_s * 2u;
        }
    }
    LOG_INFO("[RECONNECT] worker exiting");
}

/* ---------- Lifecycle ---------- */

ThingModelClientSession_t *ThingModelClientSession_Connect(const ThingModelClientSession_Config_t *cfg)
{
    if (!cfg || !cfg->app_id || !cfg->app_key || !cfg->device_sn || !cfg->broker_address) {
        LOG_ERROR("missing required config field");
        thing_model_record_error(THING_MODEL_RC_INVALID_ARG, 0, 0,
                                 "missing required config (app_id/app_key/device_sn/broker_address)");
        return NULL;
    }
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (!o) { LOG_ERROR("OSAL not registered; call ThingModelOsal_RegisterPosix() first"); return NULL; }

    ThingModelClientSession_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    snprintf(s->device_sn,   sizeof(s->device_sn),   "%s", cfg->device_sn);
    snprintf(s->broker_addr, sizeof(s->broker_addr), "%s", cfg->broker_address);
    s->broker_port        = cfg->broker_port ? cfg->broker_port : 1883;
    s->keepalive_sec      = cfg->keepalive_sec ? cfg->keepalive_sec : 60;
    s->default_timeout_ms = cfg->default_timeout_ms ? cfg->default_timeout_ms : DEFAULT_TIMEOUT_MS;
    s->topic_style        = cfg->topic_style;
    s->on_disconnect      = cfg->on_disconnect;
    s->on_connect         = cfg->on_connect;
    s->on_reconnect       = cfg->on_reconnect;
    s->on_disc_user       = cfg->user;
    s->auto_reconnect     = cfg->auto_reconnect;
    s->reconnect_delay_min_sec = cfg->reconnect_delay_min_sec;
    s->reconnect_delay_max_sec = cfg->reconnect_delay_max_sec;

    /* Persist the access material so the reconnect worker can re-run the full two-phase auth
     * later (cfg strings are caller-owned and may not outlive the session). orig_device_sn keeps
     * the unmutated SN for auth identity even if device_sn is later rewritten to a DOCK account. */
    snprintf(s->orig_device_sn, sizeof(s->orig_device_sn), "%s", cfg->device_sn);
    s->broker_port_tls = cfg->broker_port_tls ? cfg->broker_port_tls : 8883;
    s->app_id       = ThingModelMem_Strdup(cfg->app_id);
    s->app_name     = ThingModelMem_Strdup(cfg->app_name);
    s->app_key      = ThingModelMem_Strdup(cfg->app_key);
    s->app_license  = ThingModelMem_Strdup(cfg->app_license);
    s->ca_cert_path = ThingModelMem_Strdup(cfg->ca_cert_path);
    if ((cfg->app_id && !s->app_id) || (cfg->app_key && !s->app_key)) {
        LOG_ERROR("persist auth material failed (out of memory)"); goto fail;
    }
#if THING_MODEL_PROFILE_DOCK
    s->plaintext_access = cfg->plaintext_access;
    s->username_suffix  = ThingModelMem_Strdup(cfg->username_suffix);
#endif

    s->seq_lock      = o->mutex_create();
    s->listener_lock = o->mutex_create();
    s->subs_lock     = o->mutex_create();
    s->reconn_lock   = o->mutex_create();   /* always present — session_on_conn locks it even without a worker */
    if (!s->seq_lock || !s->listener_lock || !s->subs_lock || !s->reconn_lock) goto fail;

    /* ----- transport / crypto vtable -----
     * By default, weak-symbol fallback to the factory of thing_model_client_{transport_mosquitto,crypto_openssl};
     * the linker side is responsible for pulling in these two default impl libs (the sample's cmake uses --whole-archive
     * so that even weak references pull in the .o, otherwise GNU ld by default won't pull an object referenced only by a weak ref from a static library → NULL).
     * Users who want to replace the implementation: give your own vtable in cfg + don't link the default impl lib in cmake. */
    extern const T_ThingModelTransportVtable *ThingModelTransport_Mosquitto(void)
        __attribute__((weak));
    extern const T_ThingModelCryptoVtable    *ThingModelCrypto_OpenSSL(void)
        __attribute__((weak));

    s->trans_vt = cfg->transport_vtable;
    if (!s->trans_vt && ThingModelTransport_Mosquitto)
        s->trans_vt = ThingModelTransport_Mosquitto();
    if (!s->trans_vt) {
        LOG_ERROR("no transport vtable: link thing_model_client_transport_mosquitto "
                  "(with --whole-archive, see sample CMakeLists), or give your own implementation in cfg->transport_vtable");
        goto fail;
    }

    s->crypto_vt = cfg->crypto_vtable;
    if (!s->crypto_vt && ThingModelCrypto_OpenSSL)
        s->crypto_vt = ThingModelCrypto_OpenSSL();
    if (!s->crypto_vt) {
        LOG_ERROR("no crypto vtable: link thing_model_client_crypto_openssl "
                  "(with --whole-archive, see sample CMakeLists), or give your own implementation in cfg->crypto_vtable");
        goto fail;
    }

    s->trans = s->trans_vt->create();
    if (!s->trans) { LOG_ERROR("transport create failed"); goto fail; }
    s->trans_vt->set_message_cb(s->trans, session_on_message, s);
    s->trans_vt->set_conn_cb   (s->trans, session_on_conn,    s);

    /* Reconnect wake event (only when auto_reconnect) — created before the first auth so a drop
     * right after the initial connect is picked up by the worker. */
    if (s->auto_reconnect) {
        s->reconn_wake = o->event_create();
        if (!s->reconn_wake) { LOG_ERROR("reconnect wake event create failed"); goto fail; }
    }

    /* Initial authentication (full two-phase secure access, or DOCK plaintext). Swallow the
     * transport conn callbacks it emits — success/failure is the return code. */
    s->auth_in_progress = true;
    T_ThingModelReturnCode arc = session_do_auth(s);
    if (!THING_MODEL_IS_OK(arc)) { s->auth_in_progress = false; goto fail; }

    /* Commit connected together with the auth_in_progress release under reconn_lock (same reason
     * as the worker's success-commit) so an immediate post-connect drop is handled, not lost. */
    o->mutex_lock(s->reconn_lock);
    s->connected             = true;
    s->phase2_connected_once = true;
    s->auth_in_progress      = false;
    o->mutex_unlock(s->reconn_lock);
    if (s->on_connect) s->on_connect(s->on_disc_user, false);   /* first connect (is_reconnect=false) */

    /* Spawn the reconnect worker — it owns full re-auth on any later drop. If it can't start we
     * keep the live session but log that auto-reconnect is unavailable. */
    if (s->auto_reconnect) {
        s->reconn_thread = o->thread_create(reconnect_worker, s, "tm_reconn", 0);
        if (!s->reconn_thread)
            LOG_WARN("reconnect worker create failed — auto-reconnect disabled for this session");
    }

    return s;

fail:
    ThingModelClientSession_Disconnect(s);
    return NULL;
}

void ThingModelClientSession_Disconnect(ThingModelClientSession_t *s)
{
    if (!s) return;
    const T_ThingModelOsal *o = ThingModelOsal_Get();

    /* 1) Stop the reconnect worker first — it uses s->trans, so join it before touching the transport. */
    s->shutting_down = true;
    if (s->reconn_wake)   o->event_signal(s->reconn_wake);
    if (s->reconn_thread) { o->thread_destroy(s->reconn_thread); s->reconn_thread = NULL; }

    /* 2) Tear down the transport — this stops the transport loop thread, after which no further
     *    session_on_conn() (which locks reconn_lock) can run. */
    if (s->trans) {
        s->trans_vt->disconnect(s->trans);
        s->trans_vt->destroy(s->trans);
        s->trans = NULL;
    }

    /* 3) Now that no thread can touch them, drop the reconnect primitives. */
    if (s->reconn_wake) { o->event_destroy(s->reconn_wake); s->reconn_wake = NULL; }
    if (s->reconn_lock) { o->mutex_destroy(s->reconn_lock); s->reconn_lock = NULL; }

    ListenerEntry_t *e = s->listeners;
    while (e) { ListenerEntry_t *n = e->next; free(e); e = n; }

    /* Free the subscription-persistence list */
    SubscriptionEntry_t *se = s->subs;
    while (se) { SubscriptionEntry_t *n = se->next; free(se); se = n; }

    if (s->seq_lock)      o->mutex_destroy(s->seq_lock);
    if (s->listener_lock) o->mutex_destroy(s->listener_lock);
    if (s->subs_lock)     o->mutex_destroy(s->subs_lock);

    /* Free the persisted access material (allocated with ThingModelMem_Strdup) */
    ThingModelMem_Free(s->app_id);
    ThingModelMem_Free(s->app_name);
    ThingModelMem_Free(s->app_key);
    ThingModelMem_Free(s->app_license);
    ThingModelMem_Free(s->ca_cert_path);
#if THING_MODEL_PROFILE_DOCK
    ThingModelMem_Free(s->username_suffix);
#endif
    free(s);
}

/* ---------- Accessors ---------- */

const char *ThingModelClientSession_GetSn(const ThingModelClientSession_t *s)
{
    return s ? s->device_sn : NULL;
}

E_ThingModelTopicStyle ThingModelClientSession_GetTopicStyle(const ThingModelClientSession_t *s)
{
    return s ? s->topic_style : THING_MODEL_TOPIC_STYLE_V2;
}

const T_ThingModelOsal *ThingModelClientSession_GetOsal(const ThingModelClientSession_t *s)
{
    (void)s;
    return ThingModelOsal_Get();
}

uint32_t ThingModelClientSession_NextSeq(ThingModelClientSession_t *s)
{
    if (!s) return 0;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    o->mutex_lock(s->seq_lock);
    uint32_t v = ++s->next_seq;
    o->mutex_unlock(s->seq_lock);
    return v;
}

/* ---------- Wire ops ---------- */

T_ThingModelReturnCode ThingModelClientSession_Publish(ThingModelClientSession_t *s, const char *topic,
                                         const char *json_payload, int qos)
{
    if (!s || !topic) return THING_MODEL_RC_INVALID_ARG;
    size_t len = json_payload ? strlen(json_payload) : 0;

    /* At DEBUG level, print the wire JSON (the pure JSON before HMAC signing, human-readable).
     * Enable: export THING_MODEL_LOG_LEVEL=debug, or in code ThingModelLog_SetLevel(DEBUG). */
    if (json_payload) {
        if (len <= 1024) {
            LOG_DEBUG("PUB topic=%s json=%s", topic, json_payload);
        } else {
            LOG_DEBUG("PUB topic=%s json=%.*s ... [%zu more bytes]",
                      topic, 1024, json_payload, len - 1024);
        }
    }
    /* wire capture (THING_MODEL_WIRE_LOG=1): full raw sent text (the pure JSON before HMAC signing), not truncated. */
    ThingModelLog_Wire("→PUB", topic, json_payload, len);
    return s->trans_vt->publish(s->trans, topic, json_payload, len, qos, false);
}

T_ThingModelReturnCode ThingModelClientSession_Subscribe(ThingModelClientSession_t *s,
                                           const char *topic_filter, int qos)
{
    if (!s || !topic_filter) return THING_MODEL_RC_INVALID_ARG;

    /* Persistence: add to the subscription list; on reconnect session_on_conn walks it and re-subscribes.
     * Duplicate Subscribe on the same topic is not de-duplicated — no incremental complexity; the mosquitto side is idempotent. */
    if (s->auto_reconnect) {
        const T_ThingModelOsal *o = ThingModelOsal_Get();
        SubscriptionEntry_t *e = calloc(1, sizeof(*e));
        if (e) {
            snprintf(e->topic, sizeof(e->topic), "%s", topic_filter);
            e->qos = qos;
            o->mutex_lock(s->subs_lock);
            e->next = s->subs;
            s->subs = e;
            o->mutex_unlock(s->subs_lock);
        }
    }

    return s->trans_vt->subscribe(s->trans, topic_filter, qos);
}

T_ThingModelReturnCode ThingModelClientSession_Unsubscribe(ThingModelClientSession_t *s,
                                             const char *topic_filter)
{
    if (!s || !topic_filter) return THING_MODEL_RC_INVALID_ARG;

    /* Remove from the subscription list (no longer re-subscribed on reconnect) */
    if (s->auto_reconnect) {
        const T_ThingModelOsal *o = ThingModelOsal_Get();
        o->mutex_lock(s->subs_lock);
        SubscriptionEntry_t **p = &s->subs;
        while (*p) {
            if (strcmp((*p)->topic, topic_filter) == 0) {
                SubscriptionEntry_t *dead = *p;
                *p = dead->next;
                free(dead);
                break;
            }
            p = &(*p)->next;
        }
        o->mutex_unlock(s->subs_lock);
    }

    return s->trans_vt->unsubscribe(s->trans, topic_filter);
}

/* ---------- listener registration ---------- */

int32_t ThingModelClientSession_AddListener(ThingModelClientSession_t *s,
                                     T_ThingModelSessionMsgCb cb, void *user)
{
    if (!s || !cb) return -1;
    ListenerEntry_t *e = calloc(1, sizeof(*e));
    if (!e) return -1;
    e->cb   = cb;
    e->user = user;

    const T_ThingModelOsal *o = ThingModelOsal_Get();
    o->mutex_lock(s->listener_lock);
    e->handle = ++s->next_listener_handle;
    e->next = s->listeners;
    s->listeners = e;
    o->mutex_unlock(s->listener_lock);
    return e->handle;
}

void ThingModelClientSession_RemoveListener(ThingModelClientSession_t *s, int32_t handle)
{
    if (!s) return;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    o->mutex_lock(s->listener_lock);
    ListenerEntry_t **pp = &s->listeners;
    while (*pp) {
        if ((*pp)->handle == handle) {
            ListenerEntry_t *e = *pp;
            *pp = e->next;
            free(e);
            break;
        }
        pp = &(*pp)->next;
    }
    o->mutex_unlock(s->listener_lock);
}
