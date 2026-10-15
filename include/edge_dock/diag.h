/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Diagnostics: the broker's rejection channel, and the decode tables for what it says.
 *
 * WHY THIS EXISTS. The gateway reports every ACL refusal on thing/product/<account>/error, and
 * nothing else in this SDK subscribes to it. A publish to a topic the account may not write, or a
 * subscribe outside the allowed set, therefore *succeeds locally*: Publish() returns 0 and the
 * PUBACK even comes back, while the broker drops the message and sends the only explanation to a
 * topic nobody is listening on. The symptom is a feature that "just doesn't work" with no error
 * anywhere -- the same shape as the 5Hz uplink limit, whose refusals also land here.
 *
 * Attach a DockDiag_t and that becomes a printed error code plus the topic that was refused.
 *
 * Two independent halves, use either on its own:
 *   - the observer  (dock_diag_create/start/wait/...) -- needs a live session;
 *   - the decoders  (dock_diag_error_str, dock_diag_check_publish, ...) -- pure functions, no
 *     session, no allocation, safe to call before connecting or in a unit test.
 */
#ifndef EDGE_DOCK_DIAG_H
#define EDGE_DOCK_DIAG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "thing_model_client/thing_model_client_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------
 * Error codes
 *
 * The gateway's ACL codes are structured 0x0101_<category>_<index>:
 *   0x010101xx  topic / ACL      16843008 .. 16843013
 *   0x010102xx  quota            16843265 (rate), 16843266 (size)
 * Unknown codes are still classified by that layout, so a code added on the gateway side later
 * decodes as "unknown <class> error" rather than as a bare number.
 * ------------------------------------------------------------------------------------------- */
#define DOCK_DIAG_ERR_UNKNOWN_AUTH        16843008L  /* 0x01010100 catch-all auth failure       */
#define DOCK_DIAG_ERR_TOPIC_MISMATCH      16843009L  /* 0x01010101 topic does not match rules   */
#define DOCK_DIAG_ERR_CLIENT_ID_MISMATCH  16843010L  /* 0x01010102 owner segment != client_id   */
#define DOCK_DIAG_ERR_SUB_NOT_ALLOWED     16843011L  /* 0x01010103 topic not subscribable       */
#define DOCK_DIAG_ERR_PUB_NOT_ALLOWED     16843012L  /* 0x01010104 topic not publishable        */
#define DOCK_DIAG_ERR_QOS_MISMATCH        16843013L  /* 0x01010105 wrong QoS for this topic     */
#define DOCK_DIAG_ERR_RATE_LIMIT          16843265L  /* 0x01010201 over 5 Hz                    */
#define DOCK_DIAG_ERR_PAYLOAD_TOO_LARGE   16843266L  /* 0x01010202 over the single-packet limit */

typedef enum {
    DOCK_DIAG_CLASS_UNKNOWN = 0,
    DOCK_DIAG_CLASS_TOPIC_ACL,     /* 0x010101xx */
    DOCK_DIAG_CLASS_QUOTA,         /* 0x010102xx */
} DockDiagClass_t;

/**
 * One refusal, captured by value.
 *
 * Everything is copied: the strings the session hands its listeners point into the receive
 * thread's buffer and are gone the moment the callback returns, so an event holding pointers
 * would be a use-after-free waiting for the first caller who stashes one.
 *
 * Sizes come from the observed traffic -- the longest error_reason seen on real hardware is 92
 * bytes ("The client ID in the topic does not match. Please confirm that the topic meets the
 * rules."), and topics are bounded by the account length.
 */
typedef struct {
    int64_t error_code;          /* data.error_code, e.g. 16843010                              */
    char    error_reason[192];   /* data.error_reason, "" when the gateway omits it             */
    char    topic[192];          /* data.topic: the topic that was REFUSED, not /error itself   */
    int64_t recv_ms;             /* capture time, for measuring how a burst of refusals spaces  */
} DockDiagEvent_t;

typedef struct DockDiag DockDiag_t;

/**
 * Async observer callback.
 *
 * Runs on the MQTT RECEIVE THREAD, inside the session's listener fan-out -- the same rules as any
 * session listener apply: never call a synchronous SDK API from here (it deadlocks), and if you
 * print, emit exactly ONE call. A multi-line print from this thread interleaves with whatever the
 * main thread is printing. dock_diag_format() exists so a one-line print is easy.
 */
typedef void (*DockDiagCb_t)(void *user, const DockDiagEvent_t *ev);

/* ---------------------------------------------------------------------------------------------
 * Observer lifecycle
 * ------------------------------------------------------------------------------------------- */

/** Allocate. Touches no wire: the topic is derived from the session's account, nothing is sent. */
DockDiag_t *dock_diag_create(ThingModelClientSession_t *session);

/** Stop, detach the listener, free. Safe on NULL. */
void dock_diag_destroy(DockDiag_t *d);

/**
 * Subscribe thing/product/<account>/error and attach the listener. qos<0 means 1 (what every
 * other topic in this SDK uses). Idempotent: a second start() returns 0 and does nothing.
 *
 * The QoS is a parameter rather than a constant because the /error subscription is itself subject
 * to the ACL: if the gateway pins a different QoS, this subscribe is refused, and the refusal is
 * announced on the very channel that just failed to subscribe. A caller that sees no events can
 * retry at another QoS to tell "the gateway allows everything" apart from "we cannot hear it".
 */
int dock_diag_start(DockDiag_t *d, int qos);

/**
 * Detach the listener. Deliberately does NOT unsubscribe: Unsubscribe() also drops the topic from
 * the runtime's re-subscribe list, and /error is precisely the channel you want still working
 * after an auto-reconnect (same reasoning as the state_reply subscription in auth.c).
 */
int dock_diag_stop(DockDiag_t *d);

/** Install/replace the async callback. NULL disables it. Safe to call while started. */
void dock_diag_set_callback(DockDiag_t *d, DockDiagCb_t cb, void *user);

/* ---------------------------------------------------------------------------------------------
 * Synchronous capture window -- what a test harness wants
 *
 * arm() then do the thing then wait(): a timeout means the broker did not object.
 * ------------------------------------------------------------------------------------------- */

/** Discard everything captured so far and open a new window. */
void dock_diag_arm(DockDiag_t *d);

/**
 * Block up to timeout_ms for the FIRST event of the current window and copy it to *out. Returns
 * immediately if one already landed before the call.
 *   0                      an event was captured
 *   THING_MODEL_RC_TIMEOUT the window stayed empty -- the broker accepted it, or is silent
 */
int dock_diag_wait(DockDiag_t *d, int timeout_ms, DockDiagEvent_t *out);

/**
 * How many events the current window has SEEN, including any dropped past the ring capacity.
 *
 * Never assume one violation produces one event: real hardware emits the same refusal more than
 * once for a single bad publish (two identical 16843010 lines 45 ms apart), and a rate-limit trip
 * produces a stream.
 */
int dock_diag_count(const DockDiag_t *d);

/**
 * Copy up to `max` events out in arrival order; returns how many were copied. When `total` is
 * non-NULL it receives the true window count, so a caller can see what it truncated.
 */
int dock_diag_drain(DockDiag_t *d, DockDiagEvent_t *out, int max, int *total);

/* ---------------------------------------------------------------------------------------------
 * Decoding -- pure, no session, no allocation
 * ------------------------------------------------------------------------------------------- */

/** Short English name. Never NULL; unrecognised codes decode via their class. */
const char *dock_diag_error_str(int64_t error_code);

/** One line of "what to change". Never NULL. */
const char *dock_diag_error_hint(int64_t error_code);

DockDiagClass_t dock_diag_error_class(int64_t error_code);

/**
 * MQTT 3.1.1 CONNACK return code 0..5.
 *
 * Provided here rather than leaving callers on mosquitto_connack_string() so the wording matches
 * the DJI access documentation, and so decoding a CONNACK does not drag libmosquitto into an
 * integrator's build.
 *
 * Note the SDK cannot currently hand you this code: the transport maps every CONNACK refusal to
 * THING_MODEL_RC_AUTH_FAILED and leaves GetLastError()->transport_rc at 0, so the raw 1..5 only
 * appears in the runtime's own log line. These helpers are for code that reads a CONNACK itself.
 */
const char *dock_diag_connack_str(int connack_rc);
const char *dock_diag_connack_hint(int connack_rc);

/**
 * Render an event as ONE line, ready for a single printf/fputs from a receive-thread callback:
 *   [diag] 16843010 client id in topic does not match | refused=thing/product/gnd/requests | <reason>
 * snprintf semantics: returns the length that would have been written, always NUL-terminates.
 */
int dock_diag_format(const DockDiagEvent_t *ev, char *buf, size_t cap);

/* ---------------------------------------------------------------------------------------------
 * Offline topic-rule check
 *
 * Predicts the gateway's verdict locally, from the documented V2 rules:
 *   template          thing/product/<owner>/<channel>, owner == account
 *   publishable       state / events / requests / osd / services_reply
 *   subscribable      state_reply / events_reply / requests_reply / services / error
 *   required QoS      1
 *
 * This is a PREDICTION, not an oracle. Where the gateway's real whitelist differs, the observed
 * error_code will differ from dock_diag_verdict_to_error_code() -- and that disagreement is the
 * finding, so do not treat this as ground truth.
 * ------------------------------------------------------------------------------------------- */
typedef enum {
    DOCK_DIAG_TOPIC_OK = 0,
    DOCK_DIAG_TOPIC_BAD_TEMPLATE,     /* not thing/product/<owner>/<channel> -> 16843009 */
    DOCK_DIAG_TOPIC_OWNER_MISMATCH,   /* owner segment != account            -> 16843010 */
    DOCK_DIAG_TOPIC_SUB_DENIED,       /* channel not subscribable            -> 16843011 */
    DOCK_DIAG_TOPIC_PUB_DENIED,       /* channel not publishable             -> 16843012 */
    DOCK_DIAG_TOPIC_QOS_DENIED,       /* channel fine, QoS is not the required one -> 16843013 */
} DockDiagVerdict_t;

/** account = dock_session_account(session). qos<0 skips the QoS check. */
DockDiagVerdict_t dock_diag_check_publish(const char *account, const char *topic, int qos);
DockDiagVerdict_t dock_diag_check_subscribe(const char *account, const char *topic, int qos);

/** DOCK_DIAG_TOPIC_OK maps to 0. */
int64_t dock_diag_verdict_to_error_code(DockDiagVerdict_t v);
const char *dock_diag_verdict_str(DockDiagVerdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* EDGE_DOCK_DIAG_H */
