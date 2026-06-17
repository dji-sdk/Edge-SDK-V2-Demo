/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 *********************************************************************
 */
#ifndef MQTT_BROKER_DISCOVERY_H
#define MQTT_BROKER_DISCOVERY_H

#include <string>

/*
 * Broker discovery (UDP broadcast)
 * --------------------------------
 * The DJI device periodically broadcasts its MQTT broker address on UDP port
 * 18830. The payload is a text frame:
 *
 *   MQTTBRK|v=1|id=<device_id>|name=<device_name>|ip=<broker_ip>|seq=<n>
 *
 * A device listens on 0.0.0.0:18830, parses the `ip` field, and connects to it
 * (MQTT port is fixed at 1883). If discovery is skipped or fails, fall back to
 * the default address 192.168.50.10.
 */

/* Default broker address used when discovery is skipped or fails. */
#define DEFAULT_BROKER_ADDRESS  "192.168.50.10"

/* The current broker IP (defaults to DEFAULT_BROKER_ADDRESS until set). */
const std::string& mqtt_broker_address();

/* Override the broker IP (call before the first getMqttClientManager()). */
void mqtt_set_broker_address(const std::string& ip);

/*
 * Interactive broker setup at startup:
 *   [1] listen on UDP 18830 until a valid broadcast arrives (blocking)
 *   [2] skip discovery, use the default address
 * The resolved address is stored and used by the MQTT connection.
 */
void mqtt_resolve_broker_address_interactive();

#endif // MQTT_BROKER_DISCOVERY_H
