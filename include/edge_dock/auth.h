/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain 1: device auth access and capability reporting (third party pushes state, role=server).
 */
#ifndef EDGE_DOCK_AUTH_H
#define EDGE_DOCK_AUTH_H
#include "thing_model_client/thing_model_client_session.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Report the capability set capability_report(dt5, device_type=5, third-party dock) to the state topic.
 * Fire-and-forget: the return code only says the PUBLISH left the client. A dock that refuses the
 * report (see the _sync variants) does so at the reply layer, which this variant never reads — use
 * it only where blocking is not allowed, e.g. DockSessionOpts.on_reconnect. */
int auth_report_capability_dt5(ThingModelClientSession_t *session);
/* Report the capability set capability_report(dt7, device_type=7, RID device) to the state topic.
 * Only after declaring the RID device do capability domain 5 rid_infos/adsb_infos take effect.
 * Fire-and-forget, same caveat as the dt5 variant above. */
int auth_report_capability_dt7(ThingModelClientSession_t *session);

/* Synchronous capability reporting — PREFER THESE at connect time. After sending capability_report
 * they subscribe to state_reply and wait for the acknowledgement, verifying data.result==0 and
 * printing data.error_reason when it is not.
 *
 * This is the only way to see a REJECTED report: publishing succeeds either way, and a real dock
 * refuses at the reply layer, e.g. result=201 "Device type slot is full, max limit reached for this
 * type" when the gateway's slots for that device_type are already taken. Left unnoticed, the device
 * stays unregistered and the dock silently discards every point-to-point requests_reply
 * (sdr / network / charging), which surfaces only as unexplained timeouts.
 *
 * timeout_ms<=0 uses the default 5000ms.
 * Returns 0=success; THING_MODEL_RC_TIMEOUT=timed out without receiving a reply; other=reply
 * received but result!=0.
 *
 * The state_reply subscription is left in place on purpose, so the session keeps receiving
 * acknowledgements for the fire-and-forget reports that DockSessionOpts.on_reconnect sends after a
 * reconnect; attach a session listener to see them. */
int auth_report_capability_dt5_sync(ThingModelClientSession_t *session, int timeout_ms);
int auth_report_capability_dt7_sync(ThingModelClientSession_t *session, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
