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
#ifndef MQTT_CLIENT_MANAGER_H
#define MQTT_CLIENT_MANAGER_H

#pragma once

#include <mosquittopp.h>
#include <string>
#include <map>
#include <mutex>
#include <functional>
#include <atomic>
#include <vector>

using OnConnectCallback    = std::function<void(int rc)>;
using OnDisconnectCallback = std::function<void(int rc)>;
using MqttMessageCallback  = std::function<void(const std::string& topic, const std::string& payload)>;

struct SubscriptionInfo {
    std::string         topic;
    int                 qos;
    MqttMessageCallback callback;
};

/*
 * A thin wrapper over mosquittopp that:
 *   - connects asynchronously and runs the network loop in a background thread
 *   - uses clean_session (true) with a 30s keepalive
 *   - auto-reconnects with exponential backoff (1s..30s), and re-subscribes
 *     on every (re)connect since clean_session clears the session
 *   - stops reconnecting on auth failure (CONNACK=5)
 *   - dispatches each incoming message to the callback registered for its topic
 */
class MqttClientManager : public mosqpp::mosquittopp {
public:
    MqttClientManager(const std::string& client_id,
                      const std::string& host,
                      int port,
                      const std::string& username,
                      const std::string& password);
    ~MqttClientManager();

    // Start connecting to the broker (async) and run the network loop thread.
    // Call this AFTER registering callbacks and subscriptions so no early
    // on_connect is missed.
    void Connect();

    // Register a topic subscription and its handler. Takes effect once connected.
    bool Subscribe(const std::string& topic, int qos, MqttMessageCallback callback);

    // Publish a message. Returns silently if not connected yet.
    void Publish(const std::string& topic, const std::string& payload,
                 int qos = 1, bool retain = false);

    bool IsConnected() const { return connected_; }

    void RegisterOnConnectCallback(OnConnectCallback cb)       { onConnectCallback_ = std::move(cb); }
    void RegisterOnDisconnectCallback(OnDisconnectCallback cb) { onDisconnectCallback_ = std::move(cb); }

protected:
    void on_connect(int rc) override;
    void on_disconnect(int rc) override;
    void on_message(const struct mosquitto_message* message) override;

private:
    void subscribeAll(); // (re)subscribe everything in subscriptions_

    std::string       host_;
    int               port_;
    std::string       client_id_;
    std::string       username_;
    std::atomic<bool> loop_started_{false};
    std::atomic<bool> auto_reconnect_{true};
    std::atomic<bool> connected_{false};

    std::mutex                              mutex_;
    std::vector<SubscriptionInfo>           subscriptions_;     // desired subscriptions
    std::map<std::string, MqttMessageCallback> topicCallbacks_; // topic -> handler

    OnConnectCallback    onConnectCallback_;
    OnDisconnectCallback onDisconnectCallback_;
};

#endif // MQTT_CLIENT_MANAGER_H
