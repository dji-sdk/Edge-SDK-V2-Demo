# Charging & Power

> Capability: power the aircraft on/off and start/stop charging inside the dock. Four simple
> request/reply operations the third party actively calls on the dock.

| | |
|---|---|
| **Device** | dt5 (third-party dock) |
| **Direction** | Uplink — the third party calls the dock (`requests` / `requests_reply`) |
| **Header** | `edge_dock/charging.h` |

---

## 1. Overview

Four operations, two pairs:

| Operation | API | Meaning |
|---|---|---|
| Aircraft power on | `charging_drone_open()` | Power **on** the aircraft in the dock |
| Aircraft power off | `charging_drone_close()` | Power **off** the aircraft in the dock |
| Start charging | `charging_charge_open()` | Begin charging |
| Stop charging | `charging_charge_close()` | Stop charging |

Each call is a `requests` → `requests_reply` round trip; the reply carries a `status`.

> Note: "power" here is the **aircraft's** power (`drone_open`/`drone_close`), not a hatch or cover —
> the dock has no hatch-cover operation in this capability.

---

## 2. Quick example

```c
#include "edge_dock/edge_dock.h"

/* with a live session `s` */
Charging_t *ch = charging_create(s);

charging_drone_open(ch);      /* power on the aircraft */
charging_charge_open(ch);     /* start charging */
/* ... */
charging_charge_close(ch);    /* stop charging */
charging_drone_close(ch);     /* power off the aircraft */

charging_destroy(ch);
```

---

## 3. API reference

### `Charging_t *charging_create(ThingModelClientSession_t *session)`
Creates a charging object bound to `session`. Returns `NULL` on failure.

### `void charging_destroy(Charging_t *c)`
Frees the object.

### `int charging_drone_open(Charging_t *c)` / `int charging_drone_close(Charging_t *c)`
Power the aircraft on / off. Returns `0` on an acknowledged success, `<0` otherwise.

### `int charging_charge_open(Charging_t *c)` / `int charging_charge_close(Charging_t *c)`
Start / stop charging. Returns `0` on an acknowledged success, `<0` otherwise.

---

## 4. Interaction flow

```
  your app            edge-sdk                 O4 dock
    │ charging_charge_open(ch) ─▶│                 │
    │                            │ request: charge_open ─▶│
    │                            │◀── requests_reply: {result, output:{status}}
    │◀── 0 (or <0) ──────────────│                 │
```

---

## 5. Data structures

`Charging_t` is opaque; the four operations take no parameters. The dock's reply contains a
`status` field (charging/power state) surfaced through the return code.

---

## 6. Error handling

- `charging_create()` returns `NULL` on a bad/closed session.
- Each operation returns `0` only when the dock replies success; a `<0` return means the request
  failed or the reply reported a non-zero `result`. Sequence power/charge sensibly (e.g. power on
  before charging) and check each return before the next step.
