#include "config.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

Config cfg;
static const char* kPath = "/config.json";

bool configLoad() {
    if (!LittleFS.begin(false)) {
        Serial.println("!! LittleFS mount failed — run `pio run -t uploadfs` with data/config.json");
        return false;
    }
    File f = LittleFS.open(kPath, "r");
    if (!f) {
        Serial.println("!! /config.json missing — copy data/config.example.json → data/config.json, edit, uploadfs");
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("!! /config.json parse error: %s\n", err.c_str());
        return false;
    }
    cfg.node = doc["node"] | "jimothy";
    cfg.wifi.ssid = doc["wifi"]["ssid"] | "";
    cfg.wifi.password = doc["wifi"]["password"] | "";
    cfg.mqtt.host = doc["mqtt"]["host"] | "10.0.0.1";
    cfg.mqtt.port = doc["mqtt"]["port"] | 1883;
    cfg.mqtt.username = doc["mqtt"]["username"] | "eden-node";
    cfg.mqtt.password = doc["mqtt"]["password"] | "";
    cfg.ota.password = doc["ota"]["password"] | "";
    cfg.moisture.dry_raw = doc["sensors"]["moisture"]["dry_raw"] | 0;
    cfg.moisture.wet_raw = doc["sensors"]["moisture"]["wet_raw"] | 0;
    cfg.moisture.interval_s = doc["sensors"]["moisture"]["interval_s"] | 60;
    cfg.env_interval_s = doc["sensors"]["env"]["interval_s"] | 60;
    cfg.light_interval_s = doc["sensors"]["light"]["interval_s"] | 60;
    cfg.loaded = true;
    return true;
}

bool configSave() {
    JsonDocument doc;
    doc["node"] = cfg.node;
    doc["wifi"]["ssid"] = cfg.wifi.ssid;
    doc["wifi"]["password"] = cfg.wifi.password;
    doc["mqtt"]["host"] = cfg.mqtt.host;
    doc["mqtt"]["port"] = cfg.mqtt.port;
    doc["mqtt"]["username"] = cfg.mqtt.username;
    doc["mqtt"]["password"] = cfg.mqtt.password;
    doc["ota"]["password"] = cfg.ota.password;
    doc["sensors"]["moisture"]["dry_raw"] = cfg.moisture.dry_raw;
    doc["sensors"]["moisture"]["wet_raw"] = cfg.moisture.wet_raw;
    doc["sensors"]["moisture"]["interval_s"] = cfg.moisture.interval_s;
    doc["sensors"]["env"]["interval_s"] = cfg.env_interval_s;
    doc["sensors"]["light"]["interval_s"] = cfg.light_interval_s;

    // Write to a temp file then rename so a power cut mid-write can't lose the config.
    File f = LittleFS.open("/config.tmp", "w");
    if (!f) return false;
    serializeJsonPretty(doc, f);
    f.close();
    LittleFS.remove(kPath);
    return LittleFS.rename("/config.tmp", kPath);
}

String configRedacted() {
    JsonDocument doc;
    doc["node"] = cfg.node;
    doc["wifi"]["ssid"] = cfg.wifi.ssid;
    doc["mqtt"]["host"] = cfg.mqtt.host;
    doc["mqtt"]["port"] = cfg.mqtt.port;
    doc["mqtt"]["username"] = cfg.mqtt.username;
    doc["ota"]["enabled"] = cfg.ota.password.length() > 0;
    doc["sensors"]["moisture"]["dry_raw"] = cfg.moisture.dry_raw;
    doc["sensors"]["moisture"]["wet_raw"] = cfg.moisture.wet_raw;
    doc["sensors"]["moisture"]["interval_s"] = cfg.moisture.interval_s;
    doc["sensors"]["env"]["interval_s"] = cfg.env_interval_s;
    doc["sensors"]["light"]["interval_s"] = cfg.light_interval_s;
    String out;
    serializeJson(doc, out);
    return out;
}
