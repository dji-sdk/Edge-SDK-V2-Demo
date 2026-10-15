/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * rid_sample —— single-device demo for the RID device (dt7) ONLY.
 *
 * This is the starting point for an integrator that only ships the RID device and does
 * NOT need the third-party dock. It includes only "edge_dock/edge_dock_rid.h" and uses
 * only the surveillance domain (ADS-B / RID). Build with -DEDGE_DOCK_ENABLE_DOCK=OFF to
 * exclude the dock charging/config/flight code (and its generated protocol) entirely.
 *
 * Flow: connect (with the dt7 identity: own SN + "@rid") -> auto report dt7 capability -> menu -> disconnect.
 */
#include "edge_dock/edge_dock_rid.h"
#include "thing_model_client/thing_model_client_codec.h"
#include "thing_model_client/thing_model_client_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
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
    double sp  = ui_ask_double("speed (km/h)",        941);
    int    hd  = ui_ask_int   ("heading (deg)",       185);
    surveillance_report_adsb_ex(sv, icao, cs, lat, lon, h, sp, hd);
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
    printf("  --- reply: %s (all fields) ---\n", method);
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

/* ---------------- dt7 menu ---------------- */
static void dt7_menu(void)
{
    printf(
        "\n===== RID device (dt7)  [capability rid+adsb auto-reported after connect] =====\n"
        "  (items marked * prompt for each parameter; Enter uses the default)\n"
        "  30* rid_infos        send a single target (input lat/lon/height/speed/heading)\n"
        "  31* adsb_infos       send a single target (input icao/callsign/lat/lon, etc.)\n"
        "  32  start_periodic   batch flush every 2s (obeys 5Hz rate limit)\n"
        "  33  stop_periodic    stop periodic reporting\n"
        "   w  wire message log toggle\n"
        "   q  quit\n"
        "select> ");
    fflush(stdout);
}

int main(void)
{
    printf("==== rid_sample: RID device (dt7) single-device demo ====\n");
    DockSessionOpts base;
    /* dt7 identity: own SN (DOCK_RID_DEVICE_SN, distinct from the dt5 dock's) + "@rid" suffix. */
    dock_session_default_opts_rid(&base);
    const char *lv = getenv("THING_MODEL_LOG_LEVEL");
    g_wire_log = (lv && strcasecmp(lv, "debug") == 0) ? 1 : 0;

    ThingModelClientSession_t *s = dock_session_connect(&base);
    if (!s) { fprintf(stderr, "connection failed\n"); return 1; }
    int32_t reply_listener = ThingModelClientSession_AddListener(s, reply_dump_cb, NULL);
    auth_report_capability_dt7_sync(s, 5000);

    Surveillance_t *sv = surveillance_create(s);
    if (!sv) {
        fprintf(stderr, "core initialization failed\n");
    } else {
        char line[32];
        for (;;) {
            dt7_menu();
            if (!fgets(line, sizeof(line), stdin)) break;
            if (line[0]=='q' || line[0]=='Q') break;
            if (line[0]=='w' || line[0]=='W') { wire_log_toggle(); continue; }
            switch (atoi(line)) {
                case 30: ui_report_rid(sv); break;
                case 31: ui_report_adsb(sv); break;
                case 32: surveillance_start_periodic(sv); break;
                case 33: surveillance_stop_periodic(sv); break;
                default: printf("  unknown selection: %s", line); break;
            }
        }
    }
    surveillance_destroy(sv);
    ThingModelClientSession_RemoveListener(s, reply_listener);
    dock_session_disconnect(s);
    printf("exiting...\n");
    return 0;
}
