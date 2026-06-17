# ESDK 2.0 Access Sample

A minimal, self-contained sample showing how a third-party device connects to the
**DJI device** over MQTT and reports data, following the ESDK 2.0
access protocol.

It is organized as a **generic connection framework** plus **per-device modules**.
RID is provided as the first example device; you can add other device types the
same way.

## What it does

1. Connects to the DJI device and authenticates over MQTT.
2. On connect, the registered device module reports its **capability set**
   (declaring its `device_type` and supported capabilities).
3. Lets you report device events from a simple menu. The RID module demonstrates
   `rid_infos` and `adsb_infos`.
4. Demonstrates the **recommended reporting pattern**: cache detected targets and
   flush them in batches every 2 seconds (instead of one event per detection),
   to stay within the 5 Hz rate limit. In the RID submenu, `[4]` starts the
   periodic report (one keypress) and `[5]` stops it.

## Project layout

```
sample_esdk2/
├── CMakeLists.txt              # top-level build script
├── cmake/                      # CMake modules
│   └── FindMosquitto.cmake     # locates the mosquitto C++ binding
├── include/                    # cross-module public headers
│   ├── common/
│   │   └── log.h
│   ├── mqtt/
│   │   ├── app_info.h          # >>> fill in your credentials here <<<
│   │   ├── mqtt_client.h       # connect/auth, topics, generic capability/event report
│   │   └── mqtt_client_manager.h  # thin mosquitto wrapper
│   └── rid/
│       └── rid_reporter.h      # data structs: RidTarget / AdsbTarget / ReporterStatus
├── src/                        # implementation sources
│   ├── common/
│   │   └── log.cpp
│   ├── mqtt/
│   │   ├── mqtt_client.cpp
│   │   └── mqtt_client_manager.cpp
│   └── rid/
│       └── rid_reporter.cpp    # builds RID/ADS-B payloads via the generic API
├── third_party/                # bundled third-party libraries
│   └── json/
│       └── json.hpp            # nlohmann/json (single header)
└── examples/                   # runnable examples
    └── rid_report_demo/
        └── main.cpp            # entry point + interactive menu
```

## Prerequisites

- A Linux host (e.g. Ubuntu / Raspberry Pi) on the same LAN as the DJI device.
  The broker is at `192.168.50.10:1883`.

### Tested environment

This sample has been built and run on the following setup:

| Item | Version |
|---|---|
| Hardware | Raspberry Pi (aarch64 / arm64) |
| OS | Debian GNU/Linux 11 (bullseye), 64-bit |
| Kernel | 6.1.21-v8+ |
| Compiler | gcc / g++ 10.2.1 |
| CMake | 3.18.4 |
| GNU Make | 4.3 |
| mosquitto C++ binding | libmosquittopp-dev 2.0.11 |

### Toolchain & dependencies

- Build toolchain: `gcc` / `g++` (>= 8, C++17), `cmake` (>= 3.5), `make`.
- Third-party library: **mosquitto** C++ binding `libmosquittopp-dev`
  (tested with 2.0.11). The header-only `nlohmann/json` is already bundled in
  `third_party/json/json.hpp`, no extra install needed.

Install everything on Debian / Ubuntu / Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y libmosquittopp-dev cmake build-essential
```

> To pin the tested version explicitly: `sudo apt install -y libmosquittopp-dev=2.0.11-1+deb11u2`
> (the exact package revision may differ across distributions; the plain
> command above installs the distro default, which is sufficient).

## Configure

Edit `include/mqtt/app_info.h` and fill in the credentials obtained from the
DJI Developer website (Application registration):

```c
#define USER_DEVICE_ID       "your_device_id"
#define USER_APP_ID          "your_app_id"
#define USER_APP_KEY         "your_app_key"
#define USER_APP_LICENSE     "your_app_license"

// Optional username suffix to lift the 2048-byte payload limit for device
// types with large messages. Leave "" for the common case.
// Example: RID devices set this to "@rid".
#define USER_USERNAME_SUFFIX ""
```

The MQTT credentials are derived automatically:

- account / ClientID = `app_id` + `_esdk_` + `device_id`
- password = `app_key` + `_` + `app_license`
- username = account + `USER_USERNAME_SUFFIX`

## Build & run

```bash
mkdir build && cd build
cmake .. && make -j4
./rid_report_demo
```

On success you will see `CONNACK=0` and `capability set accepted (result=0)` in
the log, after which you can report events from the menu.

## How it is structured

- **Generic layer (`include/mqtt/`, `src/mqtt/`)** is device-agnostic. It handles
  the connection, authentication, common topics, and offers generic helpers:
  `mqtt_report_capabilities(device_type, capabilities)` and
  `mqtt_publish_event(method, data)`.
- **Device module (`include/rid/`, `src/rid/`)** declares its own `device_type` /
  capabilities and builds its event payloads, then sends them through the
  generic helpers. It registers a callback via `MqttClient::RegisterOnReady(...)`
  so its capability set is reported automatically on connect.

### Adding a new device module

1. Add headers under `include/<your_device>/` and sources under
   `src/<your_device>/`, with a reporter that:
   - builds its capability set and events via the generic helpers;
   - exposes a register function calling `MqttClient::RegisterOnReady(...)`;
   - exposes a `xxx_menu()` for its interactive submenu.
2. In `examples/rid_report_demo/main.cpp`, call its register function and add one
   row to `kModules[]`.
3. Add its source to the top-level `CMakeLists.txt`.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| `Credentials are not configured` on startup | `app_info.h` still has placeholder values | Fill in real `app_id` / `app_key` / `app_license` / `device_id` and rebuild. |
| `connect failed, CONNACK=5` | `app_id` / `app_key` / `app_license` failed validation, or account/password format is wrong | Double-check the credentials; account = `app_id_esdk_device_id`, password = `app_key_app_license`. |
| Stuck at `Not connected yet` | Wrong broker address, or the third-party device and the DJI device are not on the same LAN | Verify the broker address `192.168.50.10:1883`, IP subnet and port 1883. |
| `capability set rejected (result=N)` | Capability payload invalid or `device_type` wrong | Check the device module declares the correct `device_type` and required fields (RID uses `device_type = 7`). |
| `error event: ...` after reporting | Rate (5 Hz) or payload (2048 B) limit exceeded | Lower the rate; large-message device types set `USER_USERNAME_SUFFIX` (e.g. `@rid` for RID). |
| `not connected, drop publish` | Reported before the connection was up | Wait for the `[Connected]` status in the menu, then retry. |

## License

This sample is MIT-licensed. Please refer to the [LICENSE](./LICENSE) file for detailed information.

## Support

You can get official support from DJI and the community with the following methods:

- Post questions on Developer Forum
    * [DJI SDK Developer Forum (CN)](https://djisdksupport.zendesk.com/hc/zh-cn/community/topics)
    * [DJI SDK Developer Forum (EN)](https://djisdksupport.zendesk.com/hc/en-us/community/topics)
- Submit a request describing your problem on Developer Support
    * [DJI SDK Developer Support (CN)](https://djisdksupport.zendesk.com/hc/zh-cn/requests/new)
    * [DJI SDK Developer Support (EN)](https://djisdksupport.zendesk.com/hc/en-us/requests/new)
