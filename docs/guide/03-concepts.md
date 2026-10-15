# Concepts

A short mental model so the API reads naturally. Five ideas cover everything.

## 1. Session

Everything flows through a **session** — an authenticated MQTT connection to the O4 dock's broker.
`dock_session_connect()` gives you a `ThingModelClientSession_t *`, and that handle is the first
argument to every capability object (`flight_create`, `charging_create`, …). No session, no
capabilities.

## 2. Device type: dt5 vs dt7

Your device declares *what it is* via a capability report:

- **dt5** — third-party dock (`device_type=5`): flight control, charging, configuration.
- **dt7** — RID device (`device_type=7`): surveillance (RID / ADS-B).

The report is a prerequisite: capabilities of a type are only accepted after that type is reported.

## 3. Uplink vs downlink

Two directions, and they decide who initiates:

- **Uplink** (you → dock) — *you* call the dock: charging, configuration, thing-model parameters.
  Request/reply, synchronous-feeling.
- **Downlink** (dock → you) — *the dock* calls you: flight control. You register handlers and reply.

Knowing the direction tells you whether you drive the call or wait for one.

## 4. Service / reply / event

Downlink flows (flight) have three parts:

| Part | Who | Meaning |
|---|---|---|
| **service** | dock → you | the dock asks you to do something |
| **services_reply** | you → dock | you acknowledge (the SDK auto-replies `in_progress`) |
| **event** | you → dock | you push progress (`percent` 0→100) until done |

So a takeoff isn't one message — it's a service you accept, then a stream of progress events you push.

## 5. Envelope: bid / tid

Every message is wrapped in an envelope carrying a `bid` (business/capability id) and a `tid` (per-call
trace id). The `tid` is how a reply is paired with its request — matching `tid`s on a request and its
reply mean the round trip succeeded. The SDK generates these for you (UUIDv4 via libuuid, or a
`SN + counter` fallback); you rarely touch them, but they're the first thing to check when debugging
the wire.

---

With these five, the feature guides read directly: pick a session, know your device type, follow the
direction, and (for flight) handle the service→reply→event rhythm. See [Quickstart](01-quickstart.md)
to put them together.

---

## Appendix: several "SNs", don't mix them up

The word "SN" is overloaded across several distinct values. Getting them confused is the #1 source of
real-device errors (e.g. the broker rejecting a topic with "client ID does not match").

| Concept | Name in the SDK | Example | Where it lives |
|---|---|---|---|
| Raw third-party dock SN (dt5 connection input) | `device_sn` | `mqtt_test` | filled from `DOCK_DEVICE_SN` by `dock_session_default_opts()` — used **before** connect |
| Raw RID device SN (dt7 connection input) | `device_sn` | `mqtt_test_rid` | filled from `DOCK_RID_DEVICE_SN` by `dock_session_default_opts_rid()` |
| Account = MQTT username = topic owner segment | `dock_session_account(s)` | `195_esdk_mqtt_test` / `195_esdk_mqtt_test_rid` (`app_id"_esdk_"device_sn`) | returned by the runtime's `GetSn()`; **after** connect, everything that builds a topic or feeds a capability object (e.g. `flight_create`) uses this |
| Dock's own real SN | (not exposed) | — | firmware-internal; the third-party side never sees it |

**dt5 and dt7 are two devices as far as the gateway is concerned, and must use two different SNs.**
The account doubles as the MQTT client_id: if they share one SN, whichever connects **second** is
refused with `CONNACK rc=5 (not authorised)`. Leaving `DOCK_RID_SN` empty derives
`<DOCK_DEVICE_SN>_rid`.

Rule of thumb: **`device_sn` only exists before connecting; once connected, use
`dock_session_account(s)` for anything topic-related.** Note the wire protocol itself reuses the field
name `device_sn` to carry the *account* value — that overload is inherited from the protocol and can't
be renamed at the SDK layer, so watch for it when reading wire logs.

