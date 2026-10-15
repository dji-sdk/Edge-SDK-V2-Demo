/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain (1): device authentication access and capability reporting.
 *
 * capability_report goes over the state topic; the envelope has no method field (golden only
 * requires bid/tid/data/timestamp/version), so the envelope is hand-assembled (the generated
 * EncodeRequest would force-inject method).
 * data.device_type selects the capability set: dt5 = third-party dock (capabilities key is
 * mandatory, value is an empty object {}), dt7 = RID device (capabilities.rid + capabilities.adsb
 * are mandatory, see V1.3).
 */
#include "edge_dock/auth.h"
#include "thing_model_client/thing_model_client_session.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_dock.h"
#include "dock_color.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* Assemble the capability_report envelope (bid/tid/version/timestamp + data), publish it to the
 * state topic, and release it.
 * Ownership of data is transferred to this function (attached to root, freed together with root).
 * The topic owner segment is taken from the session (under ESDK 2.0 plaintext this is the MQTT
 * account, not the raw SN), matching the generated code, to avoid the device replying with the
 * error "client ID in the topic does not match". */
static int auth_publish_capability(ThingModelClientSession_t *session,
                                   ThingModelJsonObj_t *data, const char *variant)
{
    ThingModelJsonObj_t *root = ThingModelJson_NewObject();
    if (!root) { if (data) ThingModelJson_Free(data); return THING_MODEL_RC_UNKNOWN; }

    char *bid = ThingModelDockIdProvider_MakeBid("capability_report");
    char *tid = ThingModelDockIdProvider_MakeTid();
    ThingModelJson_AddString(root, "bid", bid ? bid : "");
    ThingModelJson_AddString(root, "tid", tid ? tid : "");
    ThingModelJson_AddString(root, "version", "1.0.0");
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    ThingModelJson_AddInt(root, "timestamp", (o && o->time_ms) ? (int64_t)o->time_ms() : 0);
    ThingModelJson_AddObject(root, "data", data);

    char *payload = ThingModelJson_PrintCompact(root);
    ThingModelJson_Free(root);
    if (bid) ThingModelMem_Free(bid);
    if (tid) ThingModelMem_Free(tid);

    const char *account = ThingModelClientSession_GetSn(session);
    char topic[128];
    snprintf(topic, sizeof(topic), "thing/product/%s/state", account ? account : "");
    T_ThingModelReturnCode rc = ThingModelClientSession_Publish(session, topic, payload, 1);
    printf("  <report capability_report %s> topic=%s rc=%d\n", variant, topic, rc);
    if (payload) ThingModelMem_Free(payload);
    return (int)rc;
}

int auth_report_capability_dt5(ThingModelClientSession_t *session)
{
    if (!session) return THING_MODEL_RC_INVALID_ARG;
    ThingModelJsonObj_t *data = ThingModelJson_NewObject();
    if (!data) return THING_MODEL_RC_UNKNOWN;
    /* dt5: third-party dock. Aligned with dt7 -- the capabilities key must exist; dt5 has no
     * concrete capabilities, so the value is an empty object {} (the schema allows null|object;
     * here an empty object means "key present, value empty"). */
    ThingModelJson_AddInt(data, "device_type", 5);
    ThingModelJson_AddObject(data, "capabilities", ThingModelJson_NewObject());
    return auth_publish_capability(session, data, "dt5 device_type=5");
}

int auth_report_capability_dt7(ThingModelClientSession_t *session)
{
    if (!session) return THING_MODEL_RC_INVALID_ARG;
    ThingModelJsonObj_t *data = ThingModelJson_NewObject();
    if (!data) return THING_MODEL_RC_UNKNOWN;
    /* dt7: RID device. device_type + capabilities{rid, adsb} are all mandatory. */
    ThingModelJson_AddInt(data, "device_type", 7);
    ThingModelJsonObj_t *caps = ThingModelJson_NewObject();
    ThingModelJsonObj_t *rid = ThingModelJson_NewObject();
    ThingModelJson_AddBool(rid, "supported", true);
    ThingModelJson_AddString(rid, "version", "1.0.0");
    ThingModelJson_AddObject(caps, "rid", rid);
    ThingModelJsonObj_t *adsb = ThingModelJson_NewObject();
    ThingModelJson_AddBool(adsb, "supported", true);
    ThingModelJson_AddString(adsb, "version", "1.0.0");
    ThingModelJson_AddObject(caps, "adsb", adsb);
    ThingModelJson_AddObject(data, "capabilities", caps);
    return auth_publish_capability(session, data, "dt7 device_type=7 RID");
}

/* -- Synchronous capability report: wait for the state_reply acknowledgement to close the loop --
 *
 * This is the only way to learn that a capability report was REJECTED. Publishing succeeds (rc=0)
 * regardless — the dock refuses at the reply layer, e.g.
 *   {"data":{"result":201,"error_reason":"Device type slot is full, max limit reached for this type"}}
 * when the gateway's per-device-type slots are already taken. A fire-and-forget report leaves that
 * completely invisible, and the device then sits unregistered with every point-to-point
 * requests_reply (sdr / network / charging) silently discarded. */

/* Small context used while waiting for the acknowledgement. */
typedef struct {
    const T_ThingModelOsal *osal;
    void                   *ev;         /* binary event, signaled once the ack arrives */
    int                     result;     /* data.result, -1 = not yet received */
    bool                    got;        /* whether state_reply has been received */
    char                    reason[160];/* data.error_reason, "" when absent */
} CapReplyWait;

/* Invoked via the session fan-out when state_reply arrives: parse data.result / data.error_reason,
 * set the flag and wake the waiter.
 * Only accepts messages whose topic ends with "/state_reply", to avoid swallowing other acks. */
static void auth_state_reply_cb(void *user, const char *topic,
                               const char *payload, size_t len)
{
    CapReplyWait *w = (CapReplyWait *)user;
    if (!w || w->got || !topic || !strstr(topic, "/state_reply")) return;
    ThingModelJsonObj_t *root = ThingModelJson_ParseN(payload, len);
    if (!root) return;
    ThingModelJsonObj_t *data = ThingModelJson_GetItem(root, "data");
    w->result = (int)ThingModelJson_LookupInt(data, "result", -1);
    /* error_reason carries the only actionable part of a rejection ("Device type slot is full ..."),
     * so surface it rather than leaving the caller to decode a bare number. */
    const char *reason = ThingModelJson_LookupString(data, "error_reason", NULL);
    if (reason) snprintf(w->reason, sizeof(w->reason), "%s", reason);
    w->got    = true;
    ThingModelJson_Free(root);
    if (w->osal && w->ev && w->osal->event_signal) w->osal->event_signal(w->ev);
}

/* Shared body for both variants: send via `send_fn`, then wait for the ack. */
static int auth_report_capability_sync(ThingModelClientSession_t *session, int timeout_ms,
                                       int (*send_fn)(ThingModelClientSession_t *))
{
    if (!session || !send_fn) return THING_MODEL_RC_INVALID_ARG;

    const T_ThingModelOsal *osal  = ThingModelClientSession_GetOsal(session);
    const char             *account = ThingModelClientSession_GetSn(session);
    char reply_topic[128];
    snprintf(reply_topic, sizeof(reply_topic),
             "thing/product/%s/state_reply", account ? account : "");

    CapReplyWait w;
    memset(&w, 0, sizeof(w));
    w.osal   = osal;
    w.result = -1;
    w.ev     = (osal && osal->event_create) ? osal->event_create() : NULL;

    /* Subscribe and attach the listener first, then send, to make sure the ack cannot arrive
     * before the listener is in place and get missed. */
    ThingModelClientSession_Subscribe(session, reply_topic, 1);
    int32_t h = ThingModelClientSession_AddListener(session, auth_state_reply_cb, &w);

    /* Reuse the fire-and-forget send envelope (builds data{device_type:N} and logs). */
    int rc = send_fn(session);

    if (rc == THING_MODEL_RC_OK && w.ev && osal && osal->event_wait)
        osal->event_wait(w.ev, (uint32_t)(timeout_ms > 0 ? timeout_ms : 5000));

    /* Drop the listener — it points at the stack frame we are about to leave. Deliberately do NOT
     * unsubscribe: Unsubscribe() also removes the topic from the runtime's re-subscribe list, and
     * keeping it means the session still receives state_reply for the fire-and-forget report that
     * dock_session's reconnect hook sends. That reply then reaches whatever listener the app has
     * attached (the samples' reply_dump_cb), so a post-reconnect rejection stays visible too. */
    ThingModelClientSession_RemoveListener(session, h);
    if (w.ev && osal && osal->event_destroy) osal->event_destroy(w.ev);

    if (!w.got) {
        printf("  <capability_report ack> " DOCK_RED "timed out, no state_reply received" DOCK_RST "\n");
        return THING_MODEL_RC_TIMEOUT;
    }
    if (w.result != 0) {
        printf("  <capability_report ack> " DOCK_FAIL " result=%d%s%s\n",
               w.result, w.reason[0] ? " " : "", w.reason);
        return THING_MODEL_RC_UNKNOWN;
    }
    printf("  <capability_report ack> " DOCK_GRN "success" DOCK_RST " result=0\n");
    return THING_MODEL_RC_OK;
}

int auth_report_capability_dt5_sync(ThingModelClientSession_t *session, int timeout_ms)
{
    return auth_report_capability_sync(session, timeout_ms, auth_report_capability_dt5);
}

int auth_report_capability_dt7_sync(ThingModelClientSession_t *session, int timeout_ms)
{
    return auth_report_capability_sync(session, timeout_ms, auth_report_capability_dt7);
}
