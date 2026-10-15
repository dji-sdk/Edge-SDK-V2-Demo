# edge-sdk Developer Documentation

Third-party dock integration SDK for the O4 ground station. Docs are organized by capability. Every
page has a Chinese version at `*.zh.md`.

## Getting started

| | |
|---|---|
| [Quickstart](docs/guide/01-quickstart.md) | Get running in five steps |
| [Build & Deploy](docs/guide/02-build-and-deploy.md) | Host / aarch64, credentials, single-device tailoring |
| [Concepts](docs/guide/03-concepts.md) | Session · dt5/dt7 · uplink/downlink · service/reply/event · bid/tid |

## Capabilities

| Capability | Device | Summary |
|---|---|---|
| [Device Access & Info](docs/features/00-device-access-and-info.md) | common | Connect, authenticate, report capability |
| [Flight Control](docs/features/01-flight-control.md) | dt5 | Takeoff / landing / abnormal recovery, progress push |
| [Charging & Power](docs/features/02-charging-and-power.md) | dt5 | Aircraft power on/off, start/stop charging |
| [Thing-Model Parameters](docs/features/03-thing-model-parameters.md) | dt5 | Read/write dock thing-model properties |
| [Dock Configuration](docs/features/04-dock-configuration.md) | dt5 | Network, SDR firmware, cloud, organization binding |
| [Surveillance (RID/ADS-B)](docs/features/05-surveillance-rid.md) | dt7 | Report detected aircraft |

## Recommended reading path

1. [Concepts](docs/guide/03-concepts.md) — the five ideas the whole API rests on.
2. [Quickstart](docs/guide/01-quickstart.md) — wire it together end to end.
3. The [capability](docs/features/) page for whatever you're building.
