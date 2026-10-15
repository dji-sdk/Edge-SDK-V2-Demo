/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain 2: takeoff/landing control and abnormal recovery (dock calls third party + third party pushes progress, role=server).
 */
#ifndef EDGE_DOCK_FLIGHT_H
#define EDGE_DOCK_FLIGHT_H
#include "thing_model_client/thing_model_client_session.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Flight Flight_t;
/* Create and register 5 service handlers (invoked when the dock issues a request; reply in_progress).
 * account = the session account / topic owner (dock_session_account(s)), NOT the raw device_sn. */
Flight_t *flight_create(ThingModelClientSession_t *session, const char *account);
void      flight_destroy(Flight_t *f);
/* Actively push percent 0→100 progress events.
 * Each event echoes the bid/tid of the service request that started the job: a dock matches events
 * to jobs by tid and silently discards anything else. With no such request outstanding -- a manual
 * push against a mock broker -- these fall back to fresh random ids and say so on stdout; a real
 * dock ignores those events. */
void flight_push_takeoff_prepare(Flight_t *f);
void flight_push_takeoff_end(Flight_t *f);
void flight_push_landing_prepare(Flight_t *f);
void flight_push_landing_end(Flight_t *f);
void flight_push_abnormal_recovery(Flight_t *f);
/* Single custom progress event (for dock_cli interactive input):
 * action: 0=takeoff_prepare 1=takeoff_end 2=landing_prepare 3=landing_end 4=abnormal_recovery
 * status: canceled/failed/in_progress/ok/paused/rejected/sent/timeout (schema enum). A terminal
 *         status (ok/failed/canceled/rejected/timeout) closes the transaction, after which further
 *         pushes for that action get fresh ids until the dock issues a new request.
 * percent: 0~100. The five flight_push_* above are convenience wrappers that auto-sweep 0->100. */
void flight_push_single(Flight_t *f, int action, const char *status, int percent);
/* services inbound self-test: feed 5 service requests back into the locally subscribed services topic,
 * triggering the handlers to reply services_reply(in_progress). Only valid on the local mock broker (real hardware is driven by O4). */
void flight_selftest_inject_services(Flight_t *f);
#ifdef __cplusplus
}
#endif
#endif
