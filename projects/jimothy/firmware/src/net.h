// Network side: WiFi, mDNS, ArduinoOTA, MQTT (Eden contract), status web page, SNTP.
#pragma once
#include <Arduino.h>

// Latest readings, shared between the sensor code (writer) and MQTT/web (readers).
struct Readings {
    int moist_raw = -1; float moist_pct = NAN; uint32_t moist_ms = 0;
    float t = NAN, rh = NAN, p = NAN, iaq = NAN, siaq = NAN, eco2 = NAN, bvoc = NAN;
    int iaq_acc = -1; uint32_t env_ms = 0;
    float lux = NAN, uvi = NAN; uint32_t light_ms = 0;
    bool bme_ok = false, ltr_ok = false;
};
extern Readings rd;

// Sensor-side hooks the web/MQTT layer can call.
struct NetHooks {
    void (*calibrate)(bool wet) = nullptr;   // run a dry/wet capture, update cfg, save
    void (*applyConfig)() = nullptr;         // re-read intervals/calibration after reload
};
extern NetHooks hooks;

void netBegin();
void netLoop();
bool netWifiUp();
bool netMqttUp();
bool netOtaBusy();            // true while an OTA image is being written — sensors should idle
uint32_t netNow();            // unix seconds, 0 until SNTP syncs (contract: edend falls back)
String netStatusLine();       // one-line summary for serial

// Publish eden/<node>/sensor/<name> per docs/mqtt-contract.md. `extra` is appended raw
// JSON fields (e.g. "\"acc\":3"), may be nullptr. No-op when MQTT is down.
void netPublishSensor(const char* name, float v, int raw, const char* unit, const char* extra = nullptr);
