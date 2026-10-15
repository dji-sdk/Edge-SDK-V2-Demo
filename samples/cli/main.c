/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * dock_cli —— interactive demo.
 *
 * Two-level menu: first pick the "connecting device identity" (device_type; this CLI drives
 * one at a time), then enter that device's feature submenu. After selecting a device it
 * automatically does "connect by identity -> report the matching capability -> enter submenu",
 * and disconnects on return. Each identity builds its own DockSessionOpts and therefore its own
 * SN/account. dt5 = third-party dock (charging/params/takeoff-landing), dt7 = RID device
 * (ADS-B/RID surveillance).
 */
#include "edge_dock/edge_dock.h"
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_client_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

/* ---------------- wire message log toggle ----------------
 * When on, the runtime prints the full JSON on every send/receive (PUB=send / RCV=receive).
 * Under the hood this just switches the runtime log level to DEBUG, and back to INFO when off;
 * it does not modify the runtime.
 * ThingModelLog_SetLevel is declared in a runtime private header, so here we just hand-write
 * an extern to use it (the symbol lives in the thing_model_client_runtime static library;
 * levels 0=DEBUG 1=INFO 2=WARN 3=ERROR). */
extern void ThingModelLog_SetLevel(int level);
#define WIRE_LOG_LEVEL_DEBUG 0
#define WIRE_LOG_LEVEL_INFO  1

static int g_wire_log = 0;

static void wire_log_apply(void)
{
    ThingModelLog_SetLevel(g_wire_log ? WIRE_LOG_LEVEL_DEBUG : WIRE_LOG_LEVEL_INFO);
}
static void wire_log_toggle(void)
{
    g_wire_log = !g_wire_log;
    wire_log_apply();
    printf("  >> wire message log has been %s%s\n",
           g_wire_log ? "enabled" : "disabled",
           g_wire_log ? " (will now print the full JSON of PUB=send / RCV=receive)" : "");
}

/* ================= interactive input helpers =================
 * Convention: each parameter prompt shows "meaning + allowed values/format + [default X]";
 * pressing Enter keeps the default. Reads one line from stdin (trailing newline stripped);
 * on EOF the default is used. */
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
static double ui_ask_double(const char *label, double def)
{
    char b[64]; printf("    %s [default %g]: ", label, def); fflush(stdout);
    ui_read_line(b, sizeof(b));
    return b[0] ? atof(b) : def;
}
static void ui_ask_str(const char *label, const char *def, char *out, size_t cap)
{
    char b[256]; printf("    %s [default %s]: ", label, def ? def : "(empty)"); fflush(stdout);
    ui_read_line(b, sizeof(b));
    if (b[0]) snprintf(out, cap, "%s", b);
    else      snprintf(out, cap, "%s", def ? def : "");
}
/* Optional set (for per-property set_tsl): Enter = skip (do not set that property), returns whether a value was entered. */
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

/* ---------- parameterized input wrappers for each test case ---------- */
static void ui_set_tsl(DockConfig_t *cf)
{
    printf("  set_tsl -- push per property (Enter skips a property; fill only what you want, set at least 1).\n"
           "  STRICT: only PDF read_write props are settable (home point / alt land / work mode).\n");
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
    printf("  set station_work_mode (remote debug mode) -- pushes only this one property.\n"
           "    0=roaming  1=external-dock  2=simple-airport\n");
    if (!ui_ask_opt_int("station_work_mode value", &i)) { printf("  (skipped, nothing sent)\n"); return; }
    dock_config_tsl_set_station_work_mode(cf, i);
    dock_config_set_tsl_params_commit(cf);
}
/* Standalone use case: set ONLY third_dock_work_mode, decoupled from the multi-param
 * set_tsl above. Pushes a single {third_dock_work_mode:v} entry and commits it. */
static void ui_set_third_dock_work_mode(DockConfig_t *cf)
{
    int i = 1;
    printf("  set third_dock_work_mode -- pushes only this one property.\n"
           "    0=off  1=integrating a third-party dock\n");
    if (!ui_ask_opt_int("third_dock_work_mode value", &i)) { printf("  (skipped, nothing sent)\n"); return; }
    dock_config_tsl_set_third_dock_work_mode(cf, i);
    dock_config_set_tsl_params_commit(cf);
}
static void ui_set_network_static(DockConfig_t *cf)
{
    char ip[64], nm[64], gw[64], d1[64], d2[64];
    printf("  set_network(static IP) -- each field is a dotted-decimal IPv4 string (e.g. 192.168.1.100):\n");
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
 * sending, so the prompts can never drift from what goes on the wire:
 *   0 FlightHub 2       -> organization_id + device_binding_code   (address is DJI's)
 *   1 third-party cloud -> all five fields
 *   2 private cloud     -> mqtt_url + organization_id + device_binding_code
 * Enter skips a field and it is left out of the request. One run sends set_cloud_params, then
 * airport_organization_bind when an org id / binding code was entered. */
static void ui_set_cloud(DockConfig_t *cf)
{
    char url[128] = "", acc[64] = "", pw[64] = "", org[64] = "", code[64] = "";
    printf("  set_cloud -- [cloud_type] fixed enum: 0=FlightHub 2, 1=third-party cloud, 2=FlightHub private cloud\n"
           "    fields by type: 0 -> org id + binding code; 1 -> all; 2 -> address + org id + binding code\n");
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
static void ui_report_rid(Surveillance_t *sv)
{
    printf("  rid_infos single -- key fields (the other ~12 fields keep their demo defaults):\n");
    double lat = ui_ask_double("lat latitude (deg)",           22.53854);
    double lon = ui_ask_double("lon longitude (deg)",          113.95749);
    int    h   = ui_ask_int   ("height above takeoff point (m)", 50);
    double sp  = ui_ask_double("speed horizontal (m/s)",       5.4);
    int    hd  = ui_ask_int   ("heading (deg 0~359)",          12);
    surveillance_report_rid_ex(sv, lat, lon, h, sp, hd);
}
static void ui_report_adsb(Surveillance_t *sv)
{
    char icao[32], cs[32];
    printf("  adsb_infos single -- key fields (the other fields keep their demo defaults):\n");
    ui_ask_str("icao 24-bit id (hex string)", "781EB7",  icao, sizeof(icao));
    ui_ask_str("callsign",                    "CSN1735", cs,   sizeof(cs));
    double lat = ui_ask_double("lat latitude (deg)",  22.53450);
    double lon = ui_ask_double("lon longitude (deg)", 113.95360);
    int    h   = ui_ask_int   ("height MSL (m)",      1000);
    /* The wire unit is km/h, but FlightHub 2 divides by 3.6 and labels the result m/s -- 941 goes up
     * and 261.4 comes out on screen. State the conversion at the prompt and echo the converted value
     * back, so nobody is left wondering why the UI shows a different number than they typed. */
    double sp  = ui_ask_double("speed (km/h on the wire; FlightHub 2 shows it /3.6 as m/s)", 941);
    printf("      -> FlightHub 2 will display %.1f m/s\n", sp / 3.6);
    int    hd  = ui_ask_int   ("heading (deg)",       185);
    surveillance_report_adsb_ex(sv, icao, cs, lat, lon, h, sp, hd);
}

/* ================= full-reply pretty printer =================
 * Registered as a session listener. Whenever the dock replies on a
 * <...>/requests_reply, <...>/services_reply or <...>/state_reply topic, parse the
 * payload and print the COMPLETE field tree (indented JSON), so every field the dock
 * returned is shown -- not just result/status. This is generic across all methods and
 * also surfaces fields the decoded C structs drop (result, error_reason, bid/tid/
 * timestamp/gateway, nested output.*).
 *
 * This runs on the SDK's MQTT receive thread, NOT the main/poll thread, so it races with
 * whoever is printing a use case's result. glibc locks stdout per stdio call only, so the
 * previous line-by-line version interleaved with the multi-line dumps under src/dock --
 * on real hardware JSON lines landed in the middle of get_tsl's param list. Header + whole
 * tree now go out in a SINGLE printf, which is atomic against the other thread's printf.
 * That costs the extra 4-space indent this used to add to each line; ThingModelJson_Print
 * already returns indented JSON, so the tree stays readable without it. */
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
    char *pretty = ThingModelJson_Print(root);   /* indented JSON */
    printf("  --- dock reply: %s (all fields) ---\n%s\n", method, pretty ? pretty : "(print failed)");
    if (pretty) ThingModelMem_Free(pretty);
    ThingModelJson_Free(root);
}

/* ---------------- broker refusal monitor ----------------
 * The gateway announces every ACL refusal on thing/product/<account>/error, and nothing subscribes
 * to it by default. That is why a publish to a topic this account may not write "succeeds" —
 * Publish() returns 0, the PUBACK comes back — and then silently goes nowhere, and why an uplink
 * over the 5 Hz cap just times out after 5s with no reason anywhere. Attaching a DockDiag_t turns
 * both into a printed, decoded line.
 *
 * Always on, no menu item: the point is to see the refusal at the moment it happens, while you are
 * exercising some other feature. Note /error only ever reports THIS account's own violations, so a
 * monitor that sits idle sees nothing by design.
 *
 * Runs on the MQTT receive thread, so it emits exactly ONE output call — same rule as
 * reply_dump_cb above. */
static void diag_print_cb(void *user, const DockDiagEvent_t *ev)
{
    (void)user;
    char line[640];
    int n = dock_diag_format(ev, line, sizeof(line));
    if (n > 0 && (size_t)n < sizeof(line)) {
        snprintf(line + n, sizeof(line) - n, "\n     fix: %s", dock_diag_error_hint(ev->error_code));
    }
    printf("\n  %s\n", line);
}

/* Attach the monitor to a freshly connected session. Returns NULL if it could not be set up, which
 * is not fatal — it only means refusals stay invisible, exactly as they were before. */
static DockDiag_t *diag_monitor_start(ThingModelClientSession_t *s)
{
    DockDiag_t *d = dock_diag_create(s);
    if (!d) return NULL;
    dock_diag_set_callback(d, diag_print_cb, NULL);
    if (dock_diag_start(d, 1) != THING_MODEL_RC_OK) {
        printf("  (broker refusal monitor could not subscribe; ACL errors will stay silent)\n");
    }
    return d;
}

/* ---------------- periodic TSL polling (mirrors the p loop in the raspberry_pi sample) ----------------
 * A background thread alternately sends get_tsl / set_tsl every 2 seconds, simulating a third
 * party's periodic collection + push of dock parameters.
 * Reuses dock_config_get/set_tsl_params, no new protocol added. */
typedef struct {
    DockConfig_t   *cf;
    pthread_t       th;
    volatile int    running;   /* 1=running, 0=stopped */
    int             started;   /* whether the thread has been created (needs join) */
    volatile int    quiet;     /* 1 = suppress the get_tsl fetched-data dump each round */
    int             get_only;  /* 1 = only query get_tsl each round (no set_tsl push) */
} TslPoller;

static void *tsl_poll_loop(void *arg)
{
    TslPoller *p = (TslPoller *)arg;
    int round = 0;
    while (p->running) {
        printf("\n[periodicTSL] round %d -> query get_tsl\n", ++round);
        dock_config_get_tsl_params_ex(p->cf, !p->quiet);
        /* split into 20x100ms, so pressing 13 again can stop it quickly */
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

/* ---------------- dt5: third-party dock device ---------------- */
static void dt5_menu(void)
{
    printf(
        "\n===== Third-party dock device (dt5)  [capability auto-reported and ack'd after connect] =====\n"
        "  (items marked * prompt for each parameter; Enter uses the default)\n"
        "  - Charging and aircraft power on/off (third party -> dock, done once ack received) -\n"
        "   1  drone_open      aircraft power on\n"
        "   2  drone_close     aircraft power off\n"
        "   3  charge_open     start charging\n"
        "   4  charge_close    stop charging\n"
        "  - Parameters and firmware (query / push dock parameters) -\n"
        "   5  get_tsl         query thing-model params (work mode / in-dock / charge state, etc.)\n"
        "   6* set_tsl         push thing-model params (per-property input)\n"
        "   7* set_work_mode   set only station_work_mode (remote debug mode)\n"
        "   8* set_3rd_work_mode  set only third_dock_work_mode\n"
        "   9  get_network     query network config\n"
        "  10  set_network     push network config (dynamic IP/DHCP, no params)\n"
        "  11* set_network_st  push network config (static IP, input each field)\n"
        "  12  get_sdr         query SDR firmware (model)\n"
        "  13* set_sdr         push SDR firmware (input drone_type)\n"
        "  14* set_cloud       cloud onboarding (cloud_type decides which fields are asked; also binds the organization)\n"
        "  15* org_bind        organization bind only (input org_id/binding code)\n"
        "  16  periodic poll      start/stop toggle (on start asks: mode get-only/get+set, print on/off)\n"
        "  - Takeoff/landing control (third party pushes progress; Enter = auto 0->100, or input percent+status) -\n"
        "  17* takeoff_prepare    push \"takeoff preparation\" progress\n"
        "  18* takeoff_end        push \"takeoff complete\" progress\n"
        "  19* landing_prepare    push \"landing preparation\" progress\n"
        "  20* landing_end        push \"landing complete\" progress\n"
        "  21* abnormal_recovery  push \"abnormal recovery\" progress\n"
        "  22  services selftest inject   replay 5 inbound service requests (local mock broker)\n"
        "   b  back to previous level (device selection)\n"
        "select> ");
    fflush(stdout);
}

/* Each identity builds its OWN opts: dt5 and dt7 are two distinct devices with distinct SNs, so
 * dt7 must not inherit a dt5-shaped base. */
static void run_dt5_dock(void)
{
    DockSessionOpts opts;
    dock_session_default_opts(&opts);            /* dt5 identity: DOCK_DEVICE_SN, no suffix */
    ThingModelClientSession_t *s = dock_session_connect(&opts);
    if (!s) { fprintf(stderr, "connection failed\n"); return; }
    /* Print every field of every reply (requests_reply/services_reply/state_reply). */
    int32_t reply_listener = ThingModelClientSession_AddListener(s, reply_dump_cb, NULL);
    /* Watch the broker's refusal channel, so an ACL rejection or a rate-limit trip shows up as a
     * decoded line instead of an unexplained timeout. */
    DockDiag_t *diag = diag_monitor_start(s);
    /* Report the capability set and WAIT for the acknowledgement. A real O4 does answer on
     * state_reply, and that reply is the only place a refusal ever shows up: the publish succeeds
     * either way, so a fire-and-forget report would hide e.g. result=201 "Device type slot is full"
     * and leave the device unregistered — with every sdr/network/charging reply silently dropped
     * from then on. The subscription this leaves behind also keeps post-reconnect acknowledgements
     * visible through reply_dump_cb above. */
    auth_report_capability_dt5_sync(s, 5000);

    Charging_t   *ch = charging_create(s);
    DockConfig_t *cf = dock_config_create(s);
    /* flight's services subscription topic owner must be the account the broker recognizes
     * (= dock_session_account(s), i.e. app_id"_esdk_"device_sn), not the bare device_sn,
     * otherwise the broker replies "client ID in the topic does not match". */
    Flight_t     *fl = flight_create(s, dock_session_account(s));
    if (!ch || !cf || !fl) {
        fprintf(stderr, "core initialization failed\n");
    } else {
        TslPoller poller;
        memset(&poller, 0, sizeof(poller));
        poller.cf = cf;
        char line[32];
        for (;;) {
            dt5_menu();
            if (!fgets(line, sizeof(line), stdin)) break;
            if (line[strspn(line, " \t\r\n")] == '\0') continue;   /* bare Enter: just reprint the menu */
            if (line[0]=='b' || line[0]=='B') break;
            switch (atoi(line)) {
                case 1:  charging_drone_open(ch); break;
                case 2:  charging_drone_close(ch); break;
                case 3:  charging_charge_open(ch); break;
                case 4:  charging_charge_close(ch); break;
                case 5:  dock_config_get_tsl_params(cf); break;
                case 6:  ui_set_tsl(cf); break;
                case 7:  ui_set_station_work_mode(cf); break;
                case 8:  ui_set_third_dock_work_mode(cf); break;
                case 9:  dock_config_get_network_params(cf); break;
                case 10: dock_config_set_network_params(cf); break;
                case 11: ui_set_network_static(cf); break;
                case 12: dock_config_get_sdr_firmware(cf); break;
                case 13: ui_set_sdr(cf); break;
                case 14: ui_set_cloud(cf); break;
                case 15: ui_org_bind(cf); break;
                case 16: tsl_poller_control(&poller); break;
                case 17: ui_flight_push(fl, 0, "takeoff_prepare"); break;
                case 18: ui_flight_push(fl, 1, "takeoff_end"); break;
                case 19: ui_flight_push(fl, 2, "landing_prepare"); break;
                case 20: ui_flight_push(fl, 3, "landing_end"); break;
                case 21: ui_flight_push(fl, 4, "abnormal_recovery"); break;
                case 22: flight_selftest_inject_services(fl); break;
                default: printf("  unknown selection: %s", line); break;
            }
        }
        tsl_poller_stop(&poller);   /* stop the background thread before destroying cf */
    }
    charging_destroy(ch);
    dock_config_destroy(cf);
    flight_destroy(fl);
    dock_diag_destroy(diag);
    ThingModelClientSession_RemoveListener(s, reply_listener);
    dock_session_disconnect(s);
}

/* ---------------- dt7: RID device ---------------- */
static void dt7_menu(void)
{
    printf(
        "\n===== RID device (dt7)  [capability rid+adsb auto-reported and ack'd after connect] =====\n"
        "  (items marked * prompt for each parameter; Enter uses the default)\n"
        "  30* rid_infos        send a single target (input lat/lon/height/speed/heading)\n"
        "  31* adsb_infos       send a single target (input icao/callsign/lat/lon, etc.)\n"
        "  32  start_periodic   batch flush every 2s (obeys 5Hz rate limit)\n"
        "  33  stop_periodic    stop periodic reporting\n"
        "   b  back to device selection\n"
        "select> ");
    fflush(stdout);
}

static void run_dt7_rid(void)
{
    DockSessionOpts opts;
    dock_session_default_opts_rid(&opts);        /* dt7 identity: DOCK_RID_DEVICE_SN + "@rid" */
    ThingModelClientSession_t *s = dock_session_connect(&opts);
    if (!s) { fprintf(stderr, "connection failed\n"); return; }
    int32_t reply_listener = ThingModelClientSession_AddListener(s, reply_dump_cb, NULL);
    DockDiag_t *diag = diag_monitor_start(s);
    auth_report_capability_dt7_sync(s, 5000);

    Surveillance_t *sv = surveillance_create(s);
    if (!sv) {
        fprintf(stderr, "core initialization failed\n");
    } else {
        char line[32];
        for (;;) {
            dt7_menu();
            if (!fgets(line, sizeof(line), stdin)) break;
            if (line[strspn(line, " \t\r\n")] == '\0') continue;   /* bare Enter: just reprint the menu */
            if (line[0]=='b' || line[0]=='B') break;
            switch (atoi(line)) {
                case 30: ui_report_rid(sv); break;
                case 31: ui_report_adsb(sv); break;
                case 32: surveillance_start_periodic(sv); break;
                case 33: surveillance_stop_periodic(sv); break;
                default: printf("  unknown selection: %s", line); break;
            }
        }
    }
    surveillance_destroy(sv);   /* internally calls stop_periodic first */
    dock_diag_destroy(diag);
    ThingModelClientSession_RemoveListener(s, reply_listener);
    dock_session_disconnect(s);
}

/* ---------------- top level: device identity selection ---------------- */
int main(void)
{
    printf("==== dock_cli: third-party device connection sample ====\n");

    /* At startup, probe the initial wire-toggle value from the THING_MODEL_LOG_LEVEL env var
     * (so the menu can show its state). */
    const char *lv = getenv("THING_MODEL_LOG_LEVEL");
    g_wire_log = (lv && strcasecmp(lv, "debug") == 0) ? 1 : 0;

    char line[32];
    for (;;) {
        printf(
            "\n===== Select connecting device identity (this CLI connects one at a time) =====\n"
            "   1  third-party dock device (dt5)\n"
            "   2  RID device              (dt7)\n"
            "   w  wire message log toggle (current: %s)\n"
            "   q  quit\n"
            "select> ", g_wire_log ? "on" : "off");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (line[strspn(line, " \t\r\n")] == '\0') continue;   /* bare Enter: just reprint the menu */
        if (line[0]=='q' || line[0]=='Q') break;
        if (line[0]=='w' || line[0]=='W') { wire_log_toggle(); continue; }
        switch (atoi(line)) {
            case 1: run_dt5_dock(); break;
            case 2: run_dt7_rid();  break;
            default: printf("  unknown selection: %s", line); break;
        }
    }
    printf("exiting...\n");
    return 0;
}
