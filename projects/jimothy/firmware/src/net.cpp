#include "net.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <MQTT.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "config.h"
#include "eden_logic.h"

#ifndef EDEN_FW_VERSION
#define EDEN_FW_VERSION "0.0.0"
#endif

Readings rd;
NetHooks hooks;

static WiFiClient tcp;
static MQTTClient mqtt(1024);
static WebServer http(80);
static eden::CmdDedupe dedupe;

static bool otaBusy = false;
static bool mdnsUp = false;
static bool appMarkedValid = false;
static uint32_t wifiLastAttempt = 0, wifiBackoff = 5000;
static uint32_t mqttLastAttempt = 0, lastTelemetry = 0;
static uint32_t wifiDrops = 0, mqttDrops = 0;
static bool wifiWasUp = false, mqttWasUp = false;

static String topic(const char* suffix) { return "eden/" + cfg.node + "/" + suffix; }

// ---------- time ----------

uint32_t netNow() {
    time_t t = time(nullptr);
    return t > 1600000000 ? static_cast<uint32_t>(t) : 0;
}

// ---------- WiFi ----------

bool netWifiUp() { return WiFi.status() == WL_CONNECTED; }

static void wifiKick() {
    wifiLastAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(cfg.wifi.ssid.c_str(), cfg.wifi.password.c_str());
    Serial.printf("wifi: connecting to \"%s\" (retry in %lus)\n", cfg.wifi.ssid.c_str(),
                  static_cast<unsigned long>(wifiBackoff / 1000));
}

static void onWifiUp() {
    wifiBackoff = 5000;
    Serial.printf("wifi: up %s rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    if (!mdnsUp) {
        mdnsUp = MDNS.begin(cfg.node.c_str());
        if (mdnsUp) MDNS.addService("http", "tcp", 80);
        Serial.printf("mdns: %s.local %s\n", cfg.node.c_str(), mdnsUp ? "up" : "FAILED");
    }
}

// ---------- MQTT ----------

bool netMqttUp() { return mqtt.connected(); }

static void publishStatus() {
    char buf[160];
    snprintf(buf, sizeof buf, "{\"online\":true,\"fw\":\"%s\",\"ip\":\"%s\"}", EDEN_FW_VERSION,
             WiFi.localIP().toString().c_str());
    mqtt.publish(topic("status"), buf, true, 1);
}

static void publishTelemetry() {
    char buf[200];
    snprintf(buf, sizeof buf, "{\"ts\":%lu,\"rssi\":%d,\"uptime_s\":%lu,\"heap\":%u,\"fw\":\"%s\"}",
             static_cast<unsigned long>(netNow()), WiFi.RSSI(),
             static_cast<unsigned long>(millis() / 1000), ESP.getFreeHeap(), EDEN_FW_VERSION);
    mqtt.publish(topic("telemetry"), buf, false, 0);
}

static void ack(const String& id, const char* state, const char* reason = nullptr) {
    char buf[160];
    if (reason)
        snprintf(buf, sizeof buf, "{\"id\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}", id.c_str(), state, reason);
    else
        snprintf(buf, sizeof buf, "{\"id\":\"%s\",\"state\":\"%s\"}", id.c_str(), state);
    mqtt.publish(topic("ack/") + id, buf, false, 1);
}

static void onCommand(String& t, String& payload) {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        Serial.printf("cmd: bad json on %s\n", t.c_str());
        return;
    }
    String id = doc["id"] | "";
    if (id.isEmpty()) { Serial.println("cmd: missing id"); return; }
    if (dedupe.seenBefore(id.c_str())) { Serial.printf("cmd: dup %s dropped\n", id.c_str()); return; }
    String action = doc["action"] | "";
    Serial.printf("cmd: %s id=%s action=%s\n", t.c_str(), id.c_str(), action.c_str());

    if (!t.endsWith("/cmd/system")) {
        // No pump/valve on this node yet.
        ack(id, "failed", "unknown_action");
        return;
    }
    ack(id, "accepted");
    if (action == "announce") {
        publishStatus();
        publishTelemetry();
        ack(id, "done");
    } else if (action == "reload") {
        bool ok = configLoad();
        if (ok && hooks.applyConfig) hooks.applyConfig();
        ack(id, ok ? "done" : "failed", ok ? nullptr : "bad_json");
    } else if (action == "reboot") {
        ack(id, "done");
        mqtt.loop();
        delay(200);
        ESP.restart();
    } else {
        ack(id, "failed", "unknown_action");
    }
}

static void mqttConnect() {
    mqttLastAttempt = millis();
    mqtt.begin(cfg.mqtt.host.c_str(), cfg.mqtt.port, tcp);
    mqtt.setOptions(60, true, 3000);
    mqtt.setWill(topic("status").c_str(), "{\"online\":false}", true, 1);
    mqtt.onMessage(onCommand);
    if (!mqtt.connect(cfg.node.c_str(), cfg.mqtt.username.c_str(), cfg.mqtt.password.c_str())) {
        Serial.printf("mqtt: connect failed (err %d, rc %d)\n", mqtt.lastError(), mqtt.returnCode());
        return;
    }
    mqtt.subscribe(topic("cmd/#"), 1);
    publishStatus();
    publishTelemetry();
    lastTelemetry = millis();
    Serial.printf("mqtt: connected to %s:%u as %s\n", cfg.mqtt.host.c_str(), cfg.mqtt.port, cfg.node.c_str());

    // OTA rollback safety: only call this image good once it has proven it can get online.
    if (!appMarkedValid) {
        esp_ota_mark_app_valid_cancel_rollback();
        appMarkedValid = true;
    }
}

void netPublishSensor(const char* name, float v, int raw, const char* unit, const char* extra) {
    if (!mqtt.connected()) return;
    char buf[200];
    int n = snprintf(buf, sizeof buf, "{\"ts\":%lu,\"v\":%.2f", static_cast<unsigned long>(netNow()), v);
    if (raw >= 0) n += snprintf(buf + n, sizeof buf - n, ",\"raw\":%d", raw);
    n += snprintf(buf + n, sizeof buf - n, ",\"unit\":\"%s\"", unit);
    if (extra && *extra) n += snprintf(buf + n, sizeof buf - n, ",%s", extra);
    snprintf(buf + n, sizeof buf - n, "}");
    mqtt.publish(topic("sensor/") + name, buf, false, 0);
}

// ---------- OTA ----------

bool netOtaBusy() { return otaBusy; }

static void otaBegin() {
    ArduinoOTA.setHostname(cfg.node.c_str());
    ArduinoOTA.setMdnsEnabled(false);  // we run MDNS ourselves
    if (cfg.ota.password.length()) ArduinoOTA.setPassword(cfg.ota.password.c_str());
    ArduinoOTA.onStart([] {
        otaBusy = true;
        Serial.println("ota: start");
        if (mqtt.connected()) mqtt.publish(topic("status"), "{\"online\":false,\"reason\":\"ota\"}", true, 1);
    });
    ArduinoOTA.onEnd([] { Serial.println("\nota: done, rebooting"); });
    ArduinoOTA.onProgress([](unsigned p, unsigned total) {
        static unsigned last = 101;
        unsigned pct = p * 100 / total;
        if (pct / 10 != last / 10) { Serial.printf("ota: %u%%\n", pct); last = pct; }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        otaBusy = false;
        Serial.printf("ota: error %d\n", e);
    });
    ArduinoOTA.begin();
}

// ---------- Web ----------

static const char kPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Jimothy</title>
<style>body{font:16px system-ui;margin:1.2em;background:#111;color:#eee;max-width:34em}
h1{font-size:1.4em;margin:0 0 .2em}small{color:#999}table{width:100%;border-collapse:collapse;margin-top:1em}
td{padding:.35em .2em;border-bottom:1px solid #333}td:last-child{text-align:right;font-variant-numeric:tabular-nums}
.big{font-size:2.6em;font-weight:600;margin:.4em 0}button{background:#333;color:#eee;border:0;padding:.5em .9em;margin:.3em .3em 0 0;border-radius:6px}
</style></head><body><h1>🥑 Jimothy</h1><small id=meta>…</small>
<div class=big id=moist>—</div><table id=t></table>
<p><button onclick="cal('dry')">capture DRY</button><button onclick="cal('wet')">capture WET</button>
<small>probe must be in air / in a cup of water first</small></p><pre id=log></pre>
<script>
const f=(v,d=1,u='')=>v==null?'—':v.toFixed(d)+u;
async function tick(){try{const s=await(await fetch('/api/state')).json();
document.getElementById('moist').textContent=s.moisture.pct==null?'raw '+s.moisture.raw:f(s.moisture.pct,0,'%')+' soil';
document.getElementById('meta').textContent=`${s.node} · fw ${s.fw} · ${s.wifi.ip} · rssi ${s.wifi.rssi} · mqtt ${s.mqtt?'up':'DOWN'} · up ${Math.floor(s.uptime_s/3600)}h`;
const r=[['moisture raw',s.moisture.raw],['temperature',f(s.env.t,1,' °C')],['humidity',f(s.env.rh,0,' %')],
['pressure',f(s.env.p,1,' hPa')],['IAQ',s.env.iaq==null?'—':`${s.env.iaq.toFixed(0)} (acc ${s.env.acc})`],
['eCO2',f(s.env.eco2,0,' ppm')],['bVOC',f(s.env.bvoc,2,' ppm')],['light',f(s.light.lux,0,' lux')],['UV index',f(s.light.uvi,2)],
['calibration',`dry ${s.cal.dry_raw} / wet ${s.cal.wet_raw}`]];
document.getElementById('t').innerHTML=r.map(x=>`<tr><td>${x[0]}</td><td>${x[1]}</td></tr>`).join('');}catch(e){}}
async function cal(k){if(!confirm('Capture '+k.toUpperCase()+' now? Takes ~5 s.'))return;
document.getElementById('log').textContent='capturing '+k+'…';
const r=await fetch('/api/cal/'+k,{method:'POST'});document.getElementById('log').textContent=await r.text();tick();}
tick();setInterval(tick,5000);</script></body></html>)HTML";

static void sendState() {
    JsonDocument d;
    d["node"] = cfg.node; d["fw"] = EDEN_FW_VERSION; d["uptime_s"] = millis() / 1000;
    d["ts"] = netNow(); d["mqtt"] = mqtt.connected();
    d["wifi"]["ip"] = WiFi.localIP().toString(); d["wifi"]["rssi"] = WiFi.RSSI();
    d["wifi"]["drops"] = wifiDrops; d["mqtt_drops"] = mqttDrops;
    auto num = [&](JsonObject o, const char* k, float v) { if (isnan(v)) o[k] = nullptr; else o[k] = v; };
    JsonObject m = d["moisture"].to<JsonObject>();
    m["raw"] = rd.moist_raw; num(m, "pct", rd.moist_pct); m["age_s"] = rd.moist_ms ? (millis() - rd.moist_ms) / 1000 : -1;
    JsonObject e = d["env"].to<JsonObject>();
    e["ok"] = rd.bme_ok; num(e, "t", rd.t); num(e, "rh", rd.rh); num(e, "p", rd.p); num(e, "iaq", rd.iaq);
    e["acc"] = rd.iaq_acc; num(e, "siaq", rd.siaq); num(e, "eco2", rd.eco2); num(e, "bvoc", rd.bvoc);
    JsonObject l = d["light"].to<JsonObject>();
    l["ok"] = rd.ltr_ok; num(l, "lux", rd.lux); num(l, "uvi", rd.uvi);
    d["cal"]["dry_raw"] = cfg.moisture.dry_raw; d["cal"]["wet_raw"] = cfg.moisture.wet_raw;
    String out; serializeJson(d, out);
    http.send(200, "application/json", out);
}

static void webBegin() {
    http.on("/", HTTP_GET, [] { http.send_P(200, "text/html", kPage); });
    http.on("/api/state", HTTP_GET, sendState);
    http.on("/api/config", HTTP_GET, [] { http.send(200, "application/json", configRedacted()); });
    http.on("/api/cal/dry", HTTP_POST, [] {
        if (!hooks.calibrate) { http.send(501, "text/plain", "no handler"); return; }
        hooks.calibrate(false);
        http.send(200, "text/plain", "dry_raw = " + String(cfg.moisture.dry_raw) + " saved");
    });
    http.on("/api/cal/wet", HTTP_POST, [] {
        if (!hooks.calibrate) { http.send(501, "text/plain", "no handler"); return; }
        hooks.calibrate(true);
        http.send(200, "text/plain", "wet_raw = " + String(cfg.moisture.wet_raw) + " saved");
    });
    http.on("/api/reboot", HTTP_POST, [] { http.send(200, "text/plain", "rebooting"); delay(200); ESP.restart(); });
    http.onNotFound([] { http.send(404, "text/plain", "not found"); });
    http.begin();
}

// ---------- lifecycle ----------

void netBegin() {
    if (!cfg.loaded || cfg.wifi.ssid.isEmpty()) {
        Serial.println("net: no config / no SSID — running offline (serial only)");
        return;
    }
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg.node.c_str());
    WiFi.setAutoReconnect(false);  // we manage backoff ourselves
    wifiKick();
    otaBegin();
    webBegin();
}

void netLoop() {
    if (!cfg.loaded || cfg.wifi.ssid.isEmpty()) return;

    bool up = netWifiUp();
    if (up && !wifiWasUp) onWifiUp();
    if (!up && wifiWasUp) { wifiDrops++; Serial.println("wifi: lost"); }
    wifiWasUp = up;

    if (!up) {
        if (millis() - wifiLastAttempt > wifiBackoff) {
            wifiBackoff = min<uint32_t>(wifiBackoff * 2, 60000);
            wifiKick();
        }
        return;
    }

    ArduinoOTA.handle();
    if (otaBusy) return;
    http.handleClient();

    bool mup = mqtt.connected();
    if (!mup && mqttWasUp) { mqttDrops++; Serial.printf("mqtt: lost (err %d)\n", mqtt.lastError()); }
    mqttWasUp = mup;
    if (mup) {
        mqtt.loop();
        if (millis() - lastTelemetry > 60000) { publishTelemetry(); lastTelemetry = millis(); }
    } else if (millis() - mqttLastAttempt > 5000) {
        mqttConnect();
    }
}

String netStatusLine() {
    char buf[200];
    snprintf(buf, sizeof buf, "net: wifi %s ip %s rssi %d | mqtt %s %s:%u | time %lu | drops wifi %lu mqtt %lu | fw %s",
             netWifiUp() ? "up" : "down", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
             mqtt.connected() ? "up" : "down", cfg.mqtt.host.c_str(), cfg.mqtt.port,
             static_cast<unsigned long>(netNow()), static_cast<unsigned long>(wifiDrops),
             static_cast<unsigned long>(mqttDrops), EDEN_FW_VERSION);
    return String(buf);
}
