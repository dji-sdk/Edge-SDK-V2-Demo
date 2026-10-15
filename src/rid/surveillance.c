/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain (5): ADS-B / RID surveillance -- SurveillanceDockPublisher_*.
 *
 * Reporting modes (aligned with the production recommendation):
 *   - Single-shot: surveillance_report_rid/adsb, reports one demo target at a time (for debug/self-test).
 *   - Periodic: surveillance_start/stop_periodic, a background thread every 2s flushes "the targets
 *     cached this cycle" as "one" batched message (one each for rid/adsb; targets go into the
 *     data.rid[]/data.adsb[] arrays).
 *
 * Why batch rather than report per target: DJI docks rate-limit uplink messages to 5Hz, so detecting
 * and reporting one at a time gets throttled and dropped. The protocol recommends caching detected
 * targets and flushing a batch every 2s. This sample has no real radar, so when the cache is empty
 * the periodic thread falls back to resending the last single-shot target (the demo one until the
 * menu sends something else), ensuring continuous output after a single key press.
 */
#include "edge_dock/surveillance.h"
#include "surveillance/surveillance_dock_publisher.h"
#include "surveillance/surveillance_types.h"
#include "thing_model_client/thing_model_client_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Periodic reporting interval: protocol recommends 2s (aligned with the dock's 5Hz rate limit). */
#define SURV_REPORT_PERIOD_MS 2000
/* Per-cycle cache target cap: 5Hz*2s=10, with headroom. */
#define SURV_CACHE_MAX 64
/* icao / callsign copy buffers (protocol caps both well below this). */
#define SURV_STR_MAX 32

struct Surveillance {
    SurveillanceDockPublisher_t *pub;
    void         *mutex;             /* protects the caches and the last_* targets below */
    void         *thread;            /* periodic reporting thread handle */
    volatile bool running;           /* thread running flag */
    T_RidTarget   rid_cache[SURV_CACHE_MAX];
    size_t        rid_count;
    T_AdsbTarget  adsb_cache[SURV_CACHE_MAX];
    size_t        adsb_count;
    /* Most recent single-shot target, used by the periodic empty-cache fallback instead of a
     * hard-coded demo. Without this, a value typed into the menu is on the wire for one message
     * and then overwritten by the demo 2s later -- which reads as "the input had no effect".
     * Seeded with the demo targets so the fallback behaves as before until something is sent. */
    T_RidTarget   last_rid;
    T_AdsbTarget  last_adsb;
    char          last_icao[SURV_STR_MAX];      /* last_adsb.icao / .callsign point here: the */
    char          last_callsign[SURV_STR_MAX];  /* caller's strings are usually on its stack.  */
};

/* Device's own status in the report (battery/power/GNSS) -- values identical to the hand-written sample_esdk2 make_demo_status. */
static void fill_gnss(T_Gnss *g)
{
    g->engine=0; g->flag=2; g->time="20250723-142355";
    g->lat=22.53450; g->lon=113.95360; g->alt=503; g->satellite=5; g->hdop=5;
}

/* ---- demo target construction: values essentially identical to the hand-written sample_esdk2
 *      rid_report_demo_rid/adsb, only the ADS-B height is changed from 11003(11km) to 1000(1km)
 *      to demonstrate a low-altitude target ---- */
static T_RidTarget demo_rid_target(void)
{
    T_RidTarget t; memset(&t, 0, sizeof(t));
    t.osid="1581F6N8C243L0039TB9"; t.rssi=-79; t.fre=1; t.uatype=2; t.status=0;
    t.heading=12; t.speed=5.4; t.uprate=1.5; t.lon=113.95749; t.lat=22.53854;
    t.altgeo=120; t.altbaro=120; t.height=50;
    t.op_lon=113.95856; t.op_lat=22.53980; t.op_alt=20; t.uatime=174462011;
    return t;
}
static T_AdsbTarget demo_adsb_target(void)
{
    T_AdsbTarget t; memset(&t, 0, sizeof(t));
    t.icao="781EB7"; t.callsign="CSN1735"; t.height=1000;   /* altitude (meters), 1km */
    /* ADS-B speed is km/h, unlike the RID target's m/s -- 941 is a typical airliner cruise speed. */
    t.lat=22.53450; t.lon=113.95360; t.speed=941; t.heading=185; t.uprate=0.32;
    return t;
}

/* ---- remember the last single-shot target so the periodic fallback can resend it ---- */
static void remember_rid(Surveillance_t *s, const T_RidTarget *t)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->mutex && o) o->mutex_lock(s->mutex);
    s->last_rid = *t;   /* osid is always the demo string literal -- no override, so no copy needed */
    if (s->mutex && o) o->mutex_unlock(s->mutex);
}
static void remember_adsb(Surveillance_t *s, const T_AdsbTarget *t)
{
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->mutex && o) o->mutex_lock(s->mutex);
    s->last_adsb = *t;
    snprintf(s->last_icao,     sizeof(s->last_icao),     "%s", t->icao     ? t->icao     : "");
    snprintf(s->last_callsign, sizeof(s->last_callsign), "%s", t->callsign ? t->callsign : "");
    s->last_adsb.icao     = s->last_icao;
    s->last_adsb.callsign = s->last_callsign;
    if (s->mutex && o) o->mutex_unlock(s->mutex);
}

/* ---- batch reporting: flush n targets as "one" event ---- */
static void report_rid_batch(Surveillance_t *s, const T_RidTarget *arr, size_t n)
{
    T_RidInfos msg; memset(&msg, 0, sizeof(msg));
    msg.capacity_percent=48; msg.power_supply=0; fill_gnss(&msg.gnss);
    msg.rid=(T_RidTarget *)arr; msg.rid_count=n;
    T_ThingModelReturnCode rc = SurveillanceDockPublisher_PublishRidInfosEvent(s->pub, &msg);
    printf("  <report rid_infos %zu targets> rc=%d\n", n, rc);
}
static void report_adsb_batch(Surveillance_t *s, const T_AdsbTarget *arr, size_t n)
{
    T_AdsbInfos msg; memset(&msg, 0, sizeof(msg));
    msg.capacity_percent=48; msg.power_supply=0; fill_gnss(&msg.gnss);
    msg.adsb=(T_AdsbTarget *)arr; msg.adsb_count=n;
    T_ThingModelReturnCode rc = SurveillanceDockPublisher_PublishAdsbInfosEvent(s->pub, &msg);
    printf("  <report adsb_infos %zu targets> rc=%d\n", n, rc);
}

Surveillance_t *surveillance_create(ThingModelClientSession_t *session)
{
    if (!session) return NULL;
    Surveillance_t *s = (Surveillance_t *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->pub = SurveillanceDockPublisher_Create(session);
    if (!s->pub) { free(s); return NULL; }
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (o && o->mutex_create) s->mutex = o->mutex_create();
    T_RidTarget  rt = demo_rid_target();
    T_AdsbTarget at = demo_adsb_target();
    remember_rid(s, &rt);
    remember_adsb(s, &at);
    return s;
}
void surveillance_destroy(Surveillance_t *s)
{
    if (!s) return;
    surveillance_stop_periodic(s);   /* stop the thread before tearing down resources */
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->mutex && o && o->mutex_destroy) o->mutex_destroy(s->mutex);
    if (s->pub) SurveillanceDockPublisher_Destroy(s->pub);
    free(s);
}

/* ---- single-shot (manually triggered from the menu, one demo target at a time) ----
 * Each of these also becomes the periodic thread's empty-cache fallback: whatever you sent last
 * is what keeps being reported, instead of the demo overwriting it on the next cycle. */
void surveillance_report_rid(Surveillance_t *s)
{
    if (!s) return;
    T_RidTarget t = demo_rid_target();
    remember_rid(s, &t);
    report_rid_batch(s, &t, 1);
}
void surveillance_report_adsb(Surveillance_t *s)
{
    if (!s) return;
    T_AdsbTarget t = demo_adsb_target();
    remember_adsb(s, &t);
    report_adsb_batch(s, &t, 1);
}

/* ---- single-shot, parameterized: start from the demo target, override key fields, then report ---- */
void surveillance_report_rid_ex(Surveillance_t *s,
        double lat, double lon, int height, double speed, int heading)
{
    if (!s) return;
    T_RidTarget t = demo_rid_target();
    t.lat = lat; t.lon = lon; t.height = height; t.speed = speed; t.heading = heading;
    remember_rid(s, &t);
    report_rid_batch(s, &t, 1);
}
void surveillance_report_adsb_ex(Surveillance_t *s,
        const char *icao, const char *callsign,
        double lat, double lon, int height, double speed, int heading)
{
    if (!s) return;
    T_AdsbTarget t = demo_adsb_target();
    if (icao && icao[0])         t.icao = (char *)icao;
    if (callsign && callsign[0]) t.callsign = (char *)callsign;
    t.lat = lat; t.lon = lon; t.height = height; t.speed = speed; t.heading = heading;
    remember_adsb(s, &t);
    report_adsb_batch(s, &t, 1);
}

/* ---- cache targets (in production called by the radar detection callback; this sample has no real radar) ---- */
void surveillance_cache_rid_target(Surveillance_t *s, const void *rid_target)
{
    if (!s || !rid_target) return;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->mutex && o) o->mutex_lock(s->mutex);
    if (s->rid_count < SURV_CACHE_MAX) s->rid_cache[s->rid_count++] = *(const T_RidTarget *)rid_target;
    if (s->mutex && o) o->mutex_unlock(s->mutex);
}
void surveillance_cache_adsb_target(Surveillance_t *s, const void *adsb_target)
{
    if (!s || !adsb_target) return;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->mutex && o) o->mutex_lock(s->mutex);
    if (s->adsb_count < SURV_CACHE_MAX) s->adsb_cache[s->adsb_count++] = *(const T_AdsbTarget *)adsb_target;
    if (s->mutex && o) o->mutex_unlock(s->mutex);
}

/* ---- periodic reporting thread: every 2s swap out the cache and flush a batch; fall back to demo if empty ---- */
static void surv_report_loop(void *arg)
{
    Surveillance_t *s = (Surveillance_t *)arg;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    while (s->running) {
        if (o && o->sleep_ms) o->sleep_ms(SURV_REPORT_PERIOD_MS);
        if (!s->running) break;

        T_RidTarget  rid_batch[SURV_CACHE_MAX];  size_t rn;
        T_AdsbTarget adsb_batch[SURV_CACHE_MAX]; size_t an;
        if (s->mutex && o) o->mutex_lock(s->mutex);
        rn = s->rid_count;  if (rn) memcpy(rid_batch,  s->rid_cache,  rn * sizeof(T_RidTarget));  s->rid_count = 0;
        an = s->adsb_count; if (an) memcpy(adsb_batch, s->adsb_cache, an * sizeof(T_AdsbTarget)); s->adsb_count = 0;
        if (s->mutex && o) o->mutex_unlock(s->mutex);

        /* No real targets this cycle: fall back to resending the last single-shot target (seeded
         * with the demo one at create), so a single key press produces continuous output AND a
         * value typed into the menu keeps being reported instead of being overwritten next cycle.
         * Real cached targets take the batch branch below. */
        if (rn == 0 && an == 0) {
            T_RidTarget  rt;
            T_AdsbTarget at;
            char icao[SURV_STR_MAX], callsign[SURV_STR_MAX];
            if (s->mutex && o) o->mutex_lock(s->mutex);
            rt = s->last_rid;
            at = s->last_adsb;
            snprintf(icao,     sizeof(icao),     "%s", s->last_icao);
            snprintf(callsign, sizeof(callsign), "%s", s->last_callsign);
            if (s->mutex && o) o->mutex_unlock(s->mutex);
            /* re-aim at our own stack copies: s->last_icao may be rewritten while we publish */
            at.icao = icao; at.callsign = callsign;
            report_rid_batch(s, &rt, 1);
            report_adsb_batch(s, &at, 1);
            continue;
        }
        if (rn) report_rid_batch(s, rid_batch, rn);
        if (an) report_adsb_batch(s, adsb_batch, an);
    }
}

void surveillance_start_periodic(Surveillance_t *s)
{
    if (!s || s->running) { if (s && s->running) printf("  periodic reporting already running\n"); return; }
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (!o || !o->thread_create) { printf("  OSAL has no thread implementation, cannot report periodically\n"); return; }
    s->running = true;
    s->thread = o->thread_create(surv_report_loop, s, "surv_report", 4096);
    if (!s->thread) { s->running = false; printf("  failed to create periodic reporting thread\n"); return; }
    printf("  periodic reporting started (batch flush every %dms)\n", SURV_REPORT_PERIOD_MS);
}
void surveillance_stop_periodic(Surveillance_t *s)
{
    if (!s || !s->running) return;
    s->running = false;
    const T_ThingModelOsal *o = ThingModelOsal_Get();
    if (s->thread && o && o->thread_destroy) o->thread_destroy(s->thread);
    s->thread = NULL;
    printf("  periodic reporting stopped\n");
}
