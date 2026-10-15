# Build & Deploy

How to build edge-sdk for your target, wire in credentials, and trim the SDK down to a single device
type. The release package is self-contained — no code generator, no Python, no network access.

## Two ways to build

### `build.sh` (convenience wrapper)

```bash
./build.sh              # host build (x86_64)            → build/
./build.sh --cross      # aarch64 cross-compile          → build-aarch64/
./build.sh -c           # clean the target's build dir and reconfigure
./build.sh -j 8         # set parallelism
```
Host and cross use **independent** build directories, so their caches never collide.

### Plain CMake

```bash
cmake -B build && cmake --build build -j
```
The top-level `CMakeLists.txt` auto-detects release mode (the `sdk/` snapshot is present in the
package) and builds a pure static target with zero code generation.

## Targets produced

| Artifact | What it is |
|---|---|
| `dock_quickstart` | Minimal example — connect + capability report + one charge |
| `dock_cli` | Interactive menu across all capabilities |

Host artifacts land in `build/`, cross artifacts in `build-aarch64/`.

## Credentials

Pass your developer credentials as CMake cache variables; `dock_app_info.h` is generated at configure
time (never commit real credentials):

```bash
cmake -B build \
  -DDOCK_APP_ID=xxx -DDOCK_APP_KEY=xxx -DDOCK_APP_LICENSE=xxx \
  -DDOCK_APP_NAME=xxx -DDOCK_DEVICE_SN=your_dock_sn \
  -DDOCK_BROKER_FALLBACK_IP=192.168.50.10 -DDOCK_BROKER_PORT=1883
```

If you also ship an RID device, it needs its **own** SN — the account `"<app_id>_esdk_<sn>"` is the
MQTT client_id, so if dt5 and dt7 share one, whichever connects second is refused with
`CONNACK rc=5 (not authorised)`. `DOCK_RID_SN` defaults to `<DOCK_DEVICE_SN>_rid`; pass it explicitly
for a fully independent value:

```bash
cmake -B build ... -DDOCK_RID_SN=your_rid_sn
```

Shipping **only** an RID device (`-DEDGE_DOCK_ENABLE_DOCK=OFF`)? Then there is no dt5 to collide
with, and `DOCK_DEVICE_SN` is used as-is — no `_rid` suffix is appended. Configure logs the SN it
resolved (`[edge-sdk] device SN: ...`); check that line if in doubt.

## Single-device tailoring

If you ship only one device type, exclude the other entirely — its sources and generated code are
dropped from the build, shrinking the binary:

```bash
cmake -B build -DEDGE_DOCK_ENABLE_DOCK=ON  -DEDGE_DOCK_ENABLE_RID=OFF   # dt5 dock only
cmake -B build -DEDGE_DOCK_ENABLE_DOCK=OFF -DEDGE_DOCK_ENABLE_RID=ON    # dt7 RID only
```

| Option | Device | Capabilities |
|---|---|---|
| `EDGE_DOCK_ENABLE_DOCK` | dt5 (dock) | flight, charging, config |
| `EDGE_DOCK_ENABLE_RID` | dt7 (RID) | surveillance |

At least one must be `ON`. The common core (session / discovery / id / auth) is always built.

## Cross-compiling for aarch64

`./build.sh --cross` uses the bundled `third_party/aarch64-linux-gnu/` mini-sysroot (headers +
prebuilt OpenSSL/mosquitto), so the cross build never touches host system libraries. It selects
`cmake/toolchain-aarch64.cmake` automatically.

## Dependencies

- **Host:** the build finds system OpenSSL and libmosquitto (cJSON is bundled as source).
- **Cross / vendored:** the bundled sysroot makes the SDK self-contained, no target system libraries
  required.
- Optional `libuuid` improves `bid`/`tid` generation; absent, the SDK falls back to `SN + counter`.
