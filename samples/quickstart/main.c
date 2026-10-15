/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * quickstart —— connect to O4 -> report capability (1) -> one charge_open (3).
 */
#include "edge_dock/edge_dock.h"
#include <stdio.h>

int main(void)
{
    DockSessionOpts opts;
    dock_session_default_opts(&opts);
    ThingModelClientSession_t *s = dock_session_connect(&opts);
    if (!s) { fprintf(stderr, "connection failed\n"); return 1; }

    /* (1) authenticated connect: report capability */
    auth_report_capability_dt5(s);

    /* (3) charging and aircraft power on/off: start charging */
    Charging_t *ch = charging_create(s);
    charging_charge_open(ch);
    charging_destroy(ch);

    dock_session_disconnect(s);
    return 0;
}
