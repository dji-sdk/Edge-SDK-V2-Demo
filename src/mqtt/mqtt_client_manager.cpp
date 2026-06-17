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
#include "mqtt/mqtt_client_manager.h"
#include "common/log.h"

MqttClientManager::MqttClientManager(const std::string& client_id, const std::string& host,
                                     int port, const std::string& username, const std::string& password)
    : mosquittopp(client_id.c_str()), host_(host), port_(port)
{
    // The base mosquittopp(client_id) uses clean_session = true. This suits RID:
    // events are real-time, so we do NOT want the broker to replay messages
    // queued while we were offline. Because the session is cleared on every
    // (re)connect, subscriptions MUST be re-issued in on_connect() -> see below.
    mosqpp::lib_init();
    username_pw_set(username.c_str(), password.c_str());
    client_id_ = client_id;
    username_  = username;
}

void MqttClientManager::Connect()
{
    if (loop_started_) return; // already connecting
    loop_started_ = true;

    // Automatic reconnect with exponential backoff, handled by the network
    // loop thread: 1s, 2s, 4s, ... capped at 30s. This avoids hammering the
    // broker while it is recovering, and removes the need to sleep/reconnect
    // manually inside on_disconnect().
    reconnect_delay_set(/*reconnect_delay=*/1, /*reconnect_delay_max=*/30,
                        /*exponential_backoff=*/true);

    // KeepAlive 30s: the link is a local LAN, so a shorter keepalive detects a
    // dead connection faster without burdening the DJI device.
    const int kKeepAliveSec = 30;
    LOG_INFO("[MQTT] connecting to %s:%d (client_id=%s, username=%s, keepalive=%ds)",
             host_.c_str(), port_, client_id_.c_str(), username_.c_str(), kKeepAliveSec);
    connect_async(host_.c_str(), port_, kKeepAliveSec);
    loop_start();
}

MqttClientManager::~MqttClientManager() {
    auto_reconnect_ = false;
    if (loop_started_) {
        disconnect();
        loop_stop();
    }
    mosqpp::lib_cleanup();
}

bool MqttClientManager::Subscribe(const std::string& topic, int qos, MqttMessageCallback callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.push_back({topic, qos, callback});
        topicCallbacks_[topic] = std::move(callback);
    }
    // If already connected, subscribe right away; otherwise on_connect will do it.
    if (connected_) {
        subscribe(nullptr, topic.c_str(), qos);
    }
    return true;
}

void MqttClientManager::Publish(const std::string& topic, const std::string& payload, int qos, bool retain) {
    if (!connected_) {
        LOG_WARN("[MQTT] not connected, drop publish to %s", topic.c_str());
        return;
    }
    int ret = publish(nullptr, topic.c_str(), payload.size(), payload.c_str(), qos, retain);
    LOG_INFO("[MQTT] publish -> %s (qos=%d, ret=%d): %s",
             topic.c_str(), qos, ret, payload.c_str());
}

void MqttClientManager::subscribeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& sub : subscriptions_) {
        subscribe(nullptr, sub.topic.c_str(), sub.qos);
        LOG_INFO("[MQTT] subscribe %s (qos=%d)", sub.topic.c_str(), sub.qos);
    }
}

void MqttClientManager::on_connect(int rc) {
    connected_ = (rc == 0);
    if (rc == 0) {
        // clean_session clears the session on every connect, so (re)subscribe.
        subscribeAll();
    } else if (rc == 5) {
        // Authentication failed (bad app_id / app_key / app_license). Retrying
        // will not help, so stop the automatic reconnect to avoid busy-looping.
        LOG_ERROR("[MQTT] auth failed (CONNACK=5), stop reconnecting; check credentials");
        auto_reconnect_ = false;
        disconnect(); // tells the loop thread not to reconnect
    }
    if (onConnectCallback_) onConnectCallback_(rc);
}

void MqttClientManager::on_disconnect(int rc) {
    connected_ = false;
    if (auto_reconnect_) {
        // The network loop thread reconnects automatically using the backoff
        // configured via reconnect_delay_set(); nothing to do here.
        LOG_WARN("[MQTT] disconnected (rc=%d), will auto-reconnect with backoff", rc);
    } else {
        LOG_INFO("[MQTT] disconnected (rc=%d), auto-reconnect disabled", rc);
    }
    if (onDisconnectCallback_) onDisconnectCallback_(rc);
}

void MqttClientManager::on_message(const struct mosquitto_message* msg) {
    if (!msg || !msg->payload) return;

    MqttMessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = topicCallbacks_.find(msg->topic);
        if (it != topicCallbacks_.end()) cb = it->second;
    }
    if (cb) {
        cb(std::string(msg->topic), std::string((char*)msg->payload, msg->payloadlen));
    }
}
