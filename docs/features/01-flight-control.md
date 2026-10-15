# Flight Control

> Capability: respond to the dock's takeoff / landing / abnormal-recovery service calls, and push
> `0 → 100` progress back to the dock. This is how the aircraft's ground-station flight phases are
> coordinated with your dock.

| | |
|---|---|
| **Device** | dt5 (third-party dock) |
| **Direction** | Downlink — the dock issues a service, you reply and push progress events |
| **Header** | `edge_dock/flight.h` |

---

## 1. Overview

Flight control is **server-role**: the O4 dock drives it. Each of the five flows follows the same
three-part pattern:

1. **service** (dock → you) — the dock calls a service (e.g. `airport_takeoff_prepare`).
2. **services_reply** (you → dock) — the SDK's registered handler replies `in_progress`.
3. **events** (you → dock) — you push `percent` progress events until the phase completes.

The five flows: `takeoff_prepare`, `takeoff_end`, `landing_prepare`, `landing_end`,
`abnormal_recovery`. `flight_create()` registers all five service handlers at once; you only need to
drive the progress push.

---

## 2. Quick example

```c
#include "edge_dock/edge_dock.h"

/* after a live session `s` and a dt5 capability report */
Flight_t *fl = flight_create(s, dock_session_account(s));   /* registers the 5 service handlers */

/* when the takeoff-preparation phase runs, push progress 0 -> 100 */
flight_push_takeoff_prepare(fl);

flight_destroy(fl);
```

---

## 3. API reference

### `Flight_t *flight_create(ThingModelClientSession_t *session, const char *account)`
Creates a flight-control object and registers the five service handlers on `session`. `account` is
the session account / topic owner (`dock_session_account(s)`), not the raw `device_sn`. The handlers
auto-reply `in_progress` when the dock issues the matching service. Returns `NULL` on failure.

### `void flight_destroy(Flight_t *f)`
Unregisters the handlers and frees the object.

### `void flight_push_takeoff_prepare(Flight_t *f)` and siblings
Convenience pushers that automatically sweep a `0 → 100` progress event stream for one flow:
- `flight_push_takeoff_prepare(f)`
- `flight_push_takeoff_end(f)`
- `flight_push_landing_prepare(f)`
- `flight_push_landing_end(f)`
- `flight_push_abnormal_recovery(f)`

### `void flight_push_single(Flight_t *f, int action, const char *status, int percent)`
Push **one** custom progress event, for fine control over a single step.
- `action`: `0`=takeoff_prepare, `1`=takeoff_end, `2`=landing_prepare, `3`=landing_end,
  `4`=abnormal_recovery.
- `status`: schema enum — one of `canceled` / `failed` / `in_progress` / `ok` / `paused` /
  `rejected` / `sent` / `timeout`.
- `percent`: `0`–`100`. (The `flight_push_*` wrappers above call this repeatedly to sweep 0→100.)

### `void flight_selftest_inject_services(Flight_t *f)`
Self-test only: feeds the five service requests into the locally subscribed `services` topic to
trigger the handlers. **Valid only on the local mock broker** — on real hardware the dock drives the
services.

---

## 4. Interaction flow

```
  O4 dock                         edge-sdk (your handler)              your app
    │  service: airport_takeoff_prepare ──▶│                              │
    │◀── services_reply: in_progress ──────│  (auto-replied by the SDK)   │
    │                                       │◀── flight_push_takeoff_prepare(fl)
    │◀── event: percent=0 ──────────────────│                              │
    │◀── event: percent=... ────────────────│                              │
    │◀── event: percent=100 ────────────────│                              │
```

---

## 5. Data structures

There is no public struct — a `Flight_t` is opaque. The wire-level enums you supply through
`flight_push_single`:

| Arg | Values |
|---|---|
| `action` | `0` takeoff_prepare · `1` takeoff_end · `2` landing_prepare · `3` landing_end · `4` abnormal_recovery |
| `status` | `canceled` · `failed` · `in_progress` · `ok` · `paused` · `rejected` · `sent` · `timeout` |
| `percent` | `0`–`100` |

---

## 6. Error handling

- `flight_create()` returns `NULL` if handler registration fails (usually an invalid/closed session).
- The progress pushers are `void` — they best-effort publish; a dropped publish is retried on the
  next event in the sweep.
- Reply to a service promptly. The SDK auto-replies `in_progress` on your behalf; use
  `flight_push_single` with a terminal `status` (`ok` / `failed` / `rejected`) to close a phase
  explicitly when you need a non-`ok` outcome.
