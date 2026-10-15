/**
 ********************************************************************
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * Broker discovery (UDP broadcast) —— pure C version
 * ------------------------------------------------------------------
 * The O4 ground station periodically broadcasts its MQTT broker address over UDP 18830, frame format:
 *
 *   MQTTBRK|v=1|id=<device_id>|name=<device_name>|ip=<broker_ip>|seq=<n>
 *
 * This module listens on 0.0.0.0:18830 and parses the ip field. On discovery failure the caller falls back to a default address.
 * (Ported from reference/sample_esdk2/src/mqtt/broker_discovery.cpp, C++→C.)
 ********************************************************************
 */
#ifndef DOCK_BROKER_DISCOVERY_H
#define DOCK_BROKER_DISCOVERY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP port for O4 broker discovery broadcasts. */
#define DOCK_DISCOVERY_PORT   18830

/**
 * Blocks listening on UDP 18830 until a valid broadcast is received or it times out.
 *
 * @param out_ip      On success, receives the parsed broker IP (dotted-decimal string).
 * @param out_ip_cap  Size of the out_ip buffer (recommended >= 46 to hold IPv4/IPv6 text).
 * @param timeout_ms  Maximum wait in milliseconds; <=0 means wait indefinitely.
 * @return 1=discovery succeeded (out_ip filled); 0=timeout/failure (caller should fall back to a default IP).
 */
int dock_discover_broker(char *out_ip, size_t out_ip_cap, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* DOCK_BROKER_DISCOVERY_H */
