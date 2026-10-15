# Device Access & Info

> Capability: connect to the O4 ground station, obtain an authenticated session, and report the
> third-party device's capability set (device type / model). This is the entry point every
> integration starts from — all other capabilities require a live session produced here.

| | |
|---|---|
| **Device** | Common (dt5 third-party dock **and** dt7 RID device) |
| **Direction** | Uplink — the third party connects to and reports state to the dock |
| **Header** | `edge_dock/dock_session.h`, `edge_dock/auth.h`, `edge_dock/broker_discovery.h`, `edge_dock/dock_id_provider.h` |
| **Aggregate include** | `#include "edge_dock/edge_dock.h"` pulls in all of the above |

---

## 1. Overview

"Device online" is two steps:

1. **Connect** — resolve the O4 broker address (UDP discovery, or a fixed IP), then open an
   authenticated MQTT session. `dock_session_connect()` wraps the whole lifecycle.
2. **Report capability** — declare *what kind of device you are* (`device_type = 5` third-party
   dock, or `device_type = 7` RID device) via `auth_report_capability_*()`. **Until the capability
   report is acknowledged, downstream capability domains do not take effect** — e.g. RID/ADS-B
   surveillance only works after a successful `dt7` report.

| Sub-capability | API | Purpose |
|---|---|---|
| Broker discovery | `dock_discover_broker()` | Listen on UDP `18830` for the O4 broadcast, parse the broker IP |
| Connect / disconnect | `dock_session_connect()` / `dock_session_disconnect()` | Open / close the authenticated session |
| Default options (dock, dt5) | `dock_session_default_opts()` | Fill `DockSessionOpts` from the compiled-in credentials, `device_sn` = `DOCK_DEVICE_SN` |
| Default options (RID, dt7) | `dock_session_default_opts_rid()` | Same, but `device_sn` = `DOCK_RID_DEVICE_SN` and `username_suffix` = `"@rid"` |
| Capability report (dock) | `auth_report_capability_dt5()` / `_sync()` | Declare a `device_type=5` third-party dock |
| Capability report (RID) | `auth_report_capability_dt7()` / `_sync()` | Declare a `device_type=7` RID device |
| ID provider | `dock_id_provider_register()` / `_unregister()` | Supply the envelope `bid`/`tid` generator (handled automatically by the session wrapper) |

---

## 2. Quick example

The minimal path — connect, then report capability. Taken from `samples/quickstart/main.c`:

```c
#include "edge_dock/edge_dock.h"
#include <stdio.h>

int main(void)
{
    DockSessionOpts opts;
    dock_session_default_opts(&opts);          /* credentials + UDP discovery from dock_app_info.h */

    ThingModelClientSession_t *s = dock_session_connect(&opts);
    if (!s) { fprintf(stderr, "connection failed\n"); return 1; }

    /* declare this device as a third-party dock (device_type = 5) */
    auth_report_capability_dt5(s);

    /* ... use other capabilities (flight / charging / config / ...) with `s` ... */

    dock_session_disconnect(s);
    return 0;
}
```

Prefer `auth_report_capability_dt5_sync()` in production: it waits for the dock's acknowledgement so
you know the device is truly online before proceeding.

### Reconnects re-report automatically

The dock keeps a third-party device registered only for the life of **one** MQTT session. The SDK
reconnects on its own (`auto_reconnect` is on), and the device comes back *connected but
unregistered*: its peripheral entry stays at `status:2`, and the dock then silently discards every
point-to-point `requests_reply` — the SDR, network and charging domains, which are answered by the
dock's peripheral server rather than published straight to the standard reply topic. Nothing reports
an error; those calls simply time out while `set_tsl_params` / `airport_organization_bind` keep
working. That asymmetric "some methods work, some time out" pattern is the fingerprint.

`dock_session_default_opts()` / `_rid()` therefore install `opts.on_reconnect` for you
(`auth_report_capability_dt5` / `_dt7`), and `dock_session_connect()` re-runs it after every
reconnect. You get this by default — nothing to call. Two caveats:

- The hook runs on the runtime's reconnect worker thread, which also owns handling the *next* drop,
  so it must stay non-blocking. Do not swap in `auth_report_capability_dt5_sync()`.
- The **first** connect is not covered; keep reporting it explicitly, as the sample above does.

Set `opts.on_reconnect = NULL` to opt out, or point it at your own function to re-run additional
per-session setup.

---

## 3. API reference

### `void dock_session_default_opts(DockSessionOpts *opts)`
Fills `opts` with the credentials and defaults compiled in from `dock_app_info.h` (set via the
`DOCK_APP_ID` / `DOCK_APP_KEY` / … CMake cache variables). This is the **dt5 (third-party dock)**
identity: `device_sn` = `DOCK_DEVICE_SN`, no username suffix. Always call this (or the `_rid`
variant) first, then override individual fields as needed.
- Run-time override: the `DOCK_DEVICE_SN` env var. An explicit `opts.device_sn` assignment still wins.

### `void dock_session_default_opts_rid(DockSessionOpts *opts)`
Same, but fills in the **dt7 (RID)** identity: `device_sn` = `DOCK_RID_DEVICE_SN` and
`username_suffix` = `"@rid"`.
- dt5 and dt7 are two distinct devices on the gateway. The account `"<app_id>_esdk_<device_sn>"` is
  both the MQTT client_id and the topic owner segment, so they **must not share an SN**: whichever
  connects second is refused with `CONNACK rc=5 (not authorised)`.
- `DOCK_RID_DEVICE_SN` defaults to `<DOCK_DEVICE_SN>_rid` (or to `DOCK_DEVICE_SN` itself in an
  RID-only build, `-DEDGE_DOCK_ENABLE_DOCK=OFF`); pass `cmake -DDOCK_RID_SN=...` for a fully
  independent value. Run-time override: the `DOCK_RID_SN` env var.

### `ThingModelClientSession_t *dock_session_connect(const DockSessionOpts *opts)`
Opens an authenticated session. Internally it, in order: registers the `bid`/`tid` id provider →
(optionally) runs UDP discovery → connects to the broker.
- **Returns** a session handle on success, or `NULL` on failure.
- The returned handle is the required first argument to every other capability API.
- Discovery: when `opts->use_discovery` is true, listens on UDP `18830` for `discovery_timeout_ms`;
  on timeout it falls back to `opts->fallback_ip`. When `opts->broker_ip` is non-empty, discovery is
  skipped and that IP is used directly.

### `void dock_session_disconnect(ThingModelClientSession_t *session)`
Closes the session and unregisters the id provider. Call once before exit.

### `int auth_report_capability_dt5(ThingModelClientSession_t *session)`
Reports `capability_report(device_type=5)` — a third-party dock — to the `state` topic. Fire-and-forget.
- **Returns** `0` on send success, `<0` on error.

### `int auth_report_capability_dt5_sync(ThingModelClientSession_t *session, int timeout_ms)`
### `int auth_report_capability_dt7_sync(ThingModelClientSession_t *session, int timeout_ms)`
Same as the fire-and-forget variants, but subscribe to `state_reply` and wait for the
acknowledgement, verifying `data.result == 0` and printing `data.error_reason` when it is not.
`timeout_ms <= 0` uses the default `5000` ms.

This is the only way to see a **rejected** report. The publish succeeds either way; a dock refuses at
the reply layer — e.g. `result=201 "Device type slot is full, max limit reached for this type"` when
the gateway's slots for that `device_type` are already taken. Left unread, the device stays
unregistered and the dock silently discards every point-to-point `requests_reply` (SDR, network,
charging), which surfaces only as unexplained timeouts.

The `state_reply` subscription is deliberately left in place afterwards, so the session keeps
receiving acknowledgements for the fire-and-forget reports that `DockSessionOpts.on_reconnect` sends
after a reconnect — attach a session listener to see them.
- **Returns** `0` = acknowledged; `THING_MODEL_RC_TIMEOUT (-3)` = no reply in time; other `<0` =
  reply received but `result != 0`.

### `int auth_report_capability_dt7(ThingModelClientSession_t *session)`
Reports `capability_report(device_type=7)` — an RID device. **Required before** the RID/ADS-B
surveillance capability (`rid_infos` / `adsb_infos`) will be accepted by the dock.

### `int dock_discover_broker(char *out_ip, size_t out_ip_cap, int timeout_ms)`
Low-level discovery, if you want the broker IP without opening a session (`dock_session_connect`
calls this for you). Blocks on UDP `18830` until a valid `MQTTBRK|…|ip=<broker_ip>|…` broadcast
arrives or it times out (`timeout_ms <= 0` = wait forever). `out_ip_cap` ≥ 46 recommended.
- **Returns** `1` = found (`out_ip` filled); `0` = timeout/failure (caller falls back to a default IP).

### `void dock_id_provider_register(const char *dock_sn)` / `void dock_id_provider_unregister(void)`
Register/unregister the envelope `bid`/`tid` generator (UUIDv4 via libuuid, falling back to
`"<dock_sn>-<counter>"`). **You normally do not call these** — `dock_session_connect` /
`_disconnect` manage them. Exposed only for advanced setups that drive the runtime directly.

---

## 4. Interaction flow

```
  your app                          edge-sdk                         O4 dock
    │  dock_session_default_opts()     │                                │
    │─────────────────────────────────▶│                                │
    │  dock_session_connect(opts)       │  register bid/tid provider     │
    │─────────────────────────────────▶│                                │
    │                                   │  UDP 18830 discovery ──────────▶ (broadcast MQTTBRK|…|ip=…)
    │                                   │◀───────────────────────────────│
    │                                   │  MQTT connect + auth ──────────▶│
    │◀── session handle (or NULL) ──────│◀───────────────────────────────│
    │  auth_report_capability_dt5_sync()│  publish state: capability_report
    │─────────────────────────────────▶│────────────────────────────────▶│
    │◀── 0 = online ────────────────────│◀─── state_reply {result:0} ─────│
                        (device is now online; other capabilities become usable)
```

---

## 5. Data structures — `DockSessionOpts`

Populate with `dock_session_default_opts()` (dt5) or `dock_session_default_opts_rid()` (dt7), then
override as needed.

| Field | Type | Meaning |
|---|---|---|
| `app_id`, `app_key`, `app_license`, `app_name` | `const char *` | DJI developer credentials. Empty → the `dock_app_info.h` macros are used |
| `device_sn` | `const char *` | This device's serial number. `DOCK_DEVICE_SN` for dt5, `DOCK_RID_DEVICE_SN` for dt7 — **the two must differ** (the account doubles as the client_id). Run-time override: the `DOCK_DEVICE_SN` / `DOCK_RID_SN` env vars |
| `broker_ip` | `const char *` | If non-empty, skip discovery and connect here directly |
| `broker_port` | `int` | Broker port (default `1883`) |
| `use_discovery` | `bool` | Listen on UDP `18830` first (default `true`) |
| `discovery_timeout_ms` | `int` | Max discovery wait before falling back |
| `fallback_ip` | `const char *` | Address used when discovery fails |
| `plaintext_access` | `bool` | Default `true`: single-shot plaintext MQTT, username `"<app_id>_esdk_<sn>"`. `false` → runtime two-phase TLS+HMAC secure access (needs a CA and a secure broker) |
| `username_suffix` | `const char *` | Plaintext username suffix; RID devices use `"@rid"` to raise the single-packet limit — set for you by `dock_session_default_opts_rid()`, no need to hand-set it for dt7. Overridable via the `DOCK_USERNAME_SUFFIX` env var. `NULL`/`""` = none |
| `transport_vtable` | `const void *` | Optional custom transport (e.g. a CI loopback mock). `NULL` = default mosquitto |

---

## 6. Error handling

`dock_session_connect()` returns `NULL` on any failure (bad credentials, no broker reachable, auth
rejected). Check for `NULL` before using the handle.

The `auth_report_capability_*` functions return a thing-model return code:

| Code | Value | Meaning |
|---|---|---|
| `THING_MODEL_RC_OK` | `0` | Success (for `_sync`: acknowledged with `result:0`) |
| `THING_MODEL_RC_TIMEOUT` | `-3` | `_sync` received no `state_reply` within the timeout |
| `THING_MODEL_RC_TRANSPORT_ERROR` | `-4` | Send failed at the transport layer |
| `THING_MODEL_RC_AUTH_FAILED` | `-5` | Authentication rejected |
| other `< 0` | — | Reply received but `result != 0`, or another runtime error |

Gateway-side `state_reply` results worth recognising:

| `data.result` | `error_reason` | What it means |
|---|---|---|
| `0` | — | Device is online; downstream capabilities are now usable |
| `201` | `Device type slot is full, max limit reached for this type` | A device of that `device_type` is already bound and the type's slots are taken. Unbind the existing device of that type first — a second one is refused whatever SN it uses |

The capability report is answered on the `state_reply` topic, so a rejection like the above is
reported there, not by the publish call: `dock_session_connect()` and the fire-and-forget
`auth_report_capability_dt5()/dt7()` return success once the message is sent. Use the `_sync` variant
(or your own `state_reply` listener) whenever you need the gateway's verdict — which is why
`auth_report_capability_dt5_sync()` is the recommended choice in production.

**Notes**
- Declare the right `device_type`: `dt5` for a third-party dock, `dt7` for an RID device. The `dt7`
  report is a hard prerequisite for the surveillance capability — reporting `dt5` will not enable it.
- Do the capability report **after** a successful connect and **before** exercising other
  capabilities; prefer the `_sync` variant so you act only once the device is confirmed online.
- Discovery and connect are the two independent failure points — a `NULL` session most often means
  the broker was neither discovered nor reachable at `fallback_ip`.
