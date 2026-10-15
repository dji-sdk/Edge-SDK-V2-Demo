# Quickstart — Get Started in Five Steps

The minimal path for a third-party dock vendor to integrate the O4 ground station with edge-sdk.
The SDK is organized by capability; a single `#include "edge_dock/edge_dock.h"` gives you everything.

For a deep dive on any capability, see the [feature guides](../features/). For the mental model
(sessions, uplink/downlink, service/reply/event), read [Concepts](03-concepts.md) first if anything
below is unfamiliar.

## Step 0 — Environment

- CMake ≥ 3.14, a C compiler, libmosquitto, OpenSSL (cJSON is bundled).
- Build (release mode, self-contained): `cmake -B build && cmake --build build -j`.

See [Build & Deploy](02-build-and-deploy.md) for host vs. aarch64, credentials, and single-device
tailoring.

## Step 1 — Fill in credentials

Set the CMake cache variables (or pass `-D`); `dock_app_info.h` is generated at configure time:

```bash
cmake -B build \
  -DDOCK_APP_ID=xxx -DDOCK_APP_KEY=xxx -DDOCK_APP_LICENSE=xxx \
  -DDOCK_APP_NAME=xxx -DDOCK_DEVICE_SN=your_dock_sn
```

(Shipping an RID device too? It needs its own SN — `DOCK_RID_SN`, default `<DOCK_DEVICE_SN>_rid`.
See [Build & deploy](02-build-and-deploy.md).)

## Step 2 — Connect

```c
#include "edge_dock/edge_dock.h"

DockSessionOpts opts;
dock_session_default_opts(&opts);          /* UDP(18830) discovery of the O4 broker, on by default */
ThingModelClientSession_t *s = dock_session_connect(&opts);
/* NULL on failure; discovery falls back to the configured fallback IP */
```

→ [Device Access & Info](../features/00-device-access-and-info.md)

## Step 3 — Go online: report capability

```c
auth_report_capability_dt5(s);   /* device_type=5 (dock). Use dt7 for an RID device */
```

## Step 4 — Register downlink handlers (dock-issued flows)

```c
Flight_t *fl = flight_create(s, dock_session_account(s));  /* registers the 5 service handlers */
/* when the dock issues airport_takeoff_prepare etc., the SDK auto-replies in_progress */
```

→ [Flight Control](../features/01-flight-control.md)

## Step 5 — Active calls / pushes

```c
/* Charging & aircraft power — call the dock */
Charging_t *ch = charging_create(s);
charging_charge_open(ch);

/* Push flight progress (percent 0→100) */
flight_push_takeoff_prepare(fl);

/* Surveillance report (RID device) */
Surveillance_t *sv = surveillance_create(s);
surveillance_report_rid(sv);

/* Thing-model parameters */
DockConfig_t *cf = dock_config_create(s);
dock_config_get_tsl_params(cf);
```

→ [Charging & Power](../features/02-charging-and-power.md) ·
[Thing-Model Parameters](../features/03-thing-model-parameters.md) ·
[Dock Configuration](../features/04-dock-configuration.md) ·
[Surveillance](../features/05-surveillance-rid.md)

## Cleanup

```c
charging_destroy(ch); dock_config_destroy(cf);
flight_destroy(fl); surveillance_destroy(sv);
dock_session_disconnect(s);
```

## Debugging: inspect the wire

```bash
THING_MODEL_LOG_LEVEL=debug ./build/dock_cli 2>&1 | grep -E 'PUB|RCV'
```
Matching `tid` values on a request and its reply mean the round trip paired up correctly.

## Runnable examples

- `samples/quickstart/main.c` — connect + capability report + a single `charge_open`.
- `samples/cli/main.c` — interactive menu across all capabilities, one item at a time.
