/**
 ********************************************************************
 *
 * @copyright (c) 2026 DJI. All rights reserved.
 *
 *********************************************************************
 *
 * ESDK 2.0 access sample
 * ----------------------
 * A generic framework showing how a third-party device connects to the DJI
 * device over MQTT and reports data:
 *
 *   - mqtt_client/  : device-agnostic connection layer (connect, auth, topics,
 *                     generic capability / event reporting).
 *   - module/       : per-device implementations built on top of that layer.
 *                     RID is provided as the first example device.
 *
 * Flow:
 *   1. Connect to the DJI device and authenticate (see app_info.h).
 *   2. On connect, the registered device module reports its capability set.
 *   3. Pick a device module from the main menu; each has its own submenu.
 *
 * Adding a new device module later:
 *   - implement the module under module/<your_device>/ with:
 *       * a register function that calls MqttClient::RegisterOnReady(...)
 *         to report its own capability set on connect
 *       * a `void xxx_menu()` for its interactive submenu
 *   - register it in main() and add one row to kModules[] below
 */
#include <iostream>
#include <vector>
#include <functional>

#include "mqtt/mqtt_client.h"
#include "mqtt/broker_discovery.h"
#include "rid/rid_reporter.h"
#include "common/log.h"

// A device module shown in the main menu.
struct DeviceModule {
    char                  key;     // menu hotkey
    const char*           name;    // display name
    std::function<void()> enter;   // submenu entry
};

// Registry of device modules. Add new modules here, one row per module.
static const std::vector<DeviceModule> kModules = {
    { '1', "RID device", rid_menu },
};

static void print_main_menu() {
    const bool connected = MqttClient::getInstance().IsConnected();
    std::cout << "\n========== ESDK 2.0 Sample ==========\n"
              << " Status: " << (connected ? "[Connected]" : "[Disconnected]") << "\n"
              << "-------------------------------------\n";
    for (const auto& m : kModules) {
        std::cout << " [" << m.key << "] " << m.name << "\n";
    }
    std::cout << " [q] Quit\n"
              << "=====================================\n"
              << "Select: ";
}

int main() {
    log_init(LOG_LEVEL_DEBUG);

    // Sanity check: remind the user to fill in credentials before anything else.
    if (!MqttClient::CredentialsConfigured()) {
        std::cout
            << "\n[!] Credentials are not configured.\n"
            << "    Please edit  include/mqtt/app_info.h  and fill in your\n"
            << "    app_id / app_key / app_license / device_id, then rebuild.\n"
            << "    (The sample will still start, but it cannot connect.)\n"
            << std::endl;
    }

    // Register the device module(s). On connect, each registered module reports
    // its own capability set. (Add more modules here as needed.)
    rid_module_register();

    // Resolve the broker address: listen for the DJI device UDP broadcast on
    // port 18830, or fall back to the default. Must run before the first
    // getMqttClientManager() call inside Init().
    mqtt_resolve_broker_address_interactive();

    // Connect to the DJI device.
    MqttClient::getInstance().Init();

    // Give the async connection a moment to settle before showing the menu,
    // so the status line is meaningful and logs do not race with the prompt.
    std::cout << "Connecting to the DJI device..." << std::endl;
    if (MqttClient::getInstance().WaitConnected(5000)) {
        std::cout << "Connected." << std::endl;
    } else {
        std::cout << "Not connected yet (will keep retrying in the background).\n"
                  << "Check the credentials and the network, see README troubleshooting.\n"
                  << std::endl;
    }

    char cmd;
    while (true) {
        print_main_menu();
        if (!(std::cin >> cmd)) break;

        if (cmd == 'q' || cmd == 'Q') {
            std::cout << "Bye." << std::endl;
            break;
        }

        // Reporting requires an active connection.
        if (!MqttClient::getInstance().IsConnected()) {
            std::cout << "[!] Not connected yet, please wait and try again.\n";
            continue;
        }

        bool handled = false;
        for (const auto& m : kModules) {
            if (m.key == cmd) {
                m.enter();
                handled = true;
                break;
            }
        }
        if (!handled) {
            std::cout << "Unknown command: " << cmd << std::endl;
        }
    }
    return 0;
}
