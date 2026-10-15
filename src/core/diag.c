/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Diagnostics part 3: the /error observer.
 *
 * Subscribes thing/product/<account>/error, decodes each refusal into a DockDiagEvent_t, and holds
 * the recent ones in a small ring so a caller can arm a window, do something illegal, and ask what
 * came back.
 *
 * THREADING. The session fans out to listeners on the MQTT receive thread. This file therefore
 * never prints: the listener parses, copies under the lock, and signals. Everything a human reads
 * is rendered by the caller, on the caller's thread, after dock_diag_wait() returns. That is
 * structural, not a convention -- a multi-line print from the receive thread interleaves with
 * whatever the main thread is writing, which is a bug this codebase has already had to fix once.
 */
#include "edge_dock/diag.h"

#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_client_osal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Deep enough for the rate-limit case, which answers a burst with a burst. Older events are
 * dropped rather than newer ones: the first refusals of a window are the ones that explain it. */
#define DIAG_RING_CAP 64

struct DockDiag {
    ThingModelClientSession_t *session;
    const T_ThingModelOsal    *osal;
    char   topic[192];              /* thing/product/<account>/error */
    size_t topic_len;

    void  *lock;
    void  *ev;                      /* signalled when an event lands in an armed window */
    int32_t listener;               /* -1 when not attached */
    bool    started;

    DockDiagEvent_t ring[DIAG_RING_CAP];
    int             count;          /* events in the current window, saturating at DIAG_RING_CAP */
    int             total;          /* events seen in the current window, including dropped ones */

    DockDiagCb_t cb;
    void        *cb_user;
};

/* strstr() would be wrong here: "/error" is short enough to appear inside a longer topic, and a
 * false match would attribute someone else's payload to the diagnostics channel. */
static bool diag_topic_matches(const DockDiag_t *d, const char *topic)
{
    if (!topic) return false;
    size_t n = strlen(topic);
    return n == d->topic_len && memcmp(topic, d->topic, n) == 0;
}

/* Runs on the MQTT receive thread. No printf, no synchronous SDK calls -- see the file header. */
static void diag_msg_cb(void *user, const char *topic, const char *payload, size_t len)
{
    DockDiag_t *d = (DockDiag_t *)user;
    if (!d || !diag_topic_matches(d, topic) || !payload) return;

    ThingModelJsonObj_t *root = ThingModelJson_ParseN(payload, len);
    if (!root) return;
    /* Observed shape: {"data":{"error_code":N,"error_reason":"...","topic":"..."}}. Fall back to
     * the root object so a gateway that ever drops the envelope still decodes. */
    ThingModelJsonObj_t *data = ThingModelJson_GetItem(root, "data");
    if (!data) data = root;

    DockDiagEvent_t e;
    memset(&e, 0, sizeof(e));
    e.error_code = ThingModelJson_LookupInt(data, "error_code", 0);
    const char *reason = ThingModelJson_LookupString(data, "error_reason", NULL);
    const char *rtopic = ThingModelJson_LookupString(data, "topic", NULL);
    if (reason) snprintf(e.error_reason, sizeof(e.error_reason), "%s", reason);
    if (rtopic) snprintf(e.topic, sizeof(e.topic), "%s", rtopic);
    e.recv_ms = (d->osal && d->osal->time_ms) ? (int64_t)d->osal->time_ms() : 0;
    ThingModelJson_Free(root);

    DockDiagCb_t cb = NULL;
    void *cb_user = NULL;
    if (d->lock && d->osal) d->osal->mutex_lock(d->lock);
    d->total++;
    if (d->count < DIAG_RING_CAP) d->ring[d->count++] = e;
    cb = d->cb;
    cb_user = d->cb_user;
    if (d->lock && d->osal) d->osal->mutex_unlock(d->lock);

    if (d->ev && d->osal && d->osal->event_signal) d->osal->event_signal(d->ev);
    /* Callback outside the lock: it is user code and may call back into this object. */
    if (cb) cb(cb_user, &e);
}

DockDiag_t *dock_diag_create(ThingModelClientSession_t *session)
{
    if (!session) return NULL;
    DockDiag_t *d = (DockDiag_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->session  = session;
    d->osal     = ThingModelClientSession_GetOsal(session);
    d->listener = -1;

    /* The owner segment is the account (app_id"_esdk_"device_sn), which is what GetSn() returns
     * under ESDK 2.0 plaintext access -- not the bare SN. Same rule as auth.c. */
    const char *account = ThingModelClientSession_GetSn(session);
    d->topic_len = (size_t)snprintf(d->topic, sizeof(d->topic),
                                    "thing/product/%s/error", account ? account : "");

    if (d->osal) {
        if (d->osal->mutex_create) d->lock = d->osal->mutex_create();
        if (d->osal->event_create) d->ev   = d->osal->event_create();
    }
    if (!d->lock) { free(d); return NULL; }
    return d;
}

void dock_diag_destroy(DockDiag_t *d)
{
    if (!d) return;
    /* Detach before freeing: the listener points at this object, and the receive thread may be
     * inside the fan-out right now. */
    dock_diag_stop(d);
    if (d->osal) {
        if (d->lock && d->osal->mutex_destroy) d->osal->mutex_destroy(d->lock);
        if (d->ev   && d->osal->event_destroy) d->osal->event_destroy(d->ev);
    }
    free(d);
}

int dock_diag_start(DockDiag_t *d, int qos)
{
    if (!d) return THING_MODEL_RC_INVALID_ARG;
    if (d->started) return THING_MODEL_RC_OK;

    if (qos < 0) qos = 1;
    /* Subscribe before attaching, so nothing can arrive with no listener in place. */
    int rc = (int)ThingModelClientSession_Subscribe(d->session, d->topic, qos);
    if (!THING_MODEL_IS_OK(rc)) return rc;

    d->listener = ThingModelClientSession_AddListener(d->session, diag_msg_cb, d);
    if (d->listener < 0) return THING_MODEL_RC_UNKNOWN;
    d->started = true;
    return THING_MODEL_RC_OK;
}

int dock_diag_stop(DockDiag_t *d)
{
    if (!d) return THING_MODEL_RC_INVALID_ARG;
    if (d->listener >= 0) {
        ThingModelClientSession_RemoveListener(d->session, d->listener);
        d->listener = -1;
    }
    /* No Unsubscribe on purpose: it would also drop the topic from the runtime's re-subscribe
     * list, and /error is exactly the channel that must survive an auto-reconnect. */
    d->started = false;
    return THING_MODEL_RC_OK;
}

void dock_diag_set_callback(DockDiag_t *d, DockDiagCb_t cb, void *user)
{
    if (!d) return;
    d->osal->mutex_lock(d->lock);
    d->cb = cb;
    d->cb_user = user;
    d->osal->mutex_unlock(d->lock);
}

void dock_diag_arm(DockDiag_t *d)
{
    if (!d) return;
    d->osal->mutex_lock(d->lock);
    d->count = 0;
    d->total = 0;
    d->osal->mutex_unlock(d->lock);
    /* Drain stale signals, so a wait() in the new window cannot return instantly on an event that
     * belonged to the previous one. The event is a counting semaphore, so drain to empty. */
    if (d->ev && d->osal->event_wait) {
        while (d->osal->event_wait(d->ev, 0) == THING_MODEL_RC_OK) { /* keep draining */ }
    }
}

int dock_diag_wait(DockDiag_t *d, int timeout_ms, DockDiagEvent_t *out)
{
    if (!d || !out) return THING_MODEL_RC_INVALID_ARG;
    if (timeout_ms < 0) timeout_ms = 0;

    uint64_t deadline = (d->osal && d->osal->time_ms)
                      ? d->osal->time_ms() + (uint64_t)timeout_ms : 0;
    for (;;) {
        bool got = false;
        d->osal->mutex_lock(d->lock);
        if (d->count > 0) { *out = d->ring[0]; got = true; }
        d->osal->mutex_unlock(d->lock);
        if (got) return THING_MODEL_RC_OK;

        if (!d->ev || !d->osal->event_wait) break;
        uint64_t now = d->osal->time_ms ? d->osal->time_ms() : deadline;
        if (now >= deadline) break;
        /* Loop rather than trusting one wait: the event is a binary flag, so a signal raised
         * between the count check and the wait would otherwise be lost. */
        d->osal->event_wait(d->ev, (uint32_t)(deadline - now));
    }

    bool got = false;
    d->osal->mutex_lock(d->lock);
    if (d->count > 0) { *out = d->ring[0]; got = true; }
    d->osal->mutex_unlock(d->lock);
    return got ? THING_MODEL_RC_OK : THING_MODEL_RC_TIMEOUT;
}

int dock_diag_count(const DockDiag_t *d)
{
    if (!d) return 0;
    DockDiag_t *m = (DockDiag_t *)d;   /* the lock is the only mutation here */
    m->osal->mutex_lock(m->lock);
    int n = m->total;
    m->osal->mutex_unlock(m->lock);
    return n;
}

int dock_diag_drain(DockDiag_t *d, DockDiagEvent_t *out, int max, int *total)
{
    if (!d || !out || max <= 0) return 0;
    d->osal->mutex_lock(d->lock);
    int n = d->count < max ? d->count : max;
    for (int i = 0; i < n; i++) out[i] = d->ring[i];
    if (total) *total = d->total;
    d->osal->mutex_unlock(d->lock);
    return n;
}
