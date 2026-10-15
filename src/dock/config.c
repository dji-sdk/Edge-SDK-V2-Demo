/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain (4): parameter and firmware configuration -- uses the generated ConfigDock_* client API.
 */
#include "edge_dock/config.h"
#include "config/config_dock_client.h"
#include "config/config_types.h"
#include "thing_model_client/thing_model_client_session.h"
#include "thing_model_client/thing_model_client_osal.h"
#include "dock_color.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UP_TIMEOUT_MS 5000
#define TSL_PENDING_MAX 32   /* max entries accumulated in one set_tsl round */

struct DockConfig {
    ConfigDock_Client_t *client;
    T_TslParamEntry      pending[TSL_PENDING_MAX];   /* on-demand params pending commit */
    size_t               pending_count;
};

DockConfig_t *dock_config_create(ThingModelClientSession_t *session)
{
    if (!session) return NULL;
    DockConfig_t *c = (DockConfig_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->client = ConfigDock_CreateClient(session);
    if (!c->client) { free(c); return NULL; }
    return c;
}
void dock_config_destroy(DockConfig_t *c)
{
    if (!c) return;
    if (c->client) ConfigDock_DestroyClient(c->client);
    free(c);
}

static int report_common(const char *name, T_ThingModelReturnCode rc, const T_ThingModelCommonReply *rep)
{
    if (THING_MODEL_IS_OK(rc)) printf("  [%s] " DOCK_OK ", result=%d\n", name, rep ? rep->result : 0);
    else                       printf("  [%s] " DOCK_FAIL " rc=%d\n", name, rc);
    return (int)rc;
}

/* -- TSL enum value -> human-readable semantics (for friendly get_tsl printing; PDF §1.1) -- */
static const char *tsl_work_mode_str(int v) {
    switch (v) { case 0: return "roaming"; case 1: return "external-dock"; case 2: return "simple-airport"; default: return "unknown"; }
}
static const char *tsl_charger_str(int v) {
    /* PDF §1.1 charger_state: 15 states (0..14) */
    switch (v) {
        case 0:  return "not charging";
        case 1:  return "waiting for charge cmd";
        case 2:  return "charging";
        case 3:  return "charge done";
        case 4:  return "cannot talk to aircraft";
        case 5:  return "input/battery voltage cannot start charge";
        case 6:  return "interface-board voltage diff too large";
        case 7:  return "no charge current";
        case 8:  return "battery recoverable abnormal (temp)";
        case 9:  return "battery unrecoverable abnormal (temp)";
        case 10: return "two-battery voltage diff too large & aircraft on";
        case 11: return "battery temp above start-charge temp";
        case 12: return "battery not present";
        case 13: return "power abnormal";
        case 14: return "battery heating";
        default: return "unknown";
    }
}
static const char *tsl_station_mode_str(int v) {
    /* PDF §1.1 station_mode_code (basestation work state) */
    switch (v) {
        case 0: return "idle";
        case 1: return "on-site debug";
        case 2: return "remote debug";
        case 3: return "firmware upgrading";
        case 4: return "working";
        case 5: return "to be calibrated";
        case 6: return "low-power mode";
        default: return "unknown";
    }
}

int dock_config_get_tsl_params(DockConfig_t *c)
{
    return dock_config_get_tsl_params_ex(c, true);
}

/* The 39-param dump goes out in ONE printf. glibc only guarantees atomicity per stdio call, and the
 * SDK's MQTT receive thread writes to the same stdout (samples/cli reply_dump_cb dumps every
 * requests_reply), so the old one-printf-per-line version left 38 seams for a reply dump to cut in
 * -- on real hardware whole JSON lines showed up between station_work_mode and third_dock_work_mode.
 * Format and args are split into a pair of macros so the OK / partial branches share one copy;
 * keep the two lists in the SAME order -- 14 of the args are %.0f doubles, so a swap compiles
 * clean and only shows up as a voltage printed under a current's name. */
#define TSL_DUMP_FMT                                                       \
    /* -- flight geo (read_write) -- */                                    \
    "    drone_home_point_longitude           = %.6f\n"                    \
    "    drone_home_point_latitude            = %.6f\n"                    \
    "    drone_home_point_height              = %.2f m\n"                  \
    "    drone_alternate_land_point_longitude = %.6f\n"                    \
    "    drone_alternate_land_point_latitude  = %.6f\n"                    \
    "    drone_alternate_land_point_height    = %.2f m\n"                  \
    /* -- station / dock work mode -- */                                   \
    "    station_work_mode                    = %d (%s)\n"                 \
    "    third_dock_work_mode                 = %d\n"                      \
    "    station_mode_code                    = %d (%s)\n"                 \
    /* -- aircraft state (read_only) -- */                                 \
    "    drone_in_dock                        = %d (%s)\n"                 \
    "    drone_is_open                        = %d (%s)\n"                 \
    "    drone_is_bt_connect                  = %d (%s)\n"                 \
    /* -- charging / battery (read_only) -- */                             \
    "    charger_state                        = %d (%s)\n"                 \
    "    battery_voltage                      = %.0f mV\n"                 \
    "    battery_current                      = %.0f mA\n"                 \
    "    battery_temp                         = %.1f (0.1C)\n"             \
    "    battery_soc                          = %d %%\n"                   \
    "    battery_fc                           = %d (%s)\n"                 \
    "    battery_cuv                          = %d (%s)\n"                 \
    "    battery_cycle                        = %d\n"                      \
    /* -- wireless charging TX (read_only) -- */                           \
    "    tx_voltage                           = %.0f mV\n"                 \
    "    tx_current                           = %.0f mA\n"                 \
    "    coil_supply_state                    = %d\n"                      \
    "    tx_output_power                      = %.0f (1e-6 mW)\n"          \
    "    tx_resonant_current                  = %.0f mA\n"                 \
    /* -- wireless charging RX (read_only) -- */                           \
    "    rx_voltage                           = %.0f mV\n"                 \
    "    rx_current                           = %.0f mA\n"                 \
    "    rx_power                             = %.0f (1e-6 mW)\n"          \
    "    rx_power_need                        = %.0f (1e-6 mW)\n"          \
    /* -- temperatures (0.1 C) + buck (read_only) -- */                    \
    "    tx_coil_temp                         = %.1f (0.1C)\n"             \
    "    tx_mos_temp                          = %.1f (0.1C)\n"             \
    "    rx_coil_temp                         = %.1f (0.1C)\n"             \
    "    buck_state                           = %d (%s)\n"                 \
    /* -- power / ACDC (read_only) -- */                                   \
    "    electric_supply_voltage              = %.0f\n"                    \
    "    acdc_to_mainboard_voltage            = %.0f mV\n"                 \
    "    acdc_to_mainboard_current            = %.0f mA\n"                 \
    "    air_to_conditionner_voltage          = %.0f mV\n"                 \
    "    air_to_conditionner_current          = %.0f mA\n"                 \
    "    acdc_temperature                     = %.1f (0.1C)\n"

#define TSL_DUMP_ARGS(r)                                                                  \
    /* -- flight geo -- */                                                                \
    (r).drone_home_point_longitude,                                                       \
    (r).drone_home_point_latitude,                                                        \
    (r).drone_home_point_height,                                                          \
    (r).drone_alternate_land_point_longitude,                                             \
    (r).drone_alternate_land_point_latitude,                                              \
    (r).drone_alternate_land_point_height,                                                \
    /* -- station / dock work mode -- */                                                  \
    (r).station_work_mode, tsl_work_mode_str((r).station_work_mode),                      \
    (r).third_dock_work_mode,                                                             \
    (r).station_mode_code, tsl_station_mode_str((r).station_mode_code),                   \
    /* -- aircraft state -- */                                                            \
    (r).drone_in_dock,       (r).drone_in_dock       ? "in dock"   : "not in dock",       \
    (r).drone_is_open,       (r).drone_is_open       ? "on"        : "off",               \
    (r).drone_is_bt_connect, (r).drone_is_bt_connect ? "connected" : "not connected",     \
    /* -- charging / battery -- */                                                        \
    (r).charger_state, tsl_charger_str((r).charger_state),                                \
    (r).battery_voltage,                                                                  \
    (r).battery_current,                                                                  \
    (r).battery_temp,                                                                     \
    (r).battery_soc,                                                                      \
    (r).battery_fc,  (r).battery_fc  ? "FC" : "not FC",                                   \
    (r).battery_cuv, (r).battery_cuv ? "under-voltage protected" : "normal",              \
    (r).battery_cycle,                                                                    \
    /* -- wireless charging TX -- */                                                      \
    (r).tx_voltage,                                                                       \
    (r).tx_current,                                                                       \
    (r).coil_supply_state,                                                                \
    (r).tx_output_power,                                                                  \
    (r).tx_resonant_current,                                                              \
    /* -- wireless charging RX -- */                                                      \
    (r).rx_voltage,                                                                       \
    (r).rx_current,                                                                       \
    (r).rx_power,                                                                         \
    (r).rx_power_need,                                                                    \
    /* -- temperatures + buck -- */                                                       \
    (r).tx_coil_temp,                                                                     \
    (r).tx_mos_temp,                                                                      \
    (r).rx_coil_temp,                                                                     \
    (r).buck_state, (r).buck_state ? "on" : "off",                                        \
    /* -- power / ACDC -- */                                                              \
    (r).electric_supply_voltage,                                                          \
    (r).acdc_to_mainboard_voltage,                                                        \
    (r).acdc_to_mainboard_current,                                                        \
    (r).air_to_conditionner_voltage,                                                      \
    (r).air_to_conditionner_current,                                                      \
    (r).acdc_temperature

int dock_config_get_tsl_params_ex(DockConfig_t *c, bool print_data)
{
    /* The schema requires data.params to be a "non-empty array of property-name strings". All names
     * below are the full O4 TSL param set from gateway_config.json (tsl_config.tsl_params.param_config,
     * 39 params) — every name is recognized by the dock, so the whole request will not 301. */
    static const char *k_query_names[] = {
        /* -- read_write (param_table) -- */
        "drone_home_point_longitude", "drone_home_point_latitude", "drone_home_point_height",
        "drone_alternate_land_point_longitude", "drone_alternate_land_point_latitude",
        "drone_alternate_land_point_height", "station_work_mode", "third_dock_work_mode",
        /* -- read_only: station / aircraft state (osd_push) -- */
        "station_mode_code", "drone_in_dock", "drone_is_open", "drone_is_bt_connect",
        /* -- read_only: charging / battery -- */
        "charger_state", "battery_voltage", "battery_current", "battery_temp", "battery_soc",
        "battery_fc", "battery_cuv", "battery_cycle",
        /* -- read_only: wireless charging TX -- */
        "tx_voltage", "tx_current", "coil_supply_state", "tx_output_power", "tx_resonant_current",
        /* -- read_only: wireless charging RX -- */
        "rx_voltage", "rx_current", "rx_power", "rx_power_need",
        /* -- read_only: temperatures + buck -- */
        "tx_coil_temp", "tx_mos_temp", "rx_coil_temp", "buck_state",
        /* -- read_only: power / ACDC -- */
        "electric_supply_voltage", "acdc_to_mainboard_voltage", "acdc_to_mainboard_current",
        "air_to_conditionner_voltage", "air_to_conditionner_current", "acdc_temperature",
    };
    T_GetTslParamsRequest req; memset(&req, 0, sizeof(req));
    req.params       = (char **)k_query_names;
    req.params_count = sizeof(k_query_names) / sizeof(k_query_names[0]);
    T_TslParamValues resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_GetTslParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    /* Distinguish "no reply" from "reply arrived but data.result != 0":
     *   - RC_TIMEOUT  : the dock never answered → genuine failure, nothing to show.
     *   - RC_UNKNOWN  : the dock answered with data.result != 0 (typically 301 "some params not
     *                   found"). Querying all 39 params includes state-dependent telemetry
     *                   (wireless-charge TX/RX, battery_current/soc/temp, buck_state, …) that only
     *                   exists while the aircraft is docked & charging; when it isn't, those keys
     *                   are omitted and the dock flags a partial failure. The generated decoder has
     *                   ALREADY filled `resp` with the params that DID come back (missing ones stay
     *                   0), so print them instead of bailing — otherwise get_tsl "sometimes prints
     *                   nothing" purely because the dock state changed. */
    if (rc == THING_MODEL_RC_TIMEOUT) { printf("  [get_tsl_params] " DOCK_FAIL " no reply (timeout)\n"); return (int)rc; }
    /* print_data=false (e.g. quiet periodic polling): keep the one-line status, suppress the 39-param
     * dump so the console isn't flooded round after round. The query still runs and rc is returned. */
    if (!print_data) {
        if (THING_MODEL_IS_OK(rc)) printf("  [get_tsl_params] " DOCK_OK ", %zu params fetched (data print off)\n", req.params_count);
        else                       printf("  [get_tsl_params] " DOCK_FAIL " partial (rc=%d) (data print off)\n", rc);
        return (int)rc;
    }
    if (!THING_MODEL_IS_OK(rc))
        printf("  [get_tsl_params] " DOCK_FAIL " partial (rc=%d, some params unavailable in current dock state); showing what returned (missing = 0):\n"
               TSL_DUMP_FMT, rc, TSL_DUMP_ARGS(resp));
    else
        printf("  [get_tsl_params] " DOCK_OK ", current dock params (%zu):\n"
               TSL_DUMP_FMT, req.params_count, TSL_DUMP_ARGS(resp));
    return (int)rc;
}
#undef TSL_DUMP_FMT
#undef TSL_DUMP_ARGS

int dock_config_set_tsl_params(DockConfig_t *c)
{
    /* demo: set 3 params on demand then commit, showing variadic usage (aligned with raspberry_pi's set_tsl). */
    dock_config_tsl_set_home_point_longitude(c, 0.1);
    dock_config_tsl_set_station_work_mode(c, 1);
    dock_config_tsl_set_third_dock_work_mode(c, 1);
    return dock_config_set_tsl_params_commit(c);
}

/* ── set_tsl on-demand variadic params: accumulate + commit ─────────────────────────────────────── */

void dock_config_tsl_reset(DockConfig_t *c)
{
    if (c) c->pending_count = 0;
}

/* Push a new entry into the pending array, returning its index; -1 if full. */
static int tsl_push(DockConfig_t *c, E_TslParamEntryValueCase which)
{
    if (!c || c->pending_count >= TSL_PENDING_MAX) return -1;
    T_TslParamEntry *e = &c->pending[c->pending_count];
    memset(e, 0, sizeof(*e));
    e->value_case = which;
    return (int)c->pending_count++;
}

#define TSL_SETTER(fn, WHICH, field, ctype)                       \
    int fn(DockConfig_t *c, ctype v) {                            \
        int i = tsl_push(c, WHICH);                               \
        if (i < 0) { printf("  [set_tsl] accumulation full(%d)\n", TSL_PENDING_MAX); return -1; } \
        c->pending[i].field = v; return 0;                        \
    }

/* STRICT (PDF §1.1): only read_write properties get a setter. Read-only telemetry is get-only. */
TSL_SETTER(dock_config_tsl_set_home_point_longitude, TSL_PARAM_ENTRY_DRONE_HOME_POINT_LONGITUDE, drone_home_point_longitude, double)
TSL_SETTER(dock_config_tsl_set_home_point_latitude,  TSL_PARAM_ENTRY_DRONE_HOME_POINT_LATITUDE,  drone_home_point_latitude,  double)
TSL_SETTER(dock_config_tsl_set_home_point_height,    TSL_PARAM_ENTRY_DRONE_HOME_POINT_HEIGHT,    drone_home_point_height,    double)
TSL_SETTER(dock_config_tsl_set_alt_land_longitude,   TSL_PARAM_ENTRY_DRONE_ALTERNATE_LAND_POINT_LONGITUDE, drone_alternate_land_point_longitude, double)
TSL_SETTER(dock_config_tsl_set_alt_land_latitude,    TSL_PARAM_ENTRY_DRONE_ALTERNATE_LAND_POINT_LATITUDE,  drone_alternate_land_point_latitude,  double)
TSL_SETTER(dock_config_tsl_set_alt_land_height,      TSL_PARAM_ENTRY_DRONE_ALTERNATE_LAND_POINT_HEIGHT,    drone_alternate_land_point_height,    double)
TSL_SETTER(dock_config_tsl_set_station_work_mode,    TSL_PARAM_ENTRY_STATION_WORK_MODE,    station_work_mode,    int32_t)
TSL_SETTER(dock_config_tsl_set_third_dock_work_mode, TSL_PARAM_ENTRY_THIRD_DOCK_WORK_MODE, third_dock_work_mode, int32_t)
#undef TSL_SETTER

int dock_config_tsl_set_home_point(DockConfig_t *c, double lon, double lat, double height)
{
    int r = 0;
    r |= dock_config_tsl_set_home_point_longitude(c, lon);
    r |= dock_config_tsl_set_home_point_latitude(c, lat);
    r |= dock_config_tsl_set_home_point_height(c, height);
    return r ? -1 : 0;
}
int dock_config_tsl_set_work_mode(DockConfig_t *c, int32_t station_mode, int32_t third_dock_mode)
{
    int r = 0;
    r |= dock_config_tsl_set_station_work_mode(c, station_mode);
    r |= dock_config_tsl_set_third_dock_work_mode(c, third_dock_mode);
    return r ? -1 : 0;
}

int dock_config_set_tsl_params_commit(DockConfig_t *c)
{
    if (!c) return -1;
    if (c->pending_count == 0) {
        printf("  [set_tsl] no pending params; call dock_config_tsl_set_* before commit\n");
        return 0;
    }
    T_SetTslParamsRequest req; memset(&req, 0, sizeof(req));
    req.params       = c->pending;
    req.params_count = c->pending_count;
    T_TslParamValues resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_SetTslParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    if (THING_MODEL_IS_OK(rc)) printf("  [set_tsl] " DOCK_OK " (%zu params sent)\n", c->pending_count);
    else                       printf("  [set_tsl] " DOCK_FAIL " rc=%d (%zu params)\n", rc, c->pending_count);
    c->pending_count = 0;   /* clear after commit, start the next round */
    return (int)rc;
}

int dock_config_get_network_params(DockConfig_t *c)
{
    T_GetNetworkParamsRequest req; memset(&req, 0, sizeof(req));
    T_GetNetworkParamsReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_GetNetworkParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    if (THING_MODEL_IS_OK(rc)) {
        const T_NetworkStaticIpConf *ip = &resp.static_ip_conf;
        /* Header + detail in one printf, same reason as the get_tsl dump above: the MQTT thread's
         * reply dump would otherwise slot in between the two lines. */
        printf("  [get_network_params] " DOCK_OK " dynamic_ip=%d\n"
               /* Fill in all static_ip_conf fields, to verify the set(static) -> get read-back loop. */
               "    static_ip=%s netmask=%s gateway=%s pri_dns=%s sec_dns=%s\n",
               resp.dynamic_ip,
               ip->static_ip       ? ip->static_ip       : "",
               ip->netmask         ? ip->netmask         : "",
               ip->default_gateway ? ip->default_gateway : "",
               ip->pri_dns_ip      ? ip->pri_dns_ip      : "",
               ip->sec_dns_ip      ? ip->sec_dns_ip      : "");
    } else printf("  [get_network_params] " DOCK_FAIL " rc=%d\n", rc);
    return (int)rc;
}
int dock_config_set_network_params(DockConfig_t *c)
{
    T_SetNetworkParamsRequest req; memset(&req, 0, sizeof(req));
    req.dynamic_ip = true;   /* dynamic IP (DHCP) branch, static_ip_conf is not encoded */
    T_ThingModelCommonReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_SetNetworkParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    return report_common("set_network_params(dynamic)", rc, &resp);
}
int dock_config_set_network_params_static(DockConfig_t *c)
{
    /* demo defaults: identical to the schema example. Interactive callers use _ex with specific values. */
    return dock_config_set_network_params_static_ex(c,
            "192.168.1.100", "255.255.255.0", "192.168.1.1", "8.8.8.8", "8.8.4.4");
}
int dock_config_set_network_params_static_ex(DockConfig_t *c,
        const char *static_ip, const char *netmask, const char *gateway,
        const char *pri_dns, const char *sec_dns)
{
    /* Static IP branch: when dynamic_ip=false all static_ip_conf fields take effect (aligned with the schema example). */
    T_SetNetworkParamsRequest req; memset(&req, 0, sizeof(req));
    req.dynamic_ip = false;
    req.static_ip_conf.static_ip       = (char *)static_ip;
    req.static_ip_conf.netmask         = (char *)netmask;
    req.static_ip_conf.default_gateway = (char *)gateway;
    req.static_ip_conf.pri_dns_ip      = (char *)pri_dns;
    req.static_ip_conf.sec_dns_ip      = (char *)sec_dns;
    T_ThingModelCommonReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_SetNetworkParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    return report_common("set_network_params(static)", rc, &resp);
}
int dock_config_get_sdr_firmware(DockConfig_t *c)
{
    T_GetSdrFirmwareRequest req; memset(&req, 0, sizeof(req));
    T_GetSdrFirmwareReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_GetSdrFirmware(c->client, &req, &resp, UP_TIMEOUT_MS);
    if (THING_MODEL_IS_OK(rc)) printf("  [get_sdr_firmware] " DOCK_OK " drone_type=%d\n", resp.drone_type);
    else printf("  [get_sdr_firmware] " DOCK_FAIL " rc=%d\n", rc);
    return (int)rc;
}
int dock_config_set_sdr_firmware(DockConfig_t *c)
{
    return dock_config_set_sdr_firmware_ex(c, 100);   /* demo: M4TD */
}
int dock_config_set_sdr_firmware_ex(DockConfig_t *c, int drone_type)
{
    T_SetSdrFirmwareRequest req; memset(&req, 0, sizeof(req));
    req.drone_type = drone_type;   /* schema: 100=M4TD, 103=M400; do not use invalid values */
    T_ThingModelCommonReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_SetSdrFirmware(c->client, &req, &resp, UP_TIMEOUT_MS);
    return report_common("set_sdr_firmware", rc, &resp);
}
int dock_config_set_cloud_params(DockConfig_t *c)
{
    return dock_config_set_cloud_params_ex(c, 1, "mqtt://example.com:1883", "demo", "demo");
}
int dock_config_set_cloud_params_ex(DockConfig_t *c, int cloud_type,
        const char *mqtt_url, const char *mqtt_account, const char *mqtt_password)
{
    T_SetCloudParamsRequest req; memset(&req, 0, sizeof(req));
    req.cloud_type = cloud_type; req.mqtt_url = (char *)mqtt_url;
    req.mqtt_account = (char *)mqtt_account; req.mqtt_password = (char *)mqtt_password;
    T_ThingModelCommonReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_SetCloudParams(c->client, &req, &resp, UP_TIMEOUT_MS);
    return report_common("set_cloud_params", rc, &resp);
}

/* ── Cloud onboarding field policy (see config.h) ────────────────────────────────────────────── */

const char *dock_config_cloud_type_str(int cloud_type)
{
    switch (cloud_type) {
        case 0: return "FlightHub 2";
        case 1: return "third-party cloud";
        case 2: return "FlightHub private cloud";
        default: return "unknown";
    }
}
const char *dock_config_cloud_field_name(E_DockCloudField field)
{
    switch (field) {
        case DOCK_CLOUD_FIELD_MQTT_URL:             return "mqtt_url";
        case DOCK_CLOUD_FIELD_MQTT_ACCOUNT:         return "mqtt_account";
        case DOCK_CLOUD_FIELD_MQTT_PASSWORD:        return "mqtt_password";
        case DOCK_CLOUD_FIELD_ORGANIZATION_ID:      return "organization_id";
        case DOCK_CLOUD_FIELD_DEVICE_BINDING_CODE:  return "device_binding_code";
        default:                                    return "unknown";
    }
}
bool dock_config_cloud_field_allowed(int cloud_type, E_DockCloudField field)
{
    switch (cloud_type) {
        case 0:   /* FlightHub 2: DJI owns the broker, only the organization pairing is ours */
            return field == DOCK_CLOUD_FIELD_ORGANIZATION_ID ||
                   field == DOCK_CLOUD_FIELD_DEVICE_BINDING_CODE;
        case 1:   /* third-party cloud: our broker, our organization -> everything applies */
            return true;
        case 2:   /* private cloud: our address, but credentials are issued by the binding flow */
            return field != DOCK_CLOUD_FIELD_MQTT_ACCOUNT &&
                   field != DOCK_CLOUD_FIELD_MQTT_PASSWORD;
        default:  /* unknown type: no policy to apply, pass the caller's fields through unchanged */
            return true;
    }
}

/* NULL/"" means "not provided" (the JSON encoder emits every non-NULL pointer, and an empty
 * mqtt_url on the wire is worse than an absent one); a field the type does not accept is
 * dropped with a note so the operator sees why it never reached the dock. */
static const char *cloud_field_or_null(int cloud_type, E_DockCloudField field, const char *v)
{
    if (!v || !v[0]) return NULL;
    if (!dock_config_cloud_field_allowed(cloud_type, field)) {
        printf("  [cloud_onboard] %s does not apply to cloud_type=%d (%s), dropped\n",
               dock_config_cloud_field_name(field), cloud_type, dock_config_cloud_type_str(cloud_type));
        return NULL;
    }
    return v;
}

int dock_config_cloud_onboard(DockConfig_t *c, int cloud_type,
        const char *mqtt_url, const char *mqtt_account, const char *mqtt_password,
        const char *organization_id, const char *device_binding_code)
{
    if (!c) return -1;
    const char *url  = cloud_field_or_null(cloud_type, DOCK_CLOUD_FIELD_MQTT_URL,      mqtt_url);
    const char *acc  = cloud_field_or_null(cloud_type, DOCK_CLOUD_FIELD_MQTT_ACCOUNT,  mqtt_account);
    const char *pw   = cloud_field_or_null(cloud_type, DOCK_CLOUD_FIELD_MQTT_PASSWORD, mqtt_password);
    const char *org  = cloud_field_or_null(cloud_type, DOCK_CLOUD_FIELD_ORGANIZATION_ID,     organization_id);
    const char *code = cloud_field_or_null(cloud_type, DOCK_CLOUD_FIELD_DEVICE_BINDING_CODE, device_binding_code);

    int rc = dock_config_set_cloud_params_ex(c, cloud_type, url, acc, pw);
    if (rc != 0) {
        printf("  [cloud_onboard] set_cloud_params failed, organization bind skipped\n");
        return rc;
    }
    if (!org && !code) {
        printf("  [cloud_onboard] no organization_id / device_binding_code given, bind skipped\n");
        return rc;
    }
    return dock_config_organization_bind_ex(c, org, code);
}

int dock_config_organization_bind(DockConfig_t *c)
{
    return dock_config_organization_bind_ex(c, "org-demo-0001", "bind-code-demo");
}
int dock_config_organization_bind_ex(DockConfig_t *c,
        const char *organization_id, const char *device_binding_code)
{
    T_AirportOrganizationBindRequest req; memset(&req, 0, sizeof(req));
    req.organization_id = (char *)organization_id;
    req.device_binding_code = (char *)device_binding_code;
    T_ThingModelCommonReply resp; memset(&resp, 0, sizeof(resp));
    T_ThingModelReturnCode rc = ConfigDock_AirportOrganizationBind(c->client, &req, &resp, UP_TIMEOUT_MS);
    return report_common("airport_organization_bind", rc, &resp);
}
