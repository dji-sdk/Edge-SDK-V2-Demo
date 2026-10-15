/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 *
 * edge_dock_dock.h —— aggregate header for the third-party DOCK device (dt5) only.
 * Includes the common core (session/auth) plus the dock-specific capability domains
 * (charging / config / flight). Does NOT pull in RID/surveillance.
 *
 * Use this header if you only integrate the third-party dock (dt5) and build with
 * -DEDGE_DOCK_ENABLE_RID=OFF so the surveillance domain is excluded entirely.
 */
#ifndef EDGE_DOCK_DOCK_H
#define EDGE_DOCK_DOCK_H

#include "edge_dock/dock_session.h"       /* connection/session (incl. UDP discovery, bid/tid) */
#include "edge_dock/auth.h"               /* device auth access and capability reporting */
#include "edge_dock/diag.h"               /* broker refusals (/error) + error-code decoding */
#include "edge_dock/flight.h"             /* takeoff/landing control and abnormal recovery */
#include "edge_dock/charging.h"           /* charging and aircraft power on/off control */
#include "edge_dock/config.h"             /* parameter and firmware configuration */

#endif /* EDGE_DOCK_DOCK_H */
