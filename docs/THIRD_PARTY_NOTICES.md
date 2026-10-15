# Third-Party Notices (THIRD_PARTY NOTICES)

This SDK and its samples use the following third-party components. Copyright for each
remains with its respective author, and all are compatible with this package's MIT distribution.

| Component | Purpose | License | Notes |
|---|---|---|---|
| cJSON | JSON encoding/decoding | MIT | Source bundled with the package (`third_party/cJSON/`), v1.7.x |
| Eclipse Mosquitto (libmosquitto) | MQTT client transport | EPL-2.0 / EDL-1.0 (dual license) | Dynamically linked from the system; source not bundled |
| OpenSSL | TLS + HMAC-SHA256 | Apache-2.0 | Dynamically linked from the system |
| libuuid (util-linux) | UUIDv4 generation (optional) | BSD-3-Clause | Dynamically linked from the system; falls back to SN + counter when absent |

Notes:
- This package only **dynamically links** the system's mosquitto / OpenSSL / libuuid; it no longer
  distributes their source or binaries. The distribution obligation is borne by the consumer's system package manager.
- cJSON is bundled in source form; its MIT copyright and license notice are retained in `third_party/cJSON/cJSON.h`.
- The protocol runtime (`sdk/runtime/`) and generated code (`sdk/generated/`) are produced by DJI's
  protocol code generator and distributed with this package under MIT.
