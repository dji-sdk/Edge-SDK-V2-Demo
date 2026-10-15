/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 *
 * edge_dock.h —— aggregate header for third-party access to the O4 SDK (all five capability domains).
 * Third-party vendors that integrate BOTH devices only need to #include "edge_dock/edge_dock.h".
 *
 * If you only integrate ONE device, prefer the device-specific subset header instead
 * (and build with the matching CMake option so the other device is excluded entirely):
 *   • third-party dock (dt5): #include "edge_dock/edge_dock_dock.h"   (-DEDGE_DOCK_ENABLE_RID=OFF)
 *   • RID device        (dt7): #include "edge_dock/edge_dock_rid.h"    (-DEDGE_DOCK_ENABLE_DOCK=OFF)
 */
#ifndef EDGE_DOCK_H
#define EDGE_DOCK_H

#include "edge_dock/dock_session.h"       /* connection/session (incl. UDP discovery, bid/tid) */
#include "edge_dock/auth.h"               /* 1. device auth access and capability reporting */
#include "edge_dock/diag.h"               /* broker refusals (/error) + error-code decoding */
#include "edge_dock/flight.h"             /* 2. takeoff/landing control and abnormal recovery */
#include "edge_dock/charging.h"           /* 3. charging and aircraft power on/off control */
#include "edge_dock/config.h"             /* 4. parameter and firmware configuration */
#include "edge_dock/surveillance.h"       /* 5. ADS-B / RID surveillance */

#endif /* EDGE_DOCK_H */
