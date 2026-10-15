/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain 4: parameter and firmware configuration (third party calls the dock, role=client).
 */
#ifndef EDGE_DOCK_CONFIG_H
#define EDGE_DOCK_CONFIG_H
#include "thing_model_client/thing_model_client_session.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DockConfig DockConfig_t;
DockConfig_t *dock_config_create(ThingModelClientSession_t *session);
void          dock_config_destroy(DockConfig_t *c);
int dock_config_get_tsl_params(DockConfig_t *c);
/* Same as above but choose whether to print the full param dump. print_data=false keeps only the
 * one-line status (used by periodic polling so the fetched-data output can be turned off). */
int dock_config_get_tsl_params_ex(DockConfig_t *c, bool print_data);
int dock_config_set_tsl_params(DockConfig_t *c);   /* demo: set a few params on demand, then commit */

/* ── set_tsl on-demand variable-parameter API ──────────────────────────────
 * Usage: call several dock_config_tsl_set_* (each pushes one {attribute:value} into the array, choose as needed),
 * then dock_config_set_tsl_params_commit packages them all at once into the data.params array and sends it.
 * Attributes that were not set do not appear on the wire (underlying proto oneof entry, one single-key object per attribute). */
void dock_config_tsl_reset(DockConfig_t *c);                 /* clear the accumulator (auto-cleared after commit) */
int  dock_config_set_tsl_params_commit(DockConfig_t *c);     /* package, send, and clear the accumulator */

/* Type-safe single-parameter setters (return 0 on success, <0 if the accumulator is full).
 * STRICT (PDF §1.1): only read_write properties are settable — home point, alternate land point,
 * station_work_mode, third_dock_work_mode. Read-only telemetry (charger/battery/tx/rx/acdc ...)
 * is get-only and has no setter. */
int dock_config_tsl_set_home_point_longitude   (DockConfig_t *c, double v);
int dock_config_tsl_set_home_point_latitude    (DockConfig_t *c, double v);
int dock_config_tsl_set_home_point_height      (DockConfig_t *c, double v);   /* PDF: double (m) */
int dock_config_tsl_set_alt_land_longitude     (DockConfig_t *c, double v);
int dock_config_tsl_set_alt_land_latitude      (DockConfig_t *c, double v);
int dock_config_tsl_set_alt_land_height        (DockConfig_t *c, double v);   /* PDF: double (m) */
int dock_config_tsl_set_station_work_mode      (DockConfig_t *c, int32_t v);  /* 0=roaming 1=external-dock 2=simple-airport */
int dock_config_tsl_set_third_dock_work_mode   (DockConfig_t *c, int32_t v);  /* set 1 when integrating a third-party dock */

/* Per-type convenience helpers (internally just a few chained setters, no commit) */
int dock_config_tsl_set_home_point(DockConfig_t *c, double lon, double lat, double height);
int dock_config_tsl_set_work_mode (DockConfig_t *c, int32_t station_mode, int32_t third_dock_mode);

/* ── Cloud onboarding (set_cloud_params + airport_organization_bind) ───────
 * cloud_type is a fixed enum (schema requests-set_cloud_params):
 *   0 = FlightHub 2, 1 = third-party cloud, 2 = FlightHub private cloud.
 * Which of the five onboarding fields actually apply depends on that type — the address and
 * the MQTT credentials are only meaningful when the operator owns the broker:
 *   type 0 (FlightHub 2)      : organization_id + device_binding_code           (address is DJI's)
 *   type 1 (third-party cloud): all five fields
 *   type 2 (private cloud)    : mqtt_url + organization_id + device_binding_code
 * Fields that do not apply must NOT be put on the wire, so this policy is exposed as a
 * predicate: callers (dock_cli, dock_sample) use it to decide which fields to even prompt
 * for, and dock_config_cloud_onboard() enforces the same rule before sending. */
typedef enum {
    DOCK_CLOUD_FIELD_MQTT_URL = 0,       /* address  -> set_cloud_params.mqtt_url */
    DOCK_CLOUD_FIELD_MQTT_ACCOUNT,       /* account  -> set_cloud_params.mqtt_account */
    DOCK_CLOUD_FIELD_MQTT_PASSWORD,      /* password -> set_cloud_params.mqtt_password */
    DOCK_CLOUD_FIELD_ORGANIZATION_ID,    /* org id   -> airport_organization_bind.organization_id */
    DOCK_CLOUD_FIELD_DEVICE_BINDING_CODE,/* bind code-> airport_organization_bind.device_binding_code */
} E_DockCloudField;

bool        dock_config_cloud_field_allowed(int cloud_type, E_DockCloudField field);
const char *dock_config_cloud_field_name(E_DockCloudField field);
const char *dock_config_cloud_type_str(int cloud_type);   /* 0/1/2 -> readable name */

/* One-shot onboarding for one cloud type. Applies the policy above: fields that do not apply
 * to cloud_type, and fields passed as NULL/"", are omitted from the request entirely. Sends
 * set_cloud_params first, then airport_organization_bind when an organization id or binding
 * code was supplied (the bind is skipped if set_cloud_params itself failed). Returns 0 when
 * every request sent got an OK reply. */
int dock_config_cloud_onboard(DockConfig_t *c, int cloud_type,
        const char *mqtt_url, const char *mqtt_account, const char *mqtt_password,
        const char *organization_id, const char *device_binding_code);

int dock_config_get_network_params(DockConfig_t *c);
int dock_config_set_network_params(DockConfig_t *c);          /* dynamic_ip=true (DHCP) */
int dock_config_set_network_params_static(DockConfig_t *c);   /* dynamic_ip=false + all static_ip_conf fields (demo defaults) */
int dock_config_get_sdr_firmware(DockConfig_t *c);
int dock_config_set_sdr_firmware(DockConfig_t *c);
int dock_config_set_cloud_params(DockConfig_t *c);
int dock_config_organization_bind(DockConfig_t *c);

/* -- Parameterized _ex variants (for dock_cli interactive input; the no-arg versions above call
 *    these with demo defaults). -- */
int dock_config_set_network_params_static_ex(DockConfig_t *c,
        const char *static_ip, const char *netmask, const char *gateway,
        const char *pri_dns, const char *sec_dns);
int dock_config_set_sdr_firmware_ex(DockConfig_t *c, int drone_type);   /* 100=M4TD, 103=M400 */
int dock_config_set_cloud_params_ex(DockConfig_t *c, int cloud_type,
        const char *mqtt_url, const char *mqtt_account, const char *mqtt_password); /* cloud_type 0/1/2; NULL field = not sent */
int dock_config_organization_bind_ex(DockConfig_t *c,
        const char *organization_id, const char *device_binding_code);
#ifdef __cplusplus
}
#endif
#endif
