/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 *********************************************************************
 *
 * RID device module
 * -----------------
 * An example device implementation built on top of the generic mqtt_client
 * layer. It declares its own device_type / capabilities and builds the
 * RID-specific event payloads, but relies on the generic
 * mqtt_report_capabilities() / mqtt_publish_event() for transport.
 */
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>

#include "rid/rid_reporter.h"
#include "json/json.hpp"
#include "common/log.h"
#include "mqtt/mqtt_client.h"

using json = nlohmann::json;

static const int kRidDeviceType = 7; // device_type for RID devices

/* Serialize the reporting device's own status (battery / power / GNSS). */
static json status_to_json(const ReporterStatus& s) {
    json data;
    data["capacity_percent"] = s.capacity_percent;
    data["power_supply"]     = s.power_supply;
    data["gnss"] = {
        {"engine",    s.gnss_engine},
        {"flag",      s.gnss_flag},
        {"time",      s.gnss_time},
        {"lat",       s.gnss_lat},
        {"lon",       s.gnss_lon},
        {"alt",       s.gnss_alt},
        {"satellite", s.gnss_satellite},
        {"hdop",      s.gnss_hdop},
    };
    return data;
}

// ---------------------------------------------------------------------------
// 1) Capability set
// ---------------------------------------------------------------------------
void rid_report_capabilities() {
    json capabilities = {
        {"rid",  {{"supported", true}, {"version", "1.0.0"}}},
        {"adsb", {{"supported", true}, {"version", "1.0.0"}}},
    };
    mqtt_report_capabilities(kRidDeviceType, capabilities);
}

// ---------------------------------------------------------------------------
// 2) rid_infos
// ---------------------------------------------------------------------------
void rid_report_rid_infos(const ReporterStatus& status,
                          const std::vector<RidTarget>& targets) {
    json data = status_to_json(status);
    json rid_array = json::array();
    for (const auto& t : targets) {
        rid_array.push_back({
            {"osid",   t.osid},   {"rssi",    t.rssi},   {"fre",     t.fre},
            {"uatype", t.uatype}, {"status",  t.status}, {"heading", t.heading},
            {"speed",  t.speed},  {"uprate",  t.uprate},
            {"lon",    t.lon},    {"lat",     t.lat},
            {"altgeo", t.altgeo}, {"altbaro", t.altbaro},{"height",  t.height},
            {"op_lon", t.op_lon}, {"op_lat",  t.op_lat}, {"op_alt",  t.op_alt},
            {"uatime", t.uatime},
        });
    }
    data["rid"] = std::move(rid_array);

    LOG_INFO("[RID] report rid_infos (%zu target(s))", targets.size());
    mqtt_publish_event("rid_infos", data);
}

// ---------------------------------------------------------------------------
// 3) adsb_infos
// ---------------------------------------------------------------------------
void rid_report_adsb_infos(const ReporterStatus& status,
                           const std::vector<AdsbTarget>& targets) {
    json data = status_to_json(status);
    json adsb_array = json::array();
    for (const auto& t : targets) {
        adsb_array.push_back({
            {"icao",    t.icao},    {"callsign", t.callsign}, {"height",  t.height},
            {"lat",     t.lat},     {"lon",      t.lon},      {"speed",   t.speed},
            {"heading", t.heading}, {"uprate",   t.uprate},
        });
    }
    data["adsb"] = std::move(adsb_array);

    LOG_INFO("[RID] report adsb_infos (%zu target(s))", targets.size());
    mqtt_publish_event("adsb_infos", data);
}

// ---------------------------------------------------------------------------
// Demo helpers
// ---------------------------------------------------------------------------
static ReporterStatus make_demo_status() {
    ReporterStatus s;
    s.capacity_percent = 48;
    s.power_supply     = 0;
    s.gnss_engine = 0;
    s.gnss_flag   = 2;
    s.gnss_time   = "20250723-142355";
    s.gnss_lat = 22.53450;
    s.gnss_lon = 113.95360;
    s.gnss_alt = 503;
    s.gnss_satellite = 5;
    s.gnss_hdop = 5;
    return s;
}

void rid_report_demo_rid() {
    RidTarget t;
    t.osid    = "1581F6N8C243L0039TB9";
    t.rssi    = -79;
    t.fre     = 1;
    t.uatype  = 2;
    t.status  = 0;
    t.heading = 12;
    t.speed   = 5.4;
    t.uprate  = 1.5;
    t.lon = 113.95749; t.lat = 22.53854;
    t.altgeo = 120; t.altbaro = 120; t.height = 50;
    t.op_lon = 113.95856; t.op_lat = 22.53980; t.op_alt = 20;
    t.uatime = 174462011;

    rid_report_rid_infos(make_demo_status(), {t});
}

void rid_report_demo_adsb() {
    AdsbTarget t;
    t.icao     = "781EB7";
    t.callsign = "CSN1735";
    t.height   = 11003;
    t.lat = 22.53450; t.lon = 113.95360;
    t.speed   = 941;
    t.heading = 185;
    t.uprate  = 0.32;

    rid_report_adsb_infos(make_demo_status(), {t});
}

// ---------------------------------------------------------------------------
// Periodic reporting (recommended pattern)
// ---------------------------------------------------------------------------
//
// The DJI device limits the message rate to 5 Hz. Do NOT publish one event
// per detection; instead, cache detected targets and flush them on a fixed
// cadence (the protocol recommends every 2 seconds). The thread below caches
// targets and reports rid_infos / adsb_infos once per period.
//
static const int kReportPeriodSec = 2;

static std::thread             g_report_thread;
static std::atomic<bool>       g_reporting{false};
static std::mutex              g_cache_mutex;
static std::vector<RidTarget>  g_rid_cache;
static std::vector<AdsbTarget> g_adsb_cache;

// Cache one detected target (call this whenever the radio detects something).
void rid_cache_target(const RidTarget& t) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_rid_cache.push_back(t);
}
void rid_cache_adsb_target(const AdsbTarget& t) {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_adsb_cache.push_back(t);
}

static void report_loop() {
    while (g_reporting) {
        std::this_thread::sleep_for(std::chrono::seconds(kReportPeriodSec));

        std::vector<RidTarget>  rid_batch;
        std::vector<AdsbTarget> adsb_batch;
        {
            std::lock_guard<std::mutex> lock(g_cache_mutex);
            rid_batch.swap(g_rid_cache);
            adsb_batch.swap(g_adsb_cache);
        }
        // If nothing was cached this cycle, fall back to demo targets so the
        // periodic report keeps producing output after a single keypress.
        if (rid_batch.empty() && adsb_batch.empty()) {
            rid_report_demo_rid();
            rid_report_demo_adsb();
            continue;
        }
        // Flush each batch as a single message (skip if nothing cached).
        if (!rid_batch.empty())  rid_report_rid_infos(make_demo_status(), rid_batch);
        if (!adsb_batch.empty()) rid_report_adsb_infos(make_demo_status(), adsb_batch);
    }
}

void rid_start_periodic_report() {
    if (g_reporting) {
        std::cout << "Periodic report already running.\n";
        return;
    }
    g_reporting = true;
    g_report_thread = std::thread(report_loop);
    LOG_INFO("[RID] periodic report started (every %ds)", kReportPeriodSec);
}

void rid_stop_periodic_report() {
    if (!g_reporting) return;
    g_reporting = false;
    if (g_report_thread.joinable()) g_report_thread.join();
    LOG_INFO("[RID] periodic report stopped");
}

// ---------------------------------------------------------------------------
// Interactive submenu
// ---------------------------------------------------------------------------
void rid_menu() {
    char cmd;
    while (true) {
        std::cout
            << "\n----- RID device access -----\n"
            << " [1] Report capability set        (state,  device_type=7) [run first]\n"
            << " [2] Report one RID  target now   (events, method=rid_infos)\n"
            << " [3] Report one ADS-B target now  (events, method=adsb_infos)\n"
            << " [4] Start periodic report (every 2s, demo RID/ADS-B targets)\n"
            << " [5] Stop periodic report\n"
            << " [b] Back\n"
            << "Select: ";
        if (!(std::cin >> cmd)) return;

        switch (cmd) {
            case '1': rid_report_capabilities(); break;
            case '2': rid_report_demo_rid();     break;
            case '3': rid_report_demo_adsb();    break;
            case '4': {
                // One keypress: start the periodic thread. The loop reports demo
                // RID/ADS-B targets every 2s (and any real cached targets too).
                rid_start_periodic_report();
                std::cout << "Periodic report running; demo RID/ADS-B targets "
                             "will be reported every 2s.\n";
                break;
            }
            case '5': rid_stop_periodic_report();  break;
            case 'b':
            case 'B': rid_stop_periodic_report(); return;
            default:  std::cout << "Unknown command: " << cmd << std::endl; break;
        }
    }
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------
void rid_module_register() {
    // On connect, report the RID capability set automatically.
    MqttClient::getInstance().RegisterOnReady(rid_report_capabilities);
}
