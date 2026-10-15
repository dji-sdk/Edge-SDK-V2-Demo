# edge-sdk Developer Documentation

Third-party dock integration SDK for the O4 ground station. Docs are organized by capability. Every
page has a Chinese version at `*.zh.md`.

## Getting started

| | |
|---|---|
| [Quickstart](guide/01-quickstart.md) | Get running in five steps |
| [Build & Deploy](guide/02-build-and-deploy.md) | Host / aarch64, credentials, single-device tailoring |
| [Concepts](guide/03-concepts.md) | Session · dt5/dt7 · uplink/downlink · service/reply/event · bid/tid |

## Capabilities

| Capability | Device | Summary |
|---|---|---|
| [Device Access & Info](features/00-device-access-and-info.md) | common | Connect, authenticate, report capability |
| [Flight Control](features/01-flight-control.md) | dt5 | Takeoff / landing / abnormal recovery, progress push |
| [Charging & Power](features/02-charging-and-power.md) | dt5 | Aircraft power on/off, start/stop charging |
| [Thing-Model Parameters](features/03-thing-model-parameters.md) | dt5 | Read/write dock thing-model properties |
| [Dock Configuration](features/04-dock-configuration.md) | dt5 | Network, SDR firmware, cloud, organization binding |
| [Surveillance (RID/ADS-B)](features/05-surveillance-rid.md) | dt7 | Report detected aircraft |

## Recommended reading path

1. [Concepts](guide/03-concepts.md) — the five ideas the whole API rests on.
2. [Quickstart](guide/01-quickstart.md) — wire it together end to end.
3. The [capability](features/) page for whatever you're building.
