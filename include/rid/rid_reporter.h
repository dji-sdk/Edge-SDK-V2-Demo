/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 *********************************************************************
 */
#ifndef _RID_REPORTER_H_
#define _RID_REPORTER_H_

#include <string>
#include <vector>
#include <cstdint>

/*
 * RID reporter
 * ------------
 * Implements the RID device data flow defined by the ESDK 2.0 protocol:
 *
 *   1) Report the capability set   -> topic "state",  device_type = 7
 *   2) Report detected RID targets -> topic "events", method = "rid_infos"
 *   3) Report detected ADS-B targets -> topic "events", method = "adsb_infos"
 *
 * Each function builds a protocol-compliant JSON payload and publishes it
 * through the shared MqttClientManager.
 */

/* One detected drone (Remote ID target). */
struct RidTarget {
    std::string osid;          // drone ID
    double      rssi   = 0;    // signal strength (higher = stronger)
    int         fre    = 0;    // channel: 0 WIFI 5.8G, 1 WIFI 2.4G, 2 Bluetooth
    int         uatype = 0;    // UA type, see protocol doc
    int         status = 0;    // 0 unknown, 1 ground, 2 air, 3 emergency
    double      heading = 0;   // track angle, 361 = unknown/hovering
    double      speed   = 0;   // horizontal speed, m/s
    double      uprate  = 0;   // vertical speed, m/s
    double      lon = 0, lat = 0;        // drone position
    double      altgeo = 0, altbaro = 0; // geodetic / barometric altitude, -1000 = unknown
    double      height = 0;              // height above ground, -1000 = unknown
    double      op_lon = 0, op_lat = 0, op_alt = 0; // operator position
    int64_t     uatime = 0;             // drone internal time
};

/* One detected ADS-B aircraft. */
struct AdsbTarget {
    std::string icao;       // 24-bit ICAO address
    std::string callsign;   // aircraft callsign
    double      height  = 0; // altitude
    double      lat = 0, lon = 0;
    double      speed   = 0; // ground speed
    double      heading = 0;
    double      uprate  = 0; // vertical rate
};

/* The reporting device's own status, shared by rid_infos / adsb_infos. */
struct ReporterStatus {
    int    capacity_percent = 0; // battery percentage [0,100]
    int    power_supply     = 0; // power-supply flag
    // GNSS (the reporting device's own location)
    int    gnss_engine    = 0;   // 0 hybrid, 1 GPS, 2 BeiDou
    int    gnss_flag      = 0;   // 0 none, 1 SPS, 2 differential, 3 PPS
    std::string gnss_time = "";  // UTC, e.g. "20250723-142355"
    double gnss_lat = 0, gnss_lon = 0, gnss_alt = 0;
    int    gnss_satellite = 0;
    double gnss_hdop = 0;
};

/* 1) Report capability set: device_type = 7, supports rid + adsb. */
void rid_report_capabilities();

/* 2) Report a batch of detected RID targets (method = rid_infos). */
void rid_report_rid_infos(const ReporterStatus& status,
                          const std::vector<RidTarget>& targets);

/* 3) Report a batch of detected ADS-B targets (method = adsb_infos). */
void rid_report_adsb_infos(const ReporterStatus& status,
                           const std::vector<AdsbTarget>& targets);

/* Demo helpers: build one sample target and report it (used by the menu). */
void rid_report_demo_rid();
void rid_report_demo_adsb();

/*
 * Recommended pattern: cache detected targets and flush them on a fixed
 * cadence (every 2 seconds) instead of publishing one event per detection,
 * which would exceed the 5 Hz rate limit.
 */
void rid_cache_target(const RidTarget& t);       // cache one RID target
void rid_cache_adsb_target(const AdsbTarget& t); // cache one ADS-B target
void rid_start_periodic_report();                // start the 2s flush thread
void rid_stop_periodic_report();                 // stop the flush thread

/*
 * Interactive submenu for the RID module. Entered from the main menu.
 * Returns when the user chooses "back".
 */
void rid_menu();

/*
 * Register this device module with the connection layer: on connect, the RID
 * capability set is reported automatically. Call once at startup.
 */
void rid_module_register();

#endif // _RID_REPORTER_H_
