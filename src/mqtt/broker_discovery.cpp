/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 *********************************************************************
 */
#include "mqtt/broker_discovery.h"
#include "common/log.h"

#include <cstring>
#include <cstdint>
#include <map>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

// ---------------------------------------------------------------------------
// Discovery protocol constants (must match the DJI device broadcast)
// ---------------------------------------------------------------------------
#define DISC_PORT       18830
#define DISC_MAGIC      "MQTTBRK|"
#define DISC_MAGIC_LEN  8

// ---------------------------------------------------------------------------
// Resolved broker address (process-wide)
// ---------------------------------------------------------------------------
static std::string& mutable_broker_address() {
    static std::string s_broker_ip = DEFAULT_BROKER_ADDRESS;
    return s_broker_ip;
}

const std::string& mqtt_broker_address() {
    return mutable_broker_address();
}

void mqtt_set_broker_address(const std::string& ip) {
    mutable_broker_address() = ip;
}

// ---------------------------------------------------------------------------
// Broadcast frame parsing
// ---------------------------------------------------------------------------
struct BrokerInfo {
    std::string id;
    std::string name;
    std::string ip;
    uint32_t    seq   = 0;
    int         ver   = 0;
    bool        valid = false;
};

static int safe_stoi(const std::string& s, int def = 0) {
    try { return std::stoi(s); } catch (...) { return def; }
}

// Parse "MQTTBRK|v=1|id=...|name=...|ip=...|seq=..." into a BrokerInfo.
static BrokerInfo parse_discover_message(const std::string& msg) {
    BrokerInfo info{};
    if (msg.rfind(DISC_MAGIC, 0) != 0) {
        LOG_DEBUG("[discover] parse: magic mismatch (need 'MQTTBRK|')");
        return info;
    }

    std::map<std::string, std::string> kv;
    std::stringstream ss(msg.substr(DISC_MAGIC_LEN));
    std::string tok;
    while (std::getline(ss, tok, '|')) {
        auto p = tok.find('=');
        if (p == std::string::npos) continue;
        kv[tok.substr(0, p)] = tok.substr(p + 1);
    }

    info.ver = safe_stoi(kv["v"]);
    if (info.ver != 1) {
        LOG_WARN("[discover] parse: unsupported version=%d", info.ver);
        return info;
    }
    info.id    = kv["id"];
    info.name  = kv["name"];
    info.ip    = kv["ip"];
    info.seq   = static_cast<uint32_t>(safe_stoi(kv["seq"]));
    info.valid = !info.id.empty() && !info.ip.empty();
    if (!info.valid) {
        LOG_WARN("[discover] parse: invalid (id_empty=%d ip_empty=%d)",
                 info.id.empty(), info.ip.empty());
    }
    return info;
}

// ---------------------------------------------------------------------------
// Blocking UDP listen until a valid broadcast arrives (socket error -> invalid)
// ---------------------------------------------------------------------------
static BrokerInfo discover_broker_forever() {
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG_ERROR("[discover] socket failed: %s", strerror(errno));
        return {};
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(DISC_PORT);
    addr.sin_addr.s_addr = INADDR_ANY; // bind 0.0.0.0 to receive broadcasts
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("[discover] bind 0.0.0.0:%d failed: %s", DISC_PORT, strerror(errno));
        ::close(sock);
        return {};
    }

    LOG_INFO("[discover] listening on UDP %d (no timeout), waiting for broadcast...",
             DISC_PORT);
    LOG_INFO("[discover] tip: sudo tcpdump -i any udp port %d -A   to verify packets",
             DISC_PORT);

    // 5s receive timeout, used only to print a heartbeat so the user knows we
    // are alive; the overall wait has no timeout.
    timeval tv{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[4096];
    sockaddr_in src{};
    socklen_t   slen = sizeof(src);
    uint64_t    heartbeat = 0;

    while (true) {
        slen = sizeof(src);
        ssize_t len = ::recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&src), &slen);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_INFO("[discover] heartbeat #%llu: still waiting on UDP %d ...",
                         static_cast<unsigned long long>(++heartbeat), DISC_PORT);
                continue;
            }
            LOG_WARN("[discover] recvfrom error: %s, keep trying", strerror(errno));
            continue;
        }
        if (len == 0) continue;
        buf[len] = '\0';

        char src_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
        LOG_INFO("[discover] recv %zd bytes from %s:%u", len, src_ip, ntohs(src.sin_port));

        BrokerInfo info = parse_discover_message(std::string(buf, len));
        if (!info.valid) {
            LOG_INFO("[discover] -> dropped, keep waiting");
            continue;
        }
        LOG_INFO("[discover] -> valid: id=%s name=%s ip=%s seq=%u",
                 info.id.c_str(), info.name.c_str(), info.ip.c_str(), info.seq);
        ::close(sock);
        return info;
    }
}

// ---------------------------------------------------------------------------
// Interactive startup choice: listen for discovery, or use default
// ---------------------------------------------------------------------------
void mqtt_resolve_broker_address_interactive() {
    std::cout << "\n==== MQTT Broker setup ====\n"
              << "  [1] listen UDP " << DISC_PORT
              << " until a valid broadcast arrives\n"
              << "  [2] skip discovery, use default "
              << DEFAULT_BROKER_ADDRESS << ":1883\n"
              << "select [1/2]: " << std::flush;

    std::string choice;
    if (!std::getline(std::cin, choice)) {
        LOG_WARN("[discover] read stdin failed, use default");
        mqtt_set_broker_address(DEFAULT_BROKER_ADDRESS);
        return;
    }

    if (choice == "1") {
        BrokerInfo info = discover_broker_forever();
        if (info.valid) {
            LOG_INFO("[discover] found device name=%s id=%s -> %s",
                     info.name.c_str(), info.id.c_str(), info.ip.c_str());
            mqtt_set_broker_address(info.ip);
        } else {
            LOG_WARN("[discover] discover failed, fallback to %s", DEFAULT_BROKER_ADDRESS);
            mqtt_set_broker_address(DEFAULT_BROKER_ADDRESS);
        }
    } else {
        LOG_INFO("[discover] skip discovery, use %s", DEFAULT_BROKER_ADDRESS);
        mqtt_set_broker_address(DEFAULT_BROKER_ADDRESS);
    }
}
