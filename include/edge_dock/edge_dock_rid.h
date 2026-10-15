/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 *
 * edge_dock_rid.h —— aggregate header for the RID device (dt7) only.
 * Includes the common core (session/auth) plus the RID-specific surveillance domain
 * (ADS-B / RID). Does NOT pull in the dock charging/config/flight domains.
 *
 * Use this header if you only integrate the RID device (dt7) and build with
 * -DEDGE_DOCK_ENABLE_DOCK=OFF so the dock domains are excluded entirely.
 */
#ifndef EDGE_DOCK_RID_H
#define EDGE_DOCK_RID_H

#include "edge_dock/dock_session.h"       /* connection/session (incl. UDP discovery, bid/tid) */
#include "edge_dock/auth.h"               /* device auth access and capability reporting */
#include "edge_dock/diag.h"               /* broker refusals (/error) + error-code decoding */
#include "edge_dock/surveillance.h"       /* ADS-B / RID surveillance */

#endif /* EDGE_DOCK_RID_H */
