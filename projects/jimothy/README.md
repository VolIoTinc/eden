# Jimothy 🥑

Eden's first resident. A *Persea americana* (avocado) grown from seed, ~5 months old
(sprouted ≈ June 2026). Avocados are temperamental: they hate wet feet, cold, dry air,
and salty water. So Jimothy gets a full environment profile, not just a moisture probe.

| | |
|---|---|
| Medium | 1 part perlite : 2 parts potting soil |
| Moisture | Capacitive soil probe → ESP32 ADC1 |
| Environment | BME688: temp, humidity, pressure, IAQ (via Bosch BSEC2) |
| Light | LTR390: ambient lux + UV index |
| Watering | Self-designed, 3D-printed peristaltic pump (Phase 3) |
| Camera | Pi Zero snapshots (Phase 4) |
| Data | See [docs/data-storage.md](docs/data-storage.md). **Open for discussion.** |

## Log

| Date | Event |
|---|---|
| ~2026-06 | Sprouted |
| 2026-11-07 | Moved to soil (1:2 perlite/potting mix). Project started. |
| 2026-11-07 | S2 Mini + BME688 + LTR390 + probe soldered and verified. Probe #1 wet_raw = 1346 (pre-divider). Room 21.5 °C / 46 % RH. |
| 2026-11-07 | Probe #2 installed in soil (no divider yet): dry_raw 4095 (clipped), wet_raw 1478. Probe #1 wet 1346, spare. Both clip dry on the S2 ADC → resistor kit ordered for the divider. |

Expect some transplant sulk (droop, a dropped leaf) for 1–3 weeks after repotting. That's
normal. Don't respond by watering more.

## Phases

- [ ] **1. Moisture probe:** wire, capture dry + wet, re-seat in Jimothy's soil
- [ ] **2. BME688 + LTR390:** wire, confirm PTH + lux/UV, let BSEC2 IAQ calibrate (acc → 3)
- [ ] **3. Watering:** peristaltic pump on the Eden dose contract; thresholds from observed data
- [ ] **4. Camera:** Pi Zero snapshots → storage
- [ ] **5. Data:** decide storage (see discussion doc), then move Jimothy onto the shared
      Eden node firmware + MQTT

---

## Wiring (Phases 1–2)

Board: **Wemos / LOLIN S2 Mini v1.0.0** (ESP32-S2: WiFi only, no Bluetooth, native USB).
The pin map in [`docs/wiring.md`](../../docs/wiring.md) is for a classic ESP32 and does
**not** apply here; redo it for the S2 when the pump arrives (Phase 3).

| Part | Pin | S2 Mini |
|---|---|---|
| Moisture probe VCC | → | **GPIO 5** (switched power: probe only on while sampling) |
| Moisture probe GND | → | GND |
| Moisture probe AOUT | → | **GPIO 3** (ADC1) **via a 2:1 divider** (see below) |
| BME688 VIN / VCC | → | 3V3 |
| BME688 GND | → | GND |
| BME688 SDA | → | **GPIO 33** |
| BME688 SCL | → | **GPIO 35** |
| LTR390 VIN / GND | → | 3V3 / GND |
| LTR390 SDA / SCL | → | **GPIO 33 / 35** (same bus as the BME688) |

**Probe divider (required on the S2).** The S2's ADC tops out around 2.5 V (a classic
ESP32 reads to ~3.1 V), and a dry capacitive probe on 3.3 V outputs more than that, so
without a divider "dry" clips at 4095 and you can't see the soil drying out. Two equal
resistors (10k–100k) halve the signal:

```
probe yellow ──[ R ]──┬── GPIO 3
                    [ R ]
                     GND
```

Put it on the board side so swapped probes all get it. Expect dry ≈ 2400, wet ≈ 700
after the divider. **Every probe needs its own dry/wet numbers** — they vary unit to unit,
so recalibrate on any swap.

S2 notes:
- ADC1 is GPIO 1–10; ADC2 (11–20) dies when WiFi is up, so keep analog on 1–10.
- GPIO 33/35 are the board's labelled SDA/SCL. Both I2C breakouts have on-board pull-ups.
- Avoid GPIO 0 (boot button), 18 (USB D-), 19 (USB D+), and 45/46 (strapping) for anything.
- First flash: if the port isn't found, hold **0**, tap **RST**, release **0** → bootloader.
  After that `pio run -t upload` resets it automatically.

- **Probe power from a GPIO** is fine for one capacitive probe (~5 mA). It stops the probe
  corroding and drifting from being powered 24/7. The MOSFET switch in `docs/wiring.md`
  is for when there are multiple probes.
- **BME688 address:** firmware auto-detects 0x77 (Adafruit, most breakouts) or 0x76
  (Pimoroni, or SDO tied to GND).
- **LTR390** is fixed at 0x53, so there's no address clash. Daisy-chain both on STEMMA QT /
  Qwiic if your breakouts have the connectors.
- Give the BME688 3.3 V only. Most breakouts have a regulator, but 3V3 is always safe.

## Phase 1: moisture calibration

```sh
cd projects/jimothy/firmware
pio run -t upload && pio device monitor
```

The monitor prints `moisture raw=…` every 5 s. Serial keys: `d` dry, `w` wet, `p` print
calibration, `i` I2C scan + sensor status, `r` reboot (re-shows boot messages), `x` wipe
BSEC learned state. `pio device monitor` needs a real terminal; from a script, open the
port with pyserial and send the key bytes. The port name can change (`ttyACM0`/`1`)
after a reboot — find the board by USB VID `303A`. Then:

1. **Dry.** Pull the probe, wipe it clean and fully dry, hold it in open air. Press **`d`**.
2. **Wet.** Stand the probe in a cup of water **up to the printed line**, with the
   electronics dry. Press **`w`**.
3. Press **`p`** to print the calibration. With the divider, expect dry ≈ 2400 and
   wet ≈ 700. A span under ~1000 counts means a bad probe or wiring. The capture warns
   if the 20 readings spread more than 60 counts (the first probe did 40).
4. **Back into Jimothy:**
   - Insert the probe vertically to the line, a few cm from the stem, on the side *away*
     from where the pump outlet will drip.
   - Firm the mix around it. Perlite leaves air gaps, and air gaps read as "dry".
   - Leave it there. Every re-seat changes the readings.

Values persist in NVS across reboots. Write them in the Log table too.

**Reading the numbers:** 100% means "in a cup of water". Soil never gets there. Even
fully watered, this mix will probably sit around 55–80%. The useful numbers are what
Jimothy reads **right after a thorough watering** and **when the top ~2–3 cm feels dry**.
Watch one or two full dry-down cycles before setting a watering threshold in Phase 3.

## Phase 2: BME688 (PTH + IAQ)

The same firmware prints `env T=… RH=… P=… IAQ=… (acc N) …` every 3 s.

- **Config blob is mandatory.** BSEC refuses IAQ subscriptions (status 14) unless a config
  matching the sample rate is loaded first. The firmware loads
  `bme680_iaq_33v_3s_4d` (Bosch uses the BME680 IAQ configs for classic IAQ on the 688;
  the 688-specific configs are for gas-fingerprint mode).
- **IAQ takes time.** IAQ is valid ~5 min after boot, but `acc` (accuracy) climbs 0 → 3
  over **hours to a few days** of uninterrupted running, as BSEC learns the room's
  clean and dirty baselines. Once `acc` = 3 the firmware saves BSEC's learned state to
  NVS every 6 h, so a reboot doesn't restart learning. (`x` wipes it.)
- **Placement:** at canopy height, shaded, out of the water splash zone, and not
  directly above the soil. Fresh-watered soil and potting mix emit VOCs and will spike
  IAQ. Keep it a few cm from the ESP32 too, since the board's heat skews temperature.
- **What IAQ is (and isn't):** IAQ is Bosch's 0–500 human air-quality index derived from
  VOCs. `eCO2` is an *estimate from VOCs*, **not a real CO₂ measurement**. For Jimothy,
  the numbers that matter are **temperature and humidity**. IAQ is still worth logging
  for spotting events like smoke, cooking, or fresh paint, and for correlating with the
  photos later. If actual CO₂ ever matters, add an NDIR sensor (e.g. SCD41).

## Phase 2b: LTR390 (light + UV)

Prints `light lux=… UVI=…` every 10 s. The firmware alternates the sensor between its
visible (ALS) and UV modes and auto-adjusts gain, so it handles everything from a dim
room up to direct sun.

- **Placement:** at canopy height, facing **up** toward the light source, nothing
  shading it. Mount it at a fixed angle and leave it there, because tilt changes the
  readings a lot.
- **UV indoors is ~0.** Window glass blocks most UV-B, and most LED grow lights emit
  none. Expect UVI ≈ 0.0 unless Jimothy goes outside or the light has UV diodes. That's
  still useful: it tells sun-time from lamp-time, and it's an early warning before
  moving him outdoors (young avocado leaves sunburn easily, so harden off gradually).
- **Lux is a human-eye unit**, not plant light. It's fine for "is the light on, how
  bright, how many hours". Later we can turn it into an estimated **DLI** (daily light
  integral) using a conversion factor for the light source. Treat that as a rough
  estimate, since the factor depends on the spectrum.
- If the sensor ends up behind a clear cover, raise `kWindowFactor` in `main.cpp` to
  compensate.

### Avocado comfort band (starting targets, refine with data)

| | Happy | Worry |
|---|---|---|
| Temperature | 18–29 °C (65–85 °F) | < 10 °C (50 °F): growth stalls. < 4 °C: damage |
| Humidity | 50–70 % RH | < 40 %: brown leaf tips, crispy edges |
| Soil | evenly moist, never soggy; top 2–3 cm dries between waterings | constantly wet → root rot (the #1 avocado killer) |
| Light | bright indirect, ~6 h+ of strong light; > 10k lux indoors is great | < 2k lux all day → leggy, stretched growth |
| Water | rain / filtered / low-salt | salty or chlorinated tap water → tip burn; flush the pot occasionally |

That last row matters for the pump: whatever fills the reservoir is all Jimothy drinks.

## Phase 3+ (later)

- **Pump:** fits Eden's dose contract (`docs/mqtt-contract.md`). edend decides, firmware
  enforces per-dose caps and the watchdog. Calibrate steps ↔ mL per `docs/calibration.md`.
  The pump should get a **conservative daily mL cap**, because avocados die from
  over-watering far more often than under-watering.
- **Camera + data:** [docs/data-storage.md](docs/data-storage.md).
