/**
 * @copyright (c) 2026 DJI. All rights reserved. SPDX-License-Identifier: MIT
 * Capability domain 5: ADS-B / RID surveillance (third party pushes events, role=server).
 */
#ifndef EDGE_DOCK_SURVEILLANCE_H
#define EDGE_DOCK_SURVEILLANCE_H
#include "thing_model_client/thing_model_client_session.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Surveillance Surveillance_t;
Surveillance_t *surveillance_create(ThingModelClientSession_t *session);
void            surveillance_destroy(Surveillance_t *s);

/* Single-shot: report one demo target at a time (debug/self-test). */
void surveillance_report_rid(Surveillance_t *s);
void surveillance_report_adsb(Surveillance_t *s);

/* Single-shot, parameterized (for dock_cli interactive input): only the key fields are overridden,
 * the remaining non-key fields keep their demo defaults. Each call also becomes the periodic
 * thread's empty-cache fallback, so the value stays reported instead of being overwritten in 2s.
 * rid : lat/lon in degrees, height relative to takeoff point (m), speed horizontal (m/s), heading (deg 0~359).
 * adsb: icao 24-bit hex id string, callsign, lat/lon (deg), height MSL (m), speed in km/h
 *       (note the unit differs from rid, which is m/s), heading (deg).
 *       FlightHub 2 divides speed by 3.6 and labels the result m/s, so 941 reaches the screen as
 *       261.4 -- send km/h, read m/s. */
void surveillance_report_rid_ex(Surveillance_t *s,
        double lat, double lon, int height, double speed, int heading);
void surveillance_report_adsb_ex(Surveillance_t *s,
        const char *icao, const char *callsign,
        double lat, double lon, int height, double speed, int heading);

/* Periodic reporting: a background thread flushes cached targets as one batched event every 2s (matching the dock's 5Hz rate limit).
 * When the cache is empty, the last single-shot target is resent as a fallback. Automatically stopped on destroy. */
void surveillance_start_periodic(Surveillance_t *s);
void surveillance_stop_periodic(Surveillance_t *s);

/* Cache one detected target (in production called from a radar callback, to be batch-flushed in the next cycle).
 * t points to the generated T_RidTarget / T_AdsbTarget (see surveillance/surveillance_types.h);
 * uses void* to avoid leaking the generated types into this header. */
void surveillance_cache_rid_target(Surveillance_t *s, const void *rid_target);
void surveillance_cache_adsb_target(Surveillance_t *s, const void *adsb_target);
#ifdef __cplusplus
}
#endif
#endif
