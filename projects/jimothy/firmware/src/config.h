// Node configuration: /config.json on LittleFS. Secrets live here, never in source.
#pragma once
#include <Arduino.h>

struct Config {
    String node = "jimothy";
    struct { String ssid, password; } wifi;
    struct { String host = "10.0.0.1"; uint16_t port = 1883; String username = "eden-node", password; } mqtt;
    struct { String password; } ota;
    struct {
        int dry_raw = 0, wet_raw = 0;   // 0 = uncalibrated
        uint32_t interval_s = 60;
    } moisture;
    uint32_t env_interval_s = 60;       // BME688 publish cadence (BSEC itself runs every 3 s)
    uint32_t light_interval_s = 60;
    bool loaded = false;                // false → running on defaults, WiFi disabled
};

extern Config cfg;

bool configLoad();                      // mounts LittleFS, parses /config.json
bool configSave();                      // writes cfg back (used after calibration)
String configRedacted();                // JSON for the status page, secrets masked
