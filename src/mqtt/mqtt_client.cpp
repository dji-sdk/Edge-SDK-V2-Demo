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
#include <random>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

#include "mqtt/mqtt_client.h"
#include "mqtt/broker_discovery.h"
#include "json/json.hpp"
#include "common/log.h"

using json = nlohmann::json;

static const char* kProtocolVersion = "1.0.0"; // payload "version" field

// ---------------------------------------------------------------------------
// Connection parameters
// ---------------------------------------------------------------------------
// The broker IP is resolved at startup (UDP discovery or default); see
// mqtt_broker_address() in broker_discovery.h. Port is fixed at 1883.
#define BROKER_PORT     1883

// client_id / account = app_id "_esdk_" device_id   (e.g. 123_esdk_MYDEV-01)
#define MQTT_ACCOUNT    USER_APP_ID "_esdk_" USER_DEVICE_ID
// password = app_key "_" app_license
#define MQTT_PASSWORD   USER_APP_KEY "_" USER_APP_LICENSE
#define CLIENT_ID       MQTT_ACCOUNT

// Common reply / error topics (device-agnostic).
#define TOPIC_STATE_REPLY   "thing/product/" CLIENT_ID "/state_reply"
#define TOPIC_EVENTS_REPLY  "thing/product/" CLIENT_ID "/events_reply"
#define TOPIC_ERROR         "thing/product/" CLIENT_ID "/error"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string get_uuid() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) ss << '-';
        ss << dist(gen);
    }
    return ss.str();
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string mqtt_topic(const std::string& suffix) {
    return std::string("thing/product/") + CLIENT_ID + "/" + suffix;
}

// ---------------------------------------------------------------------------
// Shared MQTT connection
// ---------------------------------------------------------------------------
MqttClientManager& getMqttClientManager() {
    // The username may carry an optional suffix (e.g. "@rid") to lift the
    // default payload limit for device types with large messages.
    static const std::string username = std::string(MQTT_ACCOUNT) + USER_USERNAME_SUFFIX;
    static MqttClientManager mqtt(CLIENT_ID, mqtt_broker_address(), BROKER_PORT,
                                  username, MQTT_PASSWORD);
    return mqtt;
}

// ---------------------------------------------------------------------------
// Generic report helpers (device-agnostic)
// ---------------------------------------------------------------------------
void mqtt_report_capabilities(int device_type, const json& capabilities) {
    json payload = {
        {"bid", get_uuid()},
        {"tid", get_uuid()},
        {"timestamp", now_ms()},
        {"version", kProtocolVersion},
        {"data", {
            {"device_type", device_type},
            {"capabilities", capabilities},
        }},
    };
    LOG_INFO("[MQTT] report capabilities (device_type=%d)", device_type);
    getMqttClientManager().Publish(mqtt_topic("state"), payload.dump(), QOS_1);
}

void mqtt_publish_event(const std::string& method, const json& data) {
    json payload = {
        {"bid", get_uuid()},
        {"tid", get_uuid()},
        {"timestamp", now_ms()},
        {"version", kProtocolVersion},
        {"method", method},
        {"data", data},
    };
    LOG_INFO("[MQTT] publish event (method=%s)", method.c_str());
    getMqttClientManager().Publish(mqtt_topic("events"), payload.dump(), QOS_1);
}

// ---------------------------------------------------------------------------
// Incoming message handlers (device-agnostic)
// ---------------------------------------------------------------------------

// Reply to the capability-set upload. result == 0 means access succeeded.
static void on_state_reply(const std::string& topic, const std::string& payload) {
    (void)topic;
    json j = json::parse(payload, nullptr, false);
    if (j.is_discarded()) {
        LOG_ERROR("[MQTT] state_reply: invalid JSON: %s", payload.c_str());
        return;
    }
    int result = j.value("data", json::object()).value("result", -1);
    if (result == 0) {
        LOG_INFO("[MQTT] capability set accepted (result=0), ready to report data");
    } else {
        LOG_ERROR("[MQTT] capability set rejected (result=%d): %s", result, payload.c_str());
    }
}

// Reply to an events upload.
static void on_events_reply(const std::string& topic, const std::string& payload) {
    (void)topic;
    LOG_INFO("[MQTT] events_reply: %s", payload.c_str());
}

// The DJI device intercepted a message (e.g. rate / payload limit exceeded).
static void on_error(const std::string& topic, const std::string& payload) {
    (void)topic;
    LOG_WARN("[MQTT] error event: %s", payload.c_str());
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------
static void on_connect(int rc) {
    if (rc != 0) {
        // rc == 5 usually means app_id / app_key / app_license failed validation.
        LOG_ERROR("[MQTT] connect failed, CONNACK=%d "
                  "(5 = credential validation failed)", rc);
        return;
    }
    LOG_INFO("[MQTT] connected (CONNACK=0)");
    // Hand control to the registered device module so it can report its
    // capability set and start its own workflow.
    MqttClient::getInstance().NotifyReady();
}

void MqttClient::Init() {
    MqttClientManager& mqtt = getMqttClientManager();

    // Register callbacks and subscriptions BEFORE connecting, so the very first
    // on_connect is never missed.
    mqtt.RegisterOnConnectCallback(on_connect);
    mqtt.Subscribe(TOPIC_STATE_REPLY,  QOS_1, on_state_reply);
    mqtt.Subscribe(TOPIC_EVENTS_REPLY, QOS_1, on_events_reply);
    mqtt.Subscribe(TOPIC_ERROR,        QOS_1, on_error);

    // Now start connecting.
    mqtt.Connect();
}

bool MqttClient::IsConnected() const {
    return getMqttClientManager().IsConnected();
}

bool MqttClient::WaitConnected(int timeout_ms) const {
    const int step_ms = 100;
    int waited = 0;
    while (!getMqttClientManager().IsConnected() && waited < timeout_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        waited += step_ms;
    }
    return getMqttClientManager().IsConnected();
}

bool MqttClient::CredentialsConfigured() {
    // Still the placeholder values shipped in app_info.h?
    return std::strcmp(USER_APP_ID, "your_app_id") != 0
        && std::strcmp(USER_APP_KEY, "your_app_key") != 0
        && std::strcmp(USER_APP_LICENSE, "your_app_license") != 0
        && std::strcmp(USER_DEVICE_ID, "your_device_id") != 0;
}
