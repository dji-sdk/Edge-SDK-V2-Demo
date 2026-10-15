/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain (3): charging and aircraft power on/off control -- uses the generated ChargingDock_* client API.
 */
#include "edge_dock/charging.h"
#include "charging/charging_dock_client.h"
#include "charging/charging_types.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "dock_color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UP_TIMEOUT_MS 5000

struct Charging { ChargingDock_Client_t *client; };

Charging_t *charging_create(ThingModelClientSession_t *session)
{
    if (!session) return NULL;
    Charging_t *c = (Charging_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->client = ChargingDock_CreateClient(session);
    if (!c->client) { free(c); return NULL; }
    return c;
}
void charging_destroy(Charging_t *c)
{
    if (!c) return;
    if (c->client) ChargingDock_DestroyClient(c->client);
    free(c);
}

static int report(const char *name, T_ThingModelReturnCode rc, const T_ActionStatus *st)
{
    if (THING_MODEL_IS_OK(rc))
        printf("  [%s] " DOCK_OK ", status=%s\n", name, (st && st->status) ? st->status : "(null)");
    else
        printf("  [%s] " DOCK_FAIL " rc=%d\n", name, rc);
    return (int)rc;
}

int charging_drone_open(Charging_t *c)
{
    T_DroneOpenRequest req; memset(&req, 0, sizeof(req));
    T_ActionStatus resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ChargingDock_DroneOpen(c->client, &req, &resp, UP_TIMEOUT_MS);
    int r = report("drone_open", rc, &resp);
    if (resp.status) ThingModelMem_Free(resp.status);
    return r;
}
int charging_drone_close(Charging_t *c)
{
    T_DroneCloseRequest req; memset(&req, 0, sizeof(req));
    T_ActionStatus resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ChargingDock_DroneClose(c->client, &req, &resp, UP_TIMEOUT_MS);
    int r = report("drone_close", rc, &resp);
    if (resp.status) ThingModelMem_Free(resp.status);
    return r;
}
int charging_charge_open(Charging_t *c)
{
    T_ChargeOpenRequest req; memset(&req, 0, sizeof(req));
    T_ActionStatus resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ChargingDock_ChargeOpen(c->client, &req, &resp, UP_TIMEOUT_MS);
    int r = report("charge_open", rc, &resp);
    if (resp.status) ThingModelMem_Free(resp.status);
    return r;
}
int charging_charge_close(Charging_t *c)
{
    T_ChargeCloseRequest req; memset(&req, 0, sizeof(req));
    T_ActionStatus resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ChargingDock_ChargeClose(c->client, &req, &resp, UP_TIMEOUT_MS);
    int r = report("charge_close", rc, &resp);
    if (resp.status) ThingModelMem_Free(resp.status);
    return r;
}
