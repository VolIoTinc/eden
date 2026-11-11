// Jimothy node firmware: capacitive moisture probe + BME688 (BSEC2 IAQ) + LTR390 (lux, UVI).
// WiFi + MQTT (docs/mqtt-contract.md) + ArduinoOTA + status page at http://<node>.local.
// Config: /config.json on LittleFS (see data/config.example.json). Without it the node runs
// offline and still prints to serial.
//
// Serial keys (115200 baud, USB CDC):
//   d  capture DRY reading   (probe clean, in open air)
//   w  capture WET reading   (probe in a cup of water up to its line — not the electronics)
//   p  print calibration      n  network status      c  print config (secrets masked)
//   i  I2C scan + sensor status                      r  reboot
//   x  wipe saved BSEC state (forces IAQ to re-learn from scratch)

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_LTR390.h>
#include <bsec2.h>

#include <algorithm>

#include "config.h"
#include "eden_logic.h"
#include "net.h"

// ---------- Pins: Wemos/LOLIN S2 Mini (ESP32-S2) ----------
// ADC1 on the S2 is GPIO 1-10 (ADC2 is unusable once WiFi is up, same as classic ESP32).
constexpr int kProbeAdcPin = 3;     // ADC1_CH2
constexpr int kProbePowerPin = 5;   // probe VCC straight from GPIO (~5 mA draw is fine)
constexpr int kSdaPin = SDA;        // 33 on the S2 Mini
constexpr int kSclPin = SCL;        // 35 on the S2 Mini

constexpr int kSamples = 15;               // median-of-15, same as Eden nodes
constexpr int kCalibrationReads = 20;      // readings averaged per dry/wet capture
constexpr uint32_t kStateSaveMs = 6UL * 3600 * 1000;  // save BSEC state every 6 h

Preferences prefs;                 // BSEC state only (calibration lives in config.json now)
Bsec2 env;
bool bsecOk = false;
uint32_t lastMoisture = 0, lastStateSave = 0, lastEnvPublish = 0, lastLightPublish = 0;

// ---------- Moisture ----------

int readProbeMedian() {
    digitalWrite(kProbePowerPin, HIGH);
    delay(100);  // let the probe's oscillator settle
    int s[kSamples];
    for (int i = 0; i < kSamples; i++) {
        s[i] = analogRead(kProbeAdcPin);
        delay(5);
    }
    digitalWrite(kProbePowerPin, LOW);
    std::sort(s, s + kSamples);
    return s[kSamples / 2];
}

bool calibrated() { return cfg.moisture.dry_raw > 0 && cfg.moisture.wet_raw > 0; }

void capture(bool wet) {
    const char* label = wet ? "wet" : "dry";
    Serial.printf("\n== capturing %s: hold still, %d readings...\n", label, kCalibrationReads);
    long sum = 0;
    int lo = 4095, hi = 0;
    for (int i = 0; i < kCalibrationReads; i++) {
        int r = readProbeMedian();
        sum += r;
        lo = std::min(lo, r);
        hi = std::max(hi, r);
        Serial.printf("  %2d: %d\n", i + 1, r);
    }
    int v = static_cast<int>(sum / kCalibrationReads);
    (wet ? cfg.moisture.wet_raw : cfg.moisture.dry_raw) = v;
    Serial.printf("== %s_raw = %d  (min %d, max %d, spread %d)\n", label, v, lo, hi, hi - lo);
    if (hi - lo > 60) Serial.println("   !! spread > 60 counts — check wiring / hold the probe steadier");
    if (cfg.loaded) Serial.println(configSave() ? "== saved to /config.json" : "!! config save FAILED");
}

void printCalibration() {
    Serial.printf("\n== calibration: dry_raw=%d wet_raw=%d\n", cfg.moisture.dry_raw, cfg.moisture.wet_raw);
    if (!calibrated()) { Serial.println("   capture both 'd' and 'w' first"); return; }
    int span = cfg.moisture.dry_raw - cfg.moisture.wet_raw;
    if (span < 300) Serial.printf("   !! span only %d counts (expect >1000) — bad probe or wiring\n", span);
}

void sampleMoisture() {
    int raw = readProbeMedian();
    rd.moist_raw = raw;
    rd.moist_ms = millis();
    if (calibrated()) {
        rd.moist_pct = eden::moisturePct(raw, cfg.moisture.dry_raw, cfg.moisture.wet_raw);
        Serial.printf("moisture raw=%d  %.1f%%\n", raw, rd.moist_pct);
        netPublishSensor("moisture", rd.moist_pct, raw, "%");
    } else {
        rd.moist_pct = NAN;
        Serial.printf("moisture raw=%d  (uncalibrated)\n", raw);
        netPublishSensor("moisture_raw", raw, raw, "raw");
    }
}

// ---------- BME688 / BSEC2 ----------

// BSEC needs a config blob matching the sample rate before it will accept IAQ subscriptions
// (without one it fails with status 14, BSEC_W_SU_SAMPLERATEMISMATCH). Classic IAQ on a
// BME688 uses the BME680 IAQ configs: 3.3 V supply, 3 s (LP) rate, 4-day calibration history.
const uint8_t kBsecConfig[] = {
#include "config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt"
};

void saveBsecState() {
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    if (env.getState(state)) {
        prefs.putBytes("bsec", state, BSEC_MAX_STATE_BLOB_SIZE);
        Serial.println("== BSEC state saved to NVS");
    }
}

void onBsec(const bme68xData data, const bsecOutputs outputs, Bsec2 bsec) {
    for (uint8_t i = 0; i < outputs.nOutputs; i++) {
        const bsecData& o = outputs.output[i];
        switch (o.sensor_id) {
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: rd.t = o.signal; break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY: rd.rh = o.signal; break;
            case BSEC_OUTPUT_RAW_PRESSURE: rd.p = o.signal; break;  // hPa
            case BSEC_OUTPUT_IAQ: rd.iaq = o.signal; rd.iaq_acc = o.accuracy; break;
            case BSEC_OUTPUT_STATIC_IAQ: rd.siaq = o.signal; break;
            case BSEC_OUTPUT_CO2_EQUIVALENT: rd.eco2 = o.signal; break;
            case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT: rd.bvoc = o.signal; break;
        }
    }
    rd.env_ms = millis();

    // BSEC fires every 3 s; publish/print at the configured cadence (first one immediately).
    if (lastEnvPublish == 0 || millis() - lastEnvPublish >= cfg.env_interval_s * 1000UL) {
        lastEnvPublish = millis();
        // accuracy: 0 = stabilizing, 1 = low, 2 = medium, 3 = calibrated
        Serial.printf("env      T=%.2fC  RH=%.1f%%  P=%.1fhPa  IAQ=%.0f (acc %d)  sIAQ=%.0f  eCO2=%.0fppm  bVOC=%.2fppm\n",
                      rd.t, rd.rh, rd.p, rd.iaq, rd.iaq_acc, rd.siaq, rd.eco2, rd.bvoc);
        char acc[16];
        snprintf(acc, sizeof acc, "\"acc\":%d", rd.iaq_acc);
        netPublishSensor("temp_c", rd.t, -1, "C");
        netPublishSensor("rh_pct", rd.rh, -1, "%");
        netPublishSensor("pressure_hpa", rd.p, -1, "hPa");
        netPublishSensor("iaq", rd.iaq, -1, "iaq", acc);
        netPublishSensor("siaq", rd.siaq, -1, "iaq", acc);
        netPublishSensor("eco2_ppm", rd.eco2, -1, "ppm", acc);
        netPublishSensor("bvoc_ppm", rd.bvoc, -1, "ppm", acc);
    }

    if (rd.iaq_acc == 3 && millis() - lastStateSave > kStateSaveMs) {
        lastStateSave = millis();
        saveBsecState();
    }
}

bool i2cPresent(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool startBsec() {
    uint8_t addr = i2cPresent(BME68X_I2C_ADDR_HIGH)  ? BME68X_I2C_ADDR_HIGH   // 0x77
                   : i2cPresent(BME68X_I2C_ADDR_LOW) ? BME68X_I2C_ADDR_LOW    // 0x76
                                                     : 0;
    if (!addr) {
        Serial.printf("!! no BME688 at 0x76/0x77 — check SDA=%d SCL=%d, 3.3V, GND\n", kSdaPin, kSclPin);
        return false;
    }
    Serial.printf("== BME688 found at 0x%02X\n", addr);
    if (!env.begin(addr, Wire)) {
        Serial.printf("!! BSEC begin failed: bsec=%d sensor=%d\n", env.status, env.sensor.status);
        return false;
    }
    if (!env.setConfig(kBsecConfig)) {
        Serial.printf("!! BSEC setConfig failed: %d\n", env.status);
        return false;
    }

    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    if (prefs.getBytesLength("bsec") == BSEC_MAX_STATE_BLOB_SIZE) {
        prefs.getBytes("bsec", state, BSEC_MAX_STATE_BLOB_SIZE);
        Serial.println(env.setState(state) ? "== restored BSEC state (IAQ calibration kept)"
                                           : "!! saved BSEC state rejected, starting fresh");
    }

    env.setTemperatureOffset(TEMP_OFFSET_LP);  // compensate the sensor's own heater
    bsecSensor outputs[] = {
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    };
    if (!env.updateSubscription(outputs, ARRAY_LEN(outputs), BSEC_SAMPLE_RATE_LP)) {
        Serial.printf("!! BSEC subscription failed: %d\n", env.status);
        return false;
    }
    env.attachCallback(onBsec);
    Serial.printf("== BSEC %d.%d.%d.%d running (LP, every 3 s). IAQ needs ~5 min to start, hours-days to reach acc 3.\n",
                  env.version.major, env.version.minor, env.version.major_bugfix, env.version.minor_bugfix);
    return true;
}

// ---------- LTR390 (ambient light + UV) ----------
//
// The LTR390 measures ALS (visible) or UVS (UV), one at a time, so we alternate modes.
// Non-blocking state machine: BSEC needs run() called on schedule, so no busy-waits here.
//   ALS: 18-bit / 100 ms, auto-gain (sunlight saturates high gain, dim rooms need it).
//   UVS: 20-bit / 400 ms at gain 18 — the datasheet's UVI sensitivity reference point.

Adafruit_LTR390 ltr;

constexpr uint32_t kLightCycleMs = 10000;   // measurement cadence; publishing is cfg.light_interval_s
constexpr float kWindowFactor = 1.0f;       // >1 if the sensor sits behind a cover/window
constexpr float kUvSensitivity = 2300.0f;   // counts per UVI @ gain 18, 20-bit
const ltr390_gain_t kGains[] = {LTR390_GAIN_1, LTR390_GAIN_3, LTR390_GAIN_6, LTR390_GAIN_9, LTR390_GAIN_18};
const float kGainMult[] = {1, 3, 6, 9, 18};
constexpr uint32_t kAls18BitMax = (1UL << 18) - 1;

enum class LtrPhase { Idle, WaitAls, WaitUvs };
LtrPhase ltrPhase = LtrPhase::Idle;
int alsGainIdx = 2;  // start at 6x
uint32_t lastLight = 0, phaseStart = 0;

void ltrStartAls() {
    ltr.setMode(LTR390_MODE_ALS);
    ltr.setGain(kGains[alsGainIdx]);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
    ltr.readALS();  // clear any stale sample so newDataAvailable() means a fresh one
    ltrPhase = LtrPhase::WaitAls;
    phaseStart = millis();
}

void ltrStartUvs() {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_18);
    ltr.setResolution(LTR390_RESOLUTION_20BIT);
    ltr.readUVS();
    ltrPhase = LtrPhase::WaitUvs;
    phaseStart = millis();
}

void ltrTick() {
    if (!rd.ltr_ok) return;
    switch (ltrPhase) {
        case LtrPhase::Idle:
            if (millis() - lastLight >= kLightCycleMs) {
                lastLight = millis();
                ltrStartAls();
            }
            break;

        case LtrPhase::WaitAls: {
            if (millis() - phaseStart < 250 || !ltr.newDataAvailable()) {
                if (millis() - phaseStart > 2000) ltrPhase = LtrPhase::Idle;  // give up this round
                break;
            }
            uint32_t als = ltr.readALS();
            // Auto-gain: re-measure this round if saturated or starved.
            if (als > kAls18BitMax * 9 / 10 && alsGainIdx > 0) { alsGainIdx--; ltrStartAls(); break; }
            if (als < 200 && alsGainIdx < 4) { alsGainIdx++; ltrStartAls(); break; }
            // Datasheet: lux = 0.6 * ALS / (gain * int), int = integration time / 100 ms (1 here).
            rd.lux = 0.6f * als / kGainMult[alsGainIdx] * kWindowFactor;
            ltrStartUvs();
            break;
        }

        case LtrPhase::WaitUvs: {
            if (millis() - phaseStart < 500 || !ltr.newDataAvailable()) {
                if (millis() - phaseStart > 3000) ltrPhase = LtrPhase::Idle;
                break;
            }
            uint32_t uvs = ltr.readUVS();
            rd.uvi = uvs / kUvSensitivity * kWindowFactor;
            rd.light_ms = millis();
            ltrPhase = LtrPhase::Idle;
            if (lastLightPublish == 0 || millis() - lastLightPublish >= cfg.light_interval_s * 1000UL) {
                lastLightPublish = millis();
                Serial.printf("light    lux=%.1f (gain %gx)  UVI=%.2f (uvs %lu)\n", rd.lux,
                              kGainMult[alsGainIdx], rd.uvi, static_cast<unsigned long>(uvs));
                netPublishSensor("lux", rd.lux, -1, "lux");
                netPublishSensor("uvi", rd.uvi, static_cast<int>(uvs), "uvi");
            }
            break;
        }
    }
}

bool startLtr() {
    if (!ltr.begin(&Wire)) {
        Serial.printf("!! no LTR390 at 0x53 — check SDA=%d SCL=%d, 3.3V, GND\n", kSdaPin, kSclPin);
        return false;
    }
    ltr.enable(true);
    Serial.println("== LTR390 found at 0x53 (lux + UV every 10 s)");
    return true;
}

// ---------- Diagnostics ----------

void i2cScan() {
    Serial.print("== I2C scan:");
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
    }
    Serial.printf("%s\n   expect BME688 at 0x76/0x77, LTR390 at 0x53\n", n ? "" : " (nothing)");
    Serial.printf("   BME688/BSEC: %s (bsec status %d, sensor status %d)\n",
                  bsecOk ? "running" : "NOT running", env.status, env.sensor.status);
    Serial.printf("   LTR390: %s\n", rd.ltr_ok ? "running" : "NOT running");
    Serial.printf("   probe: ADC GPIO %d, power GPIO %d\n", kProbeAdcPin, kProbePowerPin);
}

// One-time migration: the bring-up firmware kept dry/wet in NVS; config.json is the home now.
void migrateCalibrationFromNvs() {
    if (!cfg.loaded || calibrated()) return;
    int d = prefs.getInt("dry", 0), w = prefs.getInt("wet", 0);
    if (d > 0 && w > 0) {
        cfg.moisture.dry_raw = d;
        cfg.moisture.wet_raw = w;
        Serial.printf("== migrated calibration from NVS (dry %d / wet %d) → config.json\n", d, w);
        configSave();
    }
}

// ---------- Main ----------

void setup() {
    Serial.begin(115200);
    // Native USB CDC: wait (bounded) for the host to open the port so boot output isn't lost.
    for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);
    delay(200);
    Serial.printf("\n\n== Jimothy node fw %s ==\n", EDEN_FW_VERSION);

    pinMode(kProbePowerPin, OUTPUT);
    digitalWrite(kProbePowerPin, LOW);
    analogReadResolution(12);
    analogSetPinAttenuation(kProbeAdcPin, ADC_11db);

    prefs.begin("jimothy", false);
    configLoad();
    migrateCalibrationFromNvs();
    printCalibration();

    Wire.begin(kSdaPin, kSclPin);
    bsecOk = rd.bme_ok = startBsec();
    rd.ltr_ok = startLtr();

    hooks.calibrate = capture;
    hooks.applyConfig = [] { Serial.println("== config reloaded"); printCalibration(); };
    netBegin();

    Serial.println("keys: d=dry w=wet p=cal n=net c=config i=i2c x=wipe-bsec r=reboot\n");
}

void loop() {
    netLoop();
    if (netOtaBusy()) return;  // flash writes in progress — keep I2C/ADC quiet

    if (bsecOk && !env.run() && env.status < BSEC_OK)
        Serial.printf("!! BSEC run error: bsec=%d sensor=%d\n", env.status, env.sensor.status);
    ltrTick();

    if (Serial.available()) {
        switch (Serial.read()) {
            case 'd': capture(false); break;
            case 'w': capture(true); break;
            case 'p': printCalibration(); break;
            case 'n': Serial.println(netStatusLine()); break;
            case 'c': Serial.println(configRedacted()); break;
            case 'x': prefs.remove("bsec"); Serial.println("== BSEC state wiped; reboot to apply"); break;
            case 'i': i2cScan(); break;
            case 'r': Serial.println("== rebooting"); Serial.flush(); delay(100); ESP.restart(); break;
        }
    }

    if (lastMoisture == 0 || millis() - lastMoisture >= cfg.moisture.interval_s * 1000UL) {
        lastMoisture = millis();
        sampleMoisture();
    }
}
