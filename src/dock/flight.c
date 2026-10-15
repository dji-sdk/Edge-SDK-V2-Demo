/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain (2): takeoff/landing control and abnormal recovery -- FlightDockImpl_* + FlightDockPublisher_*.
 *
 * One dock job = one transaction = one (bid, tid) pair, shared by all three legs: the inbound
 * `services` request, our `services_reply`, and every `events` progress frame. The generated code
 * gets the reply right on its own (FlightDock_EncodeReply echoes the inbound envelope), but the
 * publisher assembles events through FlightDock_EncodeRequest, which mints a fresh random pair per
 * frame -- so every progress frame used to arrive under an id the dock had never seen, and the dock
 * discarded the lot ("event ignored: tid mismatch") and sat waiting until the job timed out.
 *
 * Since neither the generated handler signature nor the publisher passes bid/tid through, this file
 * captures them off the wire (flight_services_sniffer_cb) and replays them around each publish via
 * the id provider's thread-local pin (flight_emit).
 */
#include "edge_dock/flight.h"
#include "edge_dock/dock_id_provider.h"
#include "flight/flight_dock_impl.h"
#include "flight/flight_dock_publisher.h"
#include "flight/flight_types.h"
#include "thing_model_client/thing_model_client_session.h"
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_dock.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "dock_color.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* action -> {method name, publisher fn}. The enum below indexes this table and is the `action`
 * argument documented on flight_push_single(). */
typedef T_ThingModelReturnCode (*pub_fn)(FlightDockPublisher_t *, const T_AirportActionProgress *);
static const struct { const char *name; pub_fn fn; } k_flight_actions[] = {
    { "airport_takeoff_prepare",   FlightDockPublisher_PublishAirportTakeoffPrepareProgress },
    { "airport_takeoff_end",       FlightDockPublisher_PublishAirportTakeoffEndProgress },
    { "airport_landing_prepare",   FlightDockPublisher_PublishAirportLandingPrepareProgress },
    { "airport_landing_end",       FlightDockPublisher_PublishAirportLandingEndProgress },
    { "airport_abnormal_recovery", FlightDockPublisher_PublishAirportAbnormalRecoveryProgress },
};
#define FLIGHT_ACTION_COUNT ((int)(sizeof(k_flight_actions) / sizeof(k_flight_actions[0])))
enum {
    FLIGHT_ACT_TAKEOFF_PREPARE = 0,
    FLIGHT_ACT_TAKEOFF_END,
    FLIGHT_ACT_LANDING_PREPARE,
    FLIGHT_ACT_LANDING_END,
    FLIGHT_ACT_ABNORMAL_RECOVERY,
};

/* A progress event and its request carry the same method string, so one lookup serves both
 * directions; which channel a message came from is decided by the topic, not the method. */
static int method_to_action(const char *method)
{
    for (int i = 0; i < FLIGHT_ACTION_COUNT; i++)
        if (method && strcmp(method, k_flight_actions[i].name) == 0) return i;
    return -1;
}

struct Flight {
    FlightDockImpl_t      *impl;
    FlightDockPublisher_t *pub;
    ThingModelClientSession_t *session;   /* for self-test injection: feed back into services topic */
    const T_ThingModelOsal *osal;
    char                   account[128]; /* topic owner segment (= session account) */
    char                   topic_services[192];
    size_t                 topic_services_len;
    int32_t                sniffer;       /* services listener handle, -1 = not attached */
    void                  *ids_lock;      /* guards last_ids */
    /* bid/tid of the service request currently outstanding per action, captured off the wire. */
    struct {
        char bid[DOCK_ID_MAX];
        char tid[DOCK_ID_MAX];
        bool fresh;                       /* captured and not yet handed to a publisher */
    } last_ids[FLIGHT_ACTION_COUNT];
    atomic_int             inflight;      /* # of detached progress workers still running (destroy waits on this) */
};

/* After the sync in_progress reply, spin up a detached thread to push 0..100 progress,
 * mimicking a real dock's async job. Defined below (needs the id capture / flight_sweep). */
static void flight_start_progress_async(Flight_t *f, const char *method);

/* ── inbound transaction ids ────────────────────────────────────────────────────────────────── */

/* strstr() would be actively wrong here: "thing/product/<sn>/services" is a prefix of
 * ".../services_reply", so a loose match would capture the ids off our OWN outbound reply. */
static bool flight_topic_matches(const Flight_t *f, const char *topic)
{
    if (!topic) return false;
    size_t n = strlen(topic);
    return n == f->topic_services_len && memcmp(topic, f->topic_services, n) == 0;
}

/* Runs on the transport thread, inside the session's listener lock: parse and record, nothing else
 * -- no printf, no SDK calls. Registered AFTER FlightDockImpl_Create so that it lands ahead of the
 * impl's own listener (the session prepends listeners and dispatches head-first), and because the
 * whole fanout for one message happens under that single lock, the entry this writes is still the
 * right one when the handler runs a few frames later. */
static void flight_services_sniffer_cb(void *user, const char *topic, const char *payload, size_t len)
{
    Flight_t *f = (Flight_t *)user;
    if (!f || !payload || !flight_topic_matches(f, topic)) return;

    ThingModelJsonObj_t *root = ThingModelJson_ParseN(payload, len);
    if (!root) return;
    const char *bid    = ThingModelJson_LookupString(root, "bid",    NULL);
    const char *tid    = ThingModelJson_LookupString(root, "tid",    NULL);
    int         action = method_to_action(ThingModelJson_LookupString(root, "method", NULL));
    /* An over-long id is dropped, never truncated: a truncated tid still looks like a tid, and the
     * dock would reject it with exactly the mismatch this capture exists to prevent. */
    if (action >= 0 && bid && bid[0] && tid && tid[0] &&
        strlen(bid) < DOCK_ID_MAX && strlen(tid) < DOCK_ID_MAX) {
        f->osal->mutex_lock(f->ids_lock);
        snprintf(f->last_ids[action].bid, DOCK_ID_MAX, "%s", bid);
        snprintf(f->last_ids[action].tid, DOCK_ID_MAX, "%s", tid);
        f->last_ids[action].fresh = true;
        f->osal->mutex_unlock(f->ids_lock);
    }
    ThingModelJson_Free(root);
}

/* Hand over the ids of the request still outstanding on this action and mark them consumed.
 * false = nothing outstanding (never dispatched, or already answered) -> caller falls back to fresh
 * random ids and says so; it never publishes a stale pair. bid/tid must be DOCK_ID_MAX bytes. */
static bool flight_take_ids(Flight_t *f, int action, char *bid, char *tid)
{
    bool ok = false;
    f->osal->mutex_lock(f->ids_lock);
    if (f->last_ids[action].fresh) {
        snprintf(bid, DOCK_ID_MAX, "%s", f->last_ids[action].bid);
        snprintf(tid, DOCK_ID_MAX, "%s", f->last_ids[action].tid);
        f->last_ids[action].fresh = false;
        ok = true;
    }
    f->osal->mutex_unlock(f->ids_lock);
    if (!ok) { bid[0] = '\0'; tid[0] = '\0'; }
    return ok;
}

/* Compare-and-clear, not a blind clear: a newer request for the same action may already have
 * replaced the entry, and wiping that would strand a live transaction. */
static void flight_clear_ids_if(Flight_t *f, int action, const char *tid)
{
    if (!tid || !tid[0]) return;
    f->osal->mutex_lock(f->ids_lock);
    if (strcmp(f->last_ids[action].tid, tid) == 0) {
        f->last_ids[action].bid[0] = '\0';
        f->last_ids[action].tid[0] = '\0';
        f->last_ids[action].fresh  = false;
    }
    f->osal->mutex_unlock(f->ids_lock);
}

/* ── event emission ─────────────────────────────────────────────────────────────────────────── */

/* Statuses that end the job on the dock's side; the schema's others (in_progress / paused / sent)
 * leave it open. Keyed on status rather than percent on purpose: a "failed" at percent=40 closes
 * the transaction just as surely as an "ok" at 100. */
static bool flight_status_is_terminal(const char *status)
{
    static const char *k_terminal[] = { "ok", "failed", "canceled", "rejected", "timeout" };
    if (!status) return false;
    for (size_t i = 0; i < sizeof(k_terminal) / sizeof(k_terminal[0]); i++)
        if (strcmp(status, k_terminal[i]) == 0) return true;
    return false;
}

/* The single point where a progress event goes out, so the pin can never leak: the publisher call
 * is straight-line code between pin and unpin.
 * Empty bid/tid = no outstanding request. Publish anyway -- exercising the events channel against a
 * mock broker with no dock attached is a supported workflow here -- but say plainly that a real
 * dock will drop it, rather than letting it look like it worked. */
static void flight_emit(Flight_t *f, int action, const char *status, int percent,
                        const char *bid, const char *tid)
{
    T_AirportActionProgress ev;
    memset(&ev, 0, sizeof(ev));
    ev.status = ThingModelMem_Strdup(status && status[0] ? status : "in_progress");
    ev.progress.percent = percent;

    bool pinned = (bid && bid[0] && tid && tid[0]);
    if (pinned) dock_id_provider_pin(bid, tid);
    T_ThingModelReturnCode rc = k_flight_actions[action].fn(f->pub, &ev);
    if (pinned) dock_id_provider_unpin();

    const char *col = (rc == 0) ? DOCK_GRN : DOCK_RED;
    if (pinned)
        printf("  <push %s status=%s percent=%d tid=%.8s> %src=%d%s\n",
               k_flight_actions[action].name, ev.status, percent, tid, col, rc, DOCK_RST);
    else
        printf("  <push %s status=%s percent=%d tid=" DOCK_RED "(fresh)" DOCK_RST "> %src=%d%s"
               "  -- no outstanding service request; a real dock ignores this event\n",
               k_flight_actions[action].name, ev.status, percent, col, rc, DOCK_RST);

    bool terminal = flight_status_is_terminal(ev.status);
    ThingModelMem_Free(ev.status);
    if (pinned && terminal) flight_clear_ids_if(f, action, tid);
}

/* 0 -> 100 in steps of 20, terminal frame last. The delay sits at the TOP of the loop on purpose:
 * svc() starts this from inside the impl's message dispatch, i.e. before the impl has published its
 * services_reply, so an immediate percent=0 could otherwise beat the reply onto the wire. */
static void flight_sweep(Flight_t *f, int action, const char *bid, const char *tid)
{
    for (int p = 0; p <= 100; p += 20) {
        usleep(200 * 1000);
        flight_emit(f, action, p >= 100 ? "ok" : "in_progress", p, bid, tid);
    }
}

/* ── inbound service handlers ───────────────────────────────────────────────────────────────── */

static T_ThingModelReturnCode svc(Flight_t *f, const char *name, T_AirportActionStatus *reply)
{
    printf("  <O4 dispatch %s> -> reply " DOCK_GRN "in_progress" DOCK_RST " + async progress\n", name);
    reply->status = ThingModelMem_Strdup("in_progress");
    flight_start_progress_async(f, name);   /* returns immediately; transport thread must not block */
    return THING_MODEL_RC_OK;
}
static T_ThingModelReturnCode h_tp(FlightDockImpl_t *i, void *u, const T_AirportTakeoffPrepareRequest *r, T_AirportActionStatus *y)
{ (void)i;(void)r; return svc((Flight_t *)u, "airport_takeoff_prepare", y); }
static T_ThingModelReturnCode h_te(FlightDockImpl_t *i, void *u, const T_AirportTakeoffEndRequest *r, T_AirportActionStatus *y)
{ (void)i;(void)r; return svc((Flight_t *)u, "airport_takeoff_end", y); }
static T_ThingModelReturnCode h_lp(FlightDockImpl_t *i, void *u, const T_AirportLandingPrepareRequest *r, T_AirportActionStatus *y)
{ (void)i;(void)r; return svc((Flight_t *)u, "airport_landing_prepare", y); }
static T_ThingModelReturnCode h_le(FlightDockImpl_t *i, void *u, const T_AirportLandingEndRequest *r, T_AirportActionStatus *y)
{ (void)i;(void)r; return svc((Flight_t *)u, "airport_landing_end", y); }
static T_ThingModelReturnCode h_ar(FlightDockImpl_t *i, void *u, const T_AirportAbnormalRecoveryRequest *r, T_AirportActionStatus *y)
{ (void)i;(void)r; return svc((Flight_t *)u, "airport_abnormal_recovery", y); }

Flight_t *flight_create(ThingModelClientSession_t *session, const char *account)
{
    if (!session) return NULL;
    Flight_t *f = (Flight_t *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->sniffer = -1;
    f->session = session;
    f->osal    = ThingModelClientSession_GetOsal(session);
    atomic_init(&f->inflight, 0);
    snprintf(f->account, sizeof(f->account), "%s", account ? account : "");
    snprintf(f->topic_services, sizeof(f->topic_services), "thing/product/%s/services", f->account);
    f->topic_services_len = strlen(f->topic_services);
    f->ids_lock = f->osal ? f->osal->mutex_create() : NULL;
    f->impl = FlightDockImpl_Create(session, account ? account : "");
    f->pub  = FlightDockPublisher_Create(session);
    if (!f->impl || !f->pub || !f->ids_lock) { flight_destroy(f); return NULL; }
    FlightDockImpl_OnAirportTakeoffPrepare (f->impl, h_tp, f);
    FlightDockImpl_OnAirportTakeoffEnd     (f->impl, h_te, f);
    FlightDockImpl_OnAirportLandingPrepare (f->impl, h_lp, f);
    FlightDockImpl_OnAirportLandingEnd     (f->impl, h_le, f);
    FlightDockImpl_OnAirportAbnormalRecovery(f->impl, h_ar, f);
    /* Deliberately AFTER FlightDockImpl_Create: the session prepends listeners and dispatches
     * head-first, so registering last puts the sniffer ahead of the impl's handler and every
     * request's bid/tid is on record before svc() asks for it. No Subscribe here -- the impl
     * already subscribes to this topic, and a matching Unsubscribe would drop it from the
     * runtime's reconnect replay list. */
    f->sniffer = ThingModelClientSession_AddListener(session, flight_services_sniffer_cb, f);
    return f;
}

void flight_destroy(Flight_t *f)
{
    if (!f) return;
    /* Order matters: close the inbound path first so nothing can start a new worker while we
     * drain, then wait the workers out, and only then free what they touch (pub, ids_lock, f).
     * Draining before dropping the listeners would let a request arriving mid-drain re-arm the
     * loop, and tearing down f->pub before the drain is a use-after-free in flight_emit. */
    if (f->sniffer >= 0) {
        ThingModelClientSession_RemoveListener(f->session, f->sniffer);
        f->sniffer = -1;
    }
    if (f->impl) { FlightDockImpl_Destroy(f->impl); f->impl = NULL; }
    while (atomic_load(&f->inflight) > 0) usleep(50 * 1000);
    if (f->pub) FlightDockPublisher_Destroy(f->pub);
    if (f->ids_lock && f->osal) f->osal->mutex_destroy(f->ids_lock);
    free(f);
}

/* ── outward-facing pushes ──────────────────────────────────────────────────────────────────── */

static void flight_push_auto(Flight_t *f, int action)
{
    if (!f) return;
    char bid[DOCK_ID_MAX], tid[DOCK_ID_MAX];
    flight_take_ids(f, action, bid, tid);
    flight_sweep(f, action, bid, tid);
}
void flight_push_takeoff_prepare(Flight_t *f){ flight_push_auto(f, FLIGHT_ACT_TAKEOFF_PREPARE); }
void flight_push_takeoff_end(Flight_t *f){ flight_push_auto(f, FLIGHT_ACT_TAKEOFF_END); }
void flight_push_landing_prepare(Flight_t *f){ flight_push_auto(f, FLIGHT_ACT_LANDING_PREPARE); }
void flight_push_landing_end(Flight_t *f){ flight_push_auto(f, FLIGHT_ACT_LANDING_END); }
void flight_push_abnormal_recovery(Flight_t *f){ flight_push_auto(f, FLIGHT_ACT_ABNORMAL_RECOVERY); }

void flight_push_single(Flight_t *f, int action, const char *status, int percent)
{
    if (!f) return;
    if (action < 0 || action >= FLIGHT_ACTION_COUNT) { printf("  unknown action=%d\n", action); return; }
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    char bid[DOCK_ID_MAX], tid[DOCK_ID_MAX];
    flight_take_ids(f, action, bid, tid);
    flight_emit(f, action, status, percent, bid, tid);
}

/* ── async progress worker ──────────────────────────────────────────────────────────────────────
 * After a handler synchronously replies in_progress, a detached thread pushes 0..100 progress
 * (last frame status=ok), mimicking a real dock reporting job progress.
 * Detached => self-reaping (no join); flight_destroy waits on f->inflight to avoid use-after-free.
 * This closes the inbound loop so a real O4 dispatch (or the case 22 self-test) drives both the
 * services_reply(in_progress) and the events progress stream. */
typedef struct {
    Flight_t *f;
    int       action;
    /* Copies, not pointers into last_ids: a second request for the same action would otherwise
     * rewrite this sweep's ids mid-flight. */
    char      bid[DOCK_ID_MAX];
    char      tid[DOCK_ID_MAX];
} ProgressJob_t;

static void *progress_worker(void *arg)
{
    ProgressJob_t *job = (ProgressJob_t *)arg;
    flight_sweep(job->f, job->action, job->bid, job->tid);
    atomic_fetch_sub(&job->f->inflight, 1);
    free(job);
    return NULL;
}

/* Claims the ids here rather than in svc() because this still runs on the transport thread, inside
 * the dispatch of the very message that triggered it -- the same listener-lock hold in which the
 * sniffer recorded them, so last_ids[action] cannot yet belong to anyone else. */
static void flight_start_progress_async(Flight_t *f, const char *method)
{
    int action = method_to_action(method);
    if (!f || action < 0) return;
    ProgressJob_t *job = (ProgressJob_t *)calloc(1, sizeof(*job));
    if (!job) return;
    job->f = f; job->action = action;
    if (!flight_take_ids(f, action, job->bid, job->tid))
        printf("  " DOCK_RED "<%s> no inbound bid/tid on record" DOCK_RST
               " -- progress events will carry fresh ids and the dock will ignore them\n", method);
    atomic_fetch_add(&f->inflight, 1);
    pthread_t th;
    if (pthread_create(&th, NULL, progress_worker, job) == 0) {
        pthread_detach(th);
    } else {
        atomic_fetch_sub(&f->inflight, 1);
        free(job);
    }
}

/* ── services inbound self-test injection ───────────────────────────────────────────────────
 * In dev mode when there is no O4 dispatch, feed a schema-compliant service request (data:{})
 * directly into the services topic this device subscribes to. Via broker loopback this triggers
 * the impl's on_session_message -> h_* handler -> reply services_reply(in_progress), thereby
 * self-testing the "inbound service" path without a real machine.
 * Only valid on a local mock broker: on a real machine, O4 is the only legitimate dispatcher of services. */
static void inject_one(Flight_t *f, const char *topic, const char *method)
{
    ThingModelJsonObj_t *root = ThingModelJson_NewObject();
    if (!root) return;
    /* Mints fresh ids on purpose: this stands in for the dock ORIGINATING a transaction, so it is
     * the one place in this file that must not reuse an existing pair. It shares a thread with
     * flight_push_single, which is why flight_emit's pin has to be released on every path -- a
     * leaked pin would make the next injection replay a spent transaction's ids. */
    char *bid = ThingModelDockIdProvider_MakeBid(method);
    char *tid = ThingModelDockIdProvider_MakeTid();
    ThingModelJson_AddString(root, "bid", bid ? bid : "");
    ThingModelJson_AddString(root, "tid", tid ? tid : "");
    ThingModelJson_AddString(root, "method", method);
    ThingModelJson_AddString(root, "version", "1.0.0");
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    ThingModelJson_AddInt(root, "timestamp", (o && o->time_ms) ? (int64_t)o->time_ms() : 0);
    ThingModelJson_AddObject(root, "data", ThingModelJson_NewObject());   /* data:{} no params */
    char *payload = ThingModelJson_PrintCompact(root);
    ThingModelJson_Free(root);
    if (bid) ThingModelMem_Free(bid);
    if (tid) ThingModelMem_Free(tid);
    if (payload) {
        printf("  <self-test inject %s> -> %s\n", method, topic);
        ThingModelClientSession_Publish(f->session, topic, payload, 1);
        ThingModelMem_Free(payload);
    }
}

void flight_selftest_inject_services(Flight_t *f)
{
    if (!f || !f->session) return;
    printf("  [services inbound self-test] feeding back 5 service requests (needs local mock broker; on a real machine O4 dispatches)\n");
    for (int i = 0; i < FLIGHT_ACTION_COUNT; i++) {
        inject_one(f, f->topic_services, k_flight_actions[i].name);
        usleep(150 * 1000);   /* interval to let the handler reply services_reply */
    }
}
