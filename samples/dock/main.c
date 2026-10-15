/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * dock_sample —— single-device demo for the third-party DOCK device (dt5) ONLY.
 *
 * This is the starting point for an integrator that only ships the third-party dock and
 * does NOT need the RID device. It includes only "edge_dock/edge_dock_dock.h" and uses
 * only the dock domains (charging / config / flight). Build with -DEDGE_DOCK_ENABLE_RID=OFF
 * to exclude the RID/surveillance code (and its generated protocol) from the binary entirely.
 *
 * Flow: connect -> auto report dt5 capability -> feature menu -> disconnect.
 */
#include "edge_dock/edge_dock_dock.h"
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_client_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

/* ---------------- wire message log toggle (runtime log level DEBUG<->INFO) ---------------- */
extern void ThingModelLog_SetLevel(int level);
#define WIRE_LOG_LEVEL_DEBUG 0
#define WIRE_LOG_LEVEL_INFO  1
static int g_wire_log = 0;
static void wire_log_toggle(void)
{
    g_wire_log = !g_wire_log;
    ThingModelLog_SetLevel(g_wire_log ? WIRE_LOG_LEVEL_DEBUG : WIRE_LOG_LEVEL_INFO);
    printf("  >> wire message log has been %s\n", g_wire_log ? "enabled (prints full JSON of PUB/RCV)" : "disabled");
}

/* ================= interactive input helpers ================= */
static void ui_read_line(char *buf, size_t cap)
{
    if (!fgets(buf, (int)cap, stdin)) { buf[0] = 0; return; }
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
}
static int ui_ask_int(const char *label, int def)
{
    char b[64]; printf("    %s [default %d]: ", label, def); fflush(stdout);
    ui_read_line(b, sizeof(b));
    return b[0] ? atoi(b) : def;
}
static void ui_ask_str(const char *label, const char *def, char *out, size_t cap)
{
    char b[256]; printf("    %s [default %s]: ", label, def ? def : "(empty)"); fflush(stdout);
    ui_read_line(b, sizeof(b));
    if (b[0]) snprintf(out, cap, "%s", b);
    else      snprintf(out, cap, "%s", def ? def : "");
}
static bool ui_ask_opt_double(const char *label, double *out)
{
    char b[64]; printf("    %s [Enter to skip]: ", label); fflush(stdout);
    ui_read_line(b, sizeof(b));
    if (!b[0]) return false; *out = atof(b); return true;
}
static bool ui_ask_opt_int(const char *label, int *out)
{
    char b[64]; printf("    %s [Enter to skip]: ", label); fflush(stdout);
    ui_read_line(b, sizeof(b));
    if (!b[0]) return false; *out = atoi(b); return true;
}
/* Optional string: Enter = skip, leaving out[0]=0 so the caller can omit the field entirely. */
static void ui_ask_opt_str(const char *label, char *out, size_t cap)
{
    printf("    %s [Enter to skip]: ", label); fflush(stdout);
    ui_read_line(out, cap);
}

/* ---------- parameterized input wrappers ---------- */
static void ui_set_tsl(DockConfig_t *cf)
{
    printf("  set_tsl -- push per property (Enter skips a property; set at least 1).\n");
    printf("  STRICT: only PDF read_write props are settable (home point / alt land / work mode).\n");
    double d; int i;
    if (ui_ask_opt_double("home point longitude drone_home_point_longitude (deg, e.g. 113.95)", &d)) dock_config_tsl_set_home_point_longitude(cf, d);
    if (ui_ask_opt_double("home point latitude  drone_home_point_latitude  (deg, e.g. 22.53)",  &d)) dock_config_tsl_set_home_point_latitude(cf, d);
    if (ui_ask_opt_double("home point height    drone_home_point_height    (m, e.g. 0.1)",      &d)) dock_config_tsl_set_home_point_height(cf, d);
    printf("    [station_work_mode dock work mode] 0=roaming 1=external-dock 2=simple-airport\n");
    if (ui_ask_opt_int   ("station_work_mode", &i)) dock_config_tsl_set_station_work_mode(cf, i);
    if (ui_ask_opt_int   ("third_dock_work_mode third-party dock work mode (0/1)", &i)) dock_config_tsl_set_third_dock_work_mode(cf, i);
    dock_config_set_tsl_params_commit(cf);
}
/* Standalone use case: set ONLY station_work_mode (a.k.a. "remote debug mode"), decoupled from the
 * multi-param set_tsl above. Pushes a single {station_work_mode:v} entry and commits it. */
static void ui_set_station_work_mode(DockConfig_t *cf)
{
    int i = 2;
    printf("  set station_work_mode (remote debug mode) -- pushes only this one property.\n");
    printf("    0=roaming  1=external-dock  2=simple-airport\n");
    if (!ui_ask_opt_int("station_work_mode value", &i)) { printf("  (skipped, nothing sent)\n"); return; }
    dock_config_tsl_set_station_work_mode(cf, i);
    dock_config_set_tsl_params_commit(cf);
}
static void ui_set_network_static(DockConfig_t *cf)
{
    char ip[64], nm[64], gw[64], d1[64], d2[64];
    printf("  set_network(static IP) -- each field a dotted-decimal IPv4 string:\n");
    ui_ask_str("static_ip static IP address",   "192.168.1.100", ip, sizeof(ip));
    ui_ask_str("netmask subnet mask",           "255.255.255.0", nm, sizeof(nm));
    ui_ask_str("default_gateway default gateway","192.168.1.1",   gw, sizeof(gw));
    ui_ask_str("pri_dns_ip primary DNS",         "8.8.8.8",       d1, sizeof(d1));
    ui_ask_str("sec_dns_ip secondary DNS",       "8.8.4.4",       d2, sizeof(d2));
    dock_config_set_network_params_static_ex(cf, ip, nm, gw, d1, d2);
}
static void ui_set_sdr(DockConfig_t *cf)
{
    printf("  set_sdr -- [drone_type model] fixed enum: 100=M4TD, 103=M400\n");
    dock_config_set_sdr_firmware_ex(cf, ui_ask_int("drone_type", 100));
}
/* Cloud onboarding. cloud_type decides which of the five fields the dock accepts, so only those
 * are prompted for -- the predicate is the same one dock_config_cloud_onboard() enforces before
 * sending:
 *   0 FlightHub 2       -> organization_id + device_binding_code   (address is DJI's)
 *   1 third-party cloud -> all five fields
 *   2 private cloud     -> mqtt_url + organization_id + device_binding_code
 * Enter skips a field and it is left out of the request. */
static void ui_set_cloud(DockConfig_t *cf)
{
    char url[128] = "", acc[64] = "", pw[64] = "", org[64] = "", code[64] = "";
    printf("  set_cloud -- [cloud_type] fixed enum: 0=FlightHub 2, 1=third-party cloud, 2=FlightHub private cloud\n");
    printf("    fields by type: 0 -> org id + binding code; 1 -> all; 2 -> address + org id + binding code\n");
    int ct = ui_ask_int("cloud_type", 1);
    printf("  cloud_type=%d (%s), asking only the fields this type accepts:\n", ct, dock_config_cloud_type_str(ct));
    if (dock_config_cloud_field_allowed(ct, DOCK_CLOUD_FIELD_MQTT_URL))
        ui_ask_opt_str("mqtt_url MQTT address (format mqtt://host:port)", url, sizeof(url));
    if (dock_config_cloud_field_allowed(ct, DOCK_CLOUD_FIELD_MQTT_ACCOUNT))
        ui_ask_opt_str("mqtt_account MQTT account", acc, sizeof(acc));
    if (dock_config_cloud_field_allowed(ct, DOCK_CLOUD_FIELD_MQTT_PASSWORD))
        ui_ask_opt_str("mqtt_password MQTT password", pw, sizeof(pw));
    if (dock_config_cloud_field_allowed(ct, DOCK_CLOUD_FIELD_ORGANIZATION_ID))
        ui_ask_opt_str("organization_id organization id", org, sizeof(org));
    if (dock_config_cloud_field_allowed(ct, DOCK_CLOUD_FIELD_DEVICE_BINDING_CODE))
        ui_ask_opt_str("device_binding_code binding code", code, sizeof(code));
    dock_config_cloud_onboard(cf, ct, url, acc, pw, org, code);
}
/* Standalone organization bind, for re-binding without touching the cloud parameters. */
static void ui_org_bind(DockConfig_t *cf)
{
    char org[64], code[64];
    printf("  org_bind -- free-form strings (organization info from FlightHub/cloud):\n");
    ui_ask_str("organization_id",       "org-demo-0001",  org,  sizeof(org));
    ui_ask_str("device_binding_code",   "bind-code-demo", code, sizeof(code));
    dock_config_organization_bind_ex(cf, org, code);
}
static void ui_flight_push(Flight_t *fl, int action, const char *cn)
{
    printf("  %s -- Enter = auto-sweep 0->100 progress; or enter a percent for a single custom push.\n", cn);
    char b[64]; printf("    percent (0~100, Enter = auto-sweep): "); fflush(stdout);
    ui_read_line(b, sizeof(b));
    if (!b[0]) {
        switch (action) {
            case 0: flight_push_takeoff_prepare(fl); break;
            case 1: flight_push_takeoff_end(fl);     break;
            case 2: flight_push_landing_prepare(fl); break;
            case 3: flight_push_landing_end(fl);     break;
            case 4: flight_push_abnormal_recovery(fl); break;
        }
        return;
    }
    int pct = atoi(b);
    char st[32];
    printf("    [status] fixed enum: canceled/failed/in_progress/ok/paused/rejected/sent/timeout\n");
    ui_ask_str("status", pct >= 100 ? "ok" : "in_progress", st, sizeof(st));
    flight_push_single(fl, action, st, pct);
}

/* ================= full-reply pretty printer (session listener) ================= */
static void reply_dump_cb(void *user, const char *topic, const char *payload, size_t len)
{
    (void)user;
    if (!topic || !payload) return;
    if (!strstr(topic, "/requests_reply") &&
        !strstr(topic, "/services_reply") &&
        !strstr(topic, "/state_reply")) return;
    ThingModelJsonObj_t *root = ThingModelJson_ParseN(payload, len);
    if (!root) return;
    const char *method = ThingModelJson_LookupString(root, "method", "(state)");
    char *pretty = ThingModelJson_Print(root);
    printf("  --- dock reply: %s (all fields) ---\n", method);
    if (pretty) {
        char *p = pretty;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (nl) { *nl = 0; printf("    %s\n", p); p = nl + 1; }
            else    { printf("    %s\n", p); break; }
        }
        ThingModelMem_Free(pretty);
    }
    ThingModelJson_Free(root);
}

/* ---------------- periodic TSL polling ---------------- */
typedef struct {
    DockConfig_t   *cf;
    pthread_t       th;
    volatile int    running;
    int             started;
    volatile int    quiet;      /* 1 = suppress the get_tsl fetched-data dump each round */
    int             get_only;   /* 1 = only query get_tsl each round (no set_tsl push) */
} TslPoller;

static void *tsl_poll_loop(void *arg)
{
    TslPoller *p = (TslPoller *)arg;
    int round = 0;
    while (p->running) {
        printf("\n[periodicTSL] round %d -> query get_tsl\n", ++round);
        dock_config_get_tsl_params_ex(p->cf, !p->quiet);
        for (int i = 0; i < 20 && p->running; i++) usleep(100 * 1000);
        if (!p->running) break;
        if (p->get_only) continue;   /* get-only mode: skip the set_tsl push */
        printf("[periodicTSL] round %d -> push set_tsl\n", round);
        dock_config_set_tsl_params(p->cf);
        for (int i = 0; i < 20 && p->running; i++) usleep(100 * 1000);
    }
    printf("[periodicTSL] stopped.\n");
    return NULL;
}
static void tsl_poller_stop(TslPoller *p)
{
    if (!p->started) return;
    p->running = 0;
    pthread_join(p->th, NULL);
    p->started = 0;
}
/* One unified start/stop toggle for the periodic poller (menu 13):
 *   running -> stop; idle -> ask mode (get-only / get+set) and whether to print, then start. */
static void tsl_poller_control(TslPoller *p)
{
    if (p->started && p->running) { tsl_poller_stop(p); return; }
    int mode = 0, pr = 0;
    p->get_only = ui_ask_opt_int("mode: 1=get_tsl only, 2=get+set  [Enter=1]", &mode) ? (mode == 2 ? 0 : 1) : 1;
    p->quiet    = ui_ask_opt_int("print fetched data: 1=on, 0=off   [Enter=on]", &pr)  ? (pr == 0 ? 1 : 0) : 0;
    p->running = 1;
    if (pthread_create(&p->th, NULL, tsl_poll_loop, p) != 0) {
        printf("  periodicTSL thread failed to start.\n");
        p->running = 0; p->started = 0; return;
    }
    p->started = 1;
    printf("  periodicTSL started: %s every 2s, data print %s. Press 13 again to stop.\n",
           p->get_only ? "get_tsl only" : "get_tsl / set_tsl alternate",
           p->quiet ? "OFF" : "ON");
}

/* ---------------- dt5 menu ---------------- */
static void dt5_menu(void)
{
    printf(
        "\n===== Third-party dock device (dt5)  [capability auto-reported after connect] =====\n"
        "  (items marked * prompt for each parameter; Enter uses the default)\n"
        "  - Charging and aircraft power on/off -\n"
        "   1  drone_open      aircraft power on\n"
        "   2  drone_close     aircraft power off\n"
        "   3  charge_open     start charging\n"
        "   4  charge_close    stop charging\n"
        "  - Parameters and firmware -\n"
        "   5  get_tsl         query thing-model params\n"
        "   6* set_tsl         push thing-model params (per-property input)\n"
        "  18* set_work_mode   set only station_work_mode (remote debug mode)\n"
        "   7  get_network     query network config\n"
        "   8  set_network     push network config (dynamic IP/DHCP)\n"
        "  15* set_network_st  push network config (static IP, input each field)\n"
        "   9  get_sdr         query SDR firmware (model)\n"
        "  10* set_sdr         push SDR firmware (input drone_type)\n"
        "  11* set_cloud       cloud onboarding (cloud_type decides which fields are asked; also binds the organization)\n"
        "  12* org_bind        organization bind only\n"
        "  13  periodic poll      start/stop toggle (on start asks: mode get-only/get+set, print on/off)\n"
        "  - Takeoff/landing control -\n"
        "  20* takeoff_prepare    push \"takeoff preparation\" progress\n"
        "  21* takeoff_end        push \"takeoff complete\" progress\n"
        "  22* landing_prepare    push \"landing preparation\" progress\n"
        "  23* landing_end        push \"landing complete\" progress\n"
        "  24* abnormal_recovery  push \"abnormal recovery\" progress\n"
        "  25  services selftest inject   replay 5 inbound service requests\n"
        "   w  wire message log toggle\n"
        "   q  quit\n"
        "select> ");
    fflush(stdout);
}

int main(void)
{
    printf("==== dock_sample: third-party dock (dt5) single-device demo ====\n");
    DockSessionOpts base;
    dock_session_default_opts(&base);
    const char *lv = getenv("THING_MODEL_LOG_LEVEL");
    g_wire_log = (lv && strcasecmp(lv, "debug") == 0) ? 1 : 0;

    ThingModelClientSession_t *s = dock_session_connect(&base);
    if (!s) { fprintf(stderr, "connection failed\n"); return 1; }
    int32_t reply_listener = ThingModelClientSession_AddListener(s, reply_dump_cb, NULL);
    auth_report_capability_dt5_sync(s, 5000);

    Charging_t   *ch = charging_create(s);
    DockConfig_t *cf = dock_config_create(s);
    Flight_t     *fl = flight_create(s, dock_session_account(s));
    if (!ch || !cf || !fl) {
        fprintf(stderr, "core initialization failed\n");
    } else {
        TslPoller poller; memset(&poller, 0, sizeof(poller)); poller.cf = cf;
        char line[32];
        for (;;) {
            dt5_menu();
            if (!fgets(line, sizeof(line), stdin)) break;
            if (line[strspn(line, " \t\r\n")] == '\0') continue;   /* bare Enter: just reprint the menu */
            if (line[0]=='q' || line[0]=='Q') break;
            if (line[0]=='w' || line[0]=='W') { wire_log_toggle(); continue; }
            switch (atoi(line)) {
                case 1:  charging_drone_open(ch); break;
                case 2:  charging_drone_close(ch); break;
                case 3:  charging_charge_open(ch); break;
                case 4:  charging_charge_close(ch); break;
                case 5:  dock_config_get_tsl_params(cf); break;
                case 6:  ui_set_tsl(cf); break;
                case 18: ui_set_station_work_mode(cf); break;
                case 7:  dock_config_get_network_params(cf); break;
                case 8:  dock_config_set_network_params(cf); break;
                case 15: ui_set_network_static(cf); break;
                case 9:  dock_config_get_sdr_firmware(cf); break;
                case 10: ui_set_sdr(cf); break;
                case 11: ui_set_cloud(cf); break;
                case 12: ui_org_bind(cf); break;
                case 13: tsl_poller_control(&poller); break;
                case 20: ui_flight_push(fl, 0, "takeoff_prepare"); break;
                case 21: ui_flight_push(fl, 1, "takeoff_end"); break;
                case 22: ui_flight_push(fl, 2, "landing_prepare"); break;
                case 23: ui_flight_push(fl, 3, "landing_end"); break;
                case 24: ui_flight_push(fl, 4, "abnormal_recovery"); break;
                case 25: flight_selftest_inject_services(fl); break;
                default: printf("  unknown selection: %s", line); break;
            }
        }
        tsl_poller_stop(&poller);
    }
    charging_destroy(ch);
    dock_config_destroy(cf);
    flight_destroy(fl);
    ThingModelClientSession_RemoveListener(s, reply_listener);
    dock_session_disconnect(s);
    printf("exiting...\n");
    return 0;
}
