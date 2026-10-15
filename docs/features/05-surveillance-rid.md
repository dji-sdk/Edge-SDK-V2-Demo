# Surveillance (RID / ADS-B)

> Capability: report detected nearby aircraft — Remote ID (RID) and ADS-B targets — to the dock,
> either one-shot or as a batched periodic stream. This is the RID device's core job.

| | |
|---|---|
| **Device** | dt7 (RID device) — requires a `dt7` capability report first |
| **Direction** | Uplink — the third party pushes `events` |
| **Header** | `edge_dock/surveillance.h` |

---

## 1. Overview

Surveillance reports **other** aircraft your device detects (it is not self-positioning). Two report
styles:

- **One-shot** — `surveillance_report_rid[_ex]()` / `surveillance_report_adsb[_ex]()`: send a single
  target immediately (debug / low rate).
- **Periodic batch** — cache targets as they are detected, and a background thread flushes them as
  one batched event every 2 s (matching the dock's 5 Hz rate limit). This is the production path.

> Prerequisite: report `dt7` capability (`auth_report_capability_dt7`) before surveillance events are
> accepted — see *Device Access & Info*. Connect the RID device with `dock_session_default_opts_rid()`,
> which supplies its own SN plus the `"@rid"` username suffix.

---

## 2. Quick example

```c
#include "edge_dock/edge_dock.h"

/* session `s` connected as RID, dt7 capability already reported */
Surveillance_t *sv = surveillance_create(s);

/* one-shot RID target: lat, lon, height(m), speed(m/s), heading(deg) */
surveillance_report_rid_ex(sv, 22.53, 113.95, 120, 8.0, 90);

/* production: cache from your radar callback, flush periodically */
surveillance_start_periodic(sv);
/*   ... surveillance_cache_rid_target(sv, &target) from detections ... */
surveillance_stop_periodic(sv);

surveillance_destroy(sv);
```

---

## 3. API reference

### `Surveillance_t *surveillance_create(ThingModelClientSession_t *session)` / `void surveillance_destroy(Surveillance_t *s)`
Create / free the surveillance object. `destroy` also stops periodic reporting.

### One-shot reporters
- `void surveillance_report_rid(Surveillance_t *s)` / `void surveillance_report_adsb(Surveillance_t *s)`
  — report one demo target (debug/self-test).
- `void surveillance_report_rid_ex(Surveillance_t *s, double lat, double lon, int height, double speed, int heading)`
  — `lat`/`lon` in degrees, `height` relative to takeoff point (m), `speed` horizontal (m/s), `heading` 0–359°.
- `void surveillance_report_adsb_ex(Surveillance_t *s, const char *icao, const char *callsign, double lat, double lon, int height, double speed, int heading)`
  — `icao` 24-bit hex id string, `callsign`, `lat`/`lon` (deg), `height` MSL (m), `speed` (**km/h** —
  note the unit differs from the RID target above, which is m/s), `heading` (deg).
  FlightHub 2 divides `speed` by 3.6 and labels the result m/s, so the default `941` reaches the
  screen as `261.4` — send km/h, read m/s.
  Only the key fields are overridden; remaining non-key fields keep demo defaults.
  Each call also becomes the periodic thread's empty-cache fallback (see below).

### Periodic batch
- `void surveillance_start_periodic(Surveillance_t *s)` / `void surveillance_stop_periodic(Surveillance_t *s)`
  — start/stop the 2 s batch-flush background thread. When the cache is empty the **last single-shot
  target** is resent as fallback (the demo target until you send something else), so a value entered
  in the menu keeps being reported instead of being overwritten on the next cycle.
- `void surveillance_cache_rid_target(Surveillance_t *s, const void *rid_target)` /
  `void surveillance_cache_adsb_target(Surveillance_t *s, const void *adsb_target)`
  — cache one detected target for the next flush. In production, call from your radar callback. The
  pointer is a generated `T_RidTarget` / `T_AdsbTarget` (`surveillance/surveillance_types.h`); `void *`
  keeps the generated types out of this header.

---

## 4. Interaction flow

```
  radar callback        edge-sdk (Surveillance)              O4 dock
    │ cache_rid_target(sv,&t) ─▶│ (buffered)                    │
    │ cache_rid_target(sv,&t) ─▶│ (buffered)                    │
    │           (every 2s)      │ event: rid_infos {targets[...]} ─▶│
    │                           │ event: adsb_infos {targets[...]} ─▶│
```

---

## 5. Data structures

- One-shot `_ex` args are plain scalars/strings (see API reference above).
- Cached targets are the generated `T_RidTarget` / `T_AdsbTarget` structs from
  `surveillance/surveillance_types.h`; populate one and pass its address to `cache_*`.

---

## 6. Error handling

- The reporters return `void` — they best-effort publish. Reliability comes from the periodic batch
  cadence (2 s) rather than per-call return checks.
- Respect the 5 Hz rate limit: prefer `start_periodic` + `cache_*` over hand-rolled tight-loop
  one-shot reporting, which the dock will throttle/drop.
- If events are silently ignored by the dock, verify the `dt7` capability report succeeded first.
