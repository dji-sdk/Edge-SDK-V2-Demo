# Thing-Model Parameters

> Capability: read the dock's thing-model (TSL) properties, and push property updates using an
> accumulate-then-commit pattern. This is the dock's live state surface — work mode, home point,
> battery, charger state, and so on.

| | |
|---|---|
| **Device** | dt5 (third-party dock) |
| **Direction** | Uplink — the third party calls the dock (`get_tsl_params` / `set_tsl_params`) |
| **Header** | `edge_dock/config.h` |

---

## 1. Overview

Two directions:

- **Read** — `dock_config_get_tsl_params()` fetches the current thing-model values.
- **Write** — build up a set of property changes with the type-safe `dock_config_tsl_set_*()`
  setters, then send them all in one `set_tsl_params` message with
  `dock_config_set_tsl_params_commit()`. Only the properties you set appear on the wire.

The accumulate-then-commit design lets you push exactly the properties that changed, one batched
message per commit.

---

## 2. Quick example

```c
#include "edge_dock/edge_dock.h"

DockConfig_t *cf = dock_config_create(s);

/* read current values */
dock_config_get_tsl_params(cf);

/* stage a few property changes, then commit them as one set_tsl_params */
dock_config_tsl_set_home_point(cf, 113.95, 22.53, 100);  /* lon, lat, height */
dock_config_tsl_set_drone_is_open(cf, true);
dock_config_tsl_set_battery_voltage(cf, 12600.0);
dock_config_set_tsl_params_commit(cf);                   /* send + clear the accumulator */

dock_config_destroy(cf);
```

---

## 3. API reference

### `DockConfig_t *dock_config_create(ThingModelClientSession_t *session)` / `void dock_config_destroy(DockConfig_t *c)`
Create / free the config object. (Shared with the Dock Configuration capability.)

### `int dock_config_get_tsl_params(DockConfig_t *c)`
Request the current thing-model values. Returns `0` on an acknowledged reply, `<0` otherwise.

### Staging setters — `int dock_config_tsl_set_*(DockConfig_t *c, …)`
Each pushes one `{property: value}` into the pending accumulator. Return `0` on success, `<0` if the
accumulator is full. Available setters:

| Setter | Value type |
|---|---|
| `..._home_point_longitude` / `_latitude` | `double` |
| `..._home_point_height` | `int32_t` |
| `..._alt_land_longitude` / `_latitude` | `double` |
| `..._alt_land_height` | `int32_t` |
| `..._station_work_mode` / `_third_dock_work_mode` | `int32_t` |
| `..._drone_in_dock` / `_charger_state` | `int32_t` |
| `..._drone_is_open` / `_drone_is_bt_connect` | `bool` |
| `..._battery_voltage` / `_acdc_temperature` | `double` |

Convenience combos (chain several setters, no commit):
- `dock_config_tsl_set_home_point(c, lon, lat, height)`
- `dock_config_tsl_set_work_mode(c, station_mode, third_dock_mode)`

### `void dock_config_tsl_reset(DockConfig_t *c)`
Clear the accumulator without sending (auto-cleared after a commit).

### `int dock_config_set_tsl_params_commit(DockConfig_t *c)`
Package all staged properties into one `set_tsl_params` `data.params` array, send it, and clear the
accumulator. Returns `0` on an acknowledged success, `<0` otherwise.

### `int dock_config_set_tsl_params(DockConfig_t *c)`
Demo helper: stages a few example properties and commits them in one call (used by samples).

---

## 4. Interaction flow

```
  your app                                   edge-sdk              O4 dock
    │ dock_config_tsl_set_home_point(...)  ─▶│ (staged, not sent)   │
    │ dock_config_tsl_set_drone_is_open(...)─▶│ (staged)             │
    │ dock_config_set_tsl_params_commit()  ─▶│ set_tsl_params{params[...]} ─▶│
    │                                        │◀── requests_reply{result, output} 
    │◀── 0 (or <0) ──────────────────────────│                      │
```

---

## 5. Data structures

`DockConfig_t` is opaque. The staged properties map 1:1 to thing-model attribute names on the wire
(e.g. `drone_home_point_longitude`, `station_work_mode`, `battery_voltage`). Unset properties are
omitted from the `params` array.

---

## 6. Error handling

- A setter returning `<0` means the accumulator is full — commit (or `reset`) and retry.
- `commit` / `get` return `<0` on a transport failure or a non-zero `result` in the reply.
- Always `commit` (or `reset`) after staging — otherwise staged values linger into the next commit.
