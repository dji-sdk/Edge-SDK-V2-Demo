# Dock Configuration

> Capability: configure the dock's network, SDR (image-transmission) firmware type, cloud-platform
> access, and organization binding. Four independent settings the third party pushes to the dock.

| | |
|---|---|
| **Device** | dt5 (third-party dock) |
| **Direction** | Uplink — the third party calls the dock (`requests` / `requests_reply`) |
| **Header** | `edge_dock/config.h` |

---

## 1. Overview

Four independent configuration groups, all on the shared `DockConfig_t` object:

| Group | Read | Write |
|---|---|---|
| Network | `dock_config_get_network_params()` | `dock_config_set_network_params()` (DHCP) / `..._static[_ex]()` |
| SDR firmware | `dock_config_get_sdr_firmware()` | `dock_config_set_sdr_firmware[_ex]()` |
| Cloud platform | — | `dock_config_set_cloud_params[_ex]()` |
| Organization binding | — | `dock_config_organization_bind[_ex]()` |

Each `set` is a `requests` → `requests_reply` round trip returning a `result`.

---

## 2. Quick example

```c
#include "edge_dock/edge_dock.h"

DockConfig_t *cf = dock_config_create(s);

/* network: static IP */
dock_config_set_network_params_static_ex(cf,
        "192.168.1.100", "255.255.255.0", "192.168.1.1", "8.8.8.8", "8.8.4.4");

/* SDR firmware: 100 = M4TD, 103 = M400 */
dock_config_set_sdr_firmware_ex(cf, 100);

/* cloud onboarding: cloud_type decides which fields apply; the rest are dropped.
 * 0 = FlightHub 2 (org id + binding code), 1 = third-party cloud (everything),
 * 2 = FlightHub private cloud (address + org id + binding code).
 * Sends set_cloud_params, then airport_organization_bind. */
dock_config_cloud_onboard(cf, 2, "mqtt://host:1883", NULL, NULL, "org-id", "device-binding-code");

/* or drive the two methods yourself */
dock_config_set_cloud_params_ex(cf, 1, "mqtt://host:1883", "account", "password");
dock_config_organization_bind_ex(cf, "org-id", "device-binding-code");

dock_config_destroy(cf);
```

---

## 3. API reference

### Network
- `int dock_config_get_network_params(DockConfig_t *c)` — read current network config.
- `int dock_config_set_network_params(DockConfig_t *c)` — set dynamic IP (DHCP).
- `int dock_config_set_network_params_static(DockConfig_t *c)` — set static IP with demo defaults.
- `int dock_config_set_network_params_static_ex(DockConfig_t *c, const char *static_ip, const char *netmask, const char *gateway, const char *pri_dns, const char *sec_dns)`
  — set static IP with explicit fields.

### SDR firmware
- `int dock_config_get_sdr_firmware(DockConfig_t *c)` — read the current image-transmission firmware type.
- `int dock_config_set_sdr_firmware(DockConfig_t *c)` — set with a demo default.
- `int dock_config_set_sdr_firmware_ex(DockConfig_t *c, int drone_type)` — set explicitly; `drone_type` `100` = M4TD, `103` = M400.

### Cloud platform
`cloud_type` is a fixed enum and it decides which onboarding fields the dock accepts — the address
and MQTT credentials are only meaningful when the operator owns the broker:

| `cloud_type` | address (`mqtt_url`) | account / password | `organization_id` | `device_binding_code` |
|---|---|---|---|---|
| `0` FlightHub 2 | — | — | ✓ | ✓ |
| `1` third-party cloud | ✓ | ✓ | ✓ | ✓ |
| `2` FlightHub private cloud | ✓ | — | ✓ | ✓ |

- `bool dock_config_cloud_field_allowed(int cloud_type, E_DockCloudField field)` — the table above as a
  predicate; use it to decide which fields to collect from the operator before sending.
- `const char *dock_config_cloud_type_str(int cloud_type)` / `dock_config_cloud_field_name(E_DockCloudField)`
  — readable names for logs and prompts.
- `int dock_config_cloud_onboard(DockConfig_t *c, int cloud_type, const char *mqtt_url, const char *mqtt_account, const char *mqtt_password, const char *organization_id, const char *device_binding_code)`
  — one-shot onboarding. Fields that do not apply to `cloud_type`, and fields passed as `NULL`/`""`, are
  omitted from the request entirely. Sends `set_cloud_params`, then `airport_organization_bind` when an
  org id / binding code was supplied (the bind is skipped if `set_cloud_params` failed).
- `int dock_config_set_cloud_params(DockConfig_t *c)` — set with demo defaults.
- `int dock_config_set_cloud_params_ex(DockConfig_t *c, int cloud_type, const char *mqtt_url, const char *mqtt_account, const char *mqtt_password)`
  — the raw method, no policy applied; a `NULL` field is left off the wire.

### Organization binding
- `int dock_config_organization_bind(DockConfig_t *c)` — bind with demo defaults.
- `int dock_config_organization_bind_ex(DockConfig_t *c, const char *organization_id, const char *device_binding_code)`
  — bind with explicit organization id + binding code.

All return `0` on an acknowledged success, `<0` otherwise. The no-`_ex` variants call the `_ex` ones
with demo defaults.

---

## 4. Interaction flow

```
  your app                                    edge-sdk           O4 dock
    │ dock_config_set_sdr_firmware_ex(cf,100) ─▶│ request: set_sdr_firmware ─▶│
    │                                           │◀── requests_reply{result} ──│
    │◀── 0 (or <0) ─────────────────────────────│                   │
```

---

## 5. Data structures

`DockConfig_t` is opaque. Wire parameters by group:
- **Network (static):** `static_ip`, `netmask`, `gateway`, `pri_dns`, `sec_dns`; or `dynamic_ip=true` for DHCP.
- **SDR firmware:** `drone_type` (`100` M4TD / `103` M400).
- **Cloud:** `cloud_type` (`0`/`1`/`2`), `mqtt_url`, `mqtt_account`, `mqtt_password` — which of these
  apply depends on `cloud_type` (see the table in §3); fields that do not apply are not sent.
- **Organization:** `organization_id`, `device_binding_code`.

---

## 6. Error handling

- `set_cloud_params` and `organization_bind` reply with `result` only (no `output`); a `<0` return
  means the operation was rejected or failed in transit.
- Validate network fields before sending — an unreachable gateway/DNS will be accepted on the wire
  but can strand the dock; keep a recovery path (e.g. DHCP fallback).
