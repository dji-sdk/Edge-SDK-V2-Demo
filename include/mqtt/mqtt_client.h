/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 * All information contained herein is, and remains, the property of DJI.
 * The intellectual and technical concepts contained herein are proprietary
 * to DJI and may be covered by U.S. and foreign patents, patents in process,
 * and protected by trade secret or copyright law.  Dissemination of this
 * information, including but not limited to data and other proprietary
 * material(s) incorporated within the information, in any form, is strictly
 * prohibited without the express written consent of DJI.
 *
 * If you receive this source code without DJI's authorization, you may not
 * further disseminate the information, and you must immediately remove the
 * source code and notify DJI of its removal. DJI reserves the right to pursue
 * legal actions against you for any loss(es) or damage(s) caused by your
 * failure to do so.
 *
 *********************************************************************
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <string>
#include <functional>
#include "mqtt/mqtt_client_manager.h"
#include "mqtt/app_info.h"
#include "json/json.hpp"

#define QOS_0 0
#define QOS_1 1
#define QOS_2 2

/* Random UUID, used for the bid / tid fields of every payload. */
std::string get_uuid();

/* The shared MQTT connection to the DJI device. */
MqttClientManager& getMqttClientManager();

/*
 * Generic Topic helper. `suffix` is the part after the client_id, e.g.
 * "state", "events", "state_reply". All topics are
 * thing/product/{client_id}/{suffix}.
 */
std::string mqtt_topic(const std::string& suffix);

/*
 * Generic capability-set report (Topic: state).
 * Any device type calls this once after connecting, passing its own
 * device_type and capabilities object.
 */
void mqtt_report_capabilities(int device_type, const nlohmann::json& capabilities);

/*
 * Generic event report (Topic: events).
 * `method` is e.g. "rid_infos" / "adsb_infos"; `data` is the payload body.
 */
void mqtt_publish_event(const std::string& method, const nlohmann::json& data);

/*
 * MqttClient
 * ----------
 * Device-agnostic connection layer:
 *   - connects to the DJI device and authenticates
 *   - subscribes to the common reply / error topics
 *   - notifies the registered "on ready" callback once connected, so each
 *     device module can report its own capability set and start working
 */
class MqttClient {
public:
    static MqttClient& getInstance() {
        static MqttClient instance;
        return instance;
    }

    // Register the callback invoked once the connection is up (CONNACK=0).
    // A device module registers here to report its capability set.
    using OnReadyCallback = std::function<void()>;
    void RegisterOnReady(OnReadyCallback cb) { on_ready_ = std::move(cb); }

    void Init();              // connect + subscribe
    bool IsConnected() const; // true after CONNACK = 0

    // Block until connected, or until timeout_ms elapses. Returns the result.
    bool WaitConnected(int timeout_ms = 5000) const;

    // False if app_info.h still contains the default placeholder credentials.
    static bool CredentialsConfigured();

    // Internal: invoked by the connection callback.
    void NotifyReady() { if (on_ready_) on_ready_(); }

private:
    MqttClient() = default;
    ~MqttClient() = default;
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    OnReadyCallback on_ready_;
};

#endif // MQTT_CLIENT_H
