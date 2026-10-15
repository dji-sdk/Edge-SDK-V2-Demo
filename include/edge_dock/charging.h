/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain 3: charging and aircraft power on/off control (third party calls the dock, role=client).
 */
#ifndef EDGE_DOCK_CHARGING_H
#define EDGE_DOCK_CHARGING_H
#include "thing_model_client/thing_model_client_session.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Charging Charging_t;
Charging_t *charging_create(ThingModelClientSession_t *session);
void        charging_destroy(Charging_t *c);
int charging_drone_open (Charging_t *c);   /* power on aircraft */
int charging_drone_close(Charging_t *c);   /* power off aircraft */
int charging_charge_open (Charging_t *c);  /* start charging */
int charging_charge_close(Charging_t *c);  /* stop charging */
#ifdef __cplusplus
}
#endif
#endif
