# Wiring Guide

## Moisture sensors — ADC1 pins only

WiFi disables ADC2, so the only usable analog pins on a classic ESP32 dev board are:

**GPIO 32, 33, 34, 35, 36 (VP), 39 (VN)** — six channels. 34/35/36/39 are input-only,
which is fine for sensors.

For more than 6 probes on one node, add a **CD74HC4067** 16-channel analog mux:
`SIG → GPIO32`, `S0–S3 → any 4 output GPIOs` (e.g. 18/19/21/22), `EN → GND`.
Firmware supports this via the `mux` section in `config.json`.

**Power the probes through a switch, not directly.** Capacitive probes corrode and drift
when powered 24/7. Use a P-channel MOSFET high-side switch (gate ← GPIO through 1 k,
100 k pullup to 3.3 V): firmware powers the rail, waits 100 ms to settle, samples
(median of 15), powers off. Config key: `power_pin`.

Calibration per probe: record `raw` in dry air (`dry_raw`) and in a glass of water
(`wet_raw`); put both in `config.json`. See `calibration.md`.

## Stepper drivers (A4988 / DRV8825 / TMC2208 — all step/dir)

Suggested pin map (all output-capable, none are strapping pins):

| Function | STEP | DIR | EN |
|---|---|---|---|
| Pump 0 | 25 | 26 | 27 |
| Pump 1 | 4 | 16 | 17 |
| Valve | 14 | 12* | 13 |

\* GPIO 12 is a strapping pin (flash voltage). DIR level at boot is harmless *because EN
is held disabled*, but if your board boots oddly with something wired to 12, swap DIR to 23.

- **EN pull-ups**: add a 10 k pull-up from each EN to 3.3 V (EN is active-low on these
  drivers) so drivers stay **disabled** while the ESP32 resets/flashes. Firmware also
  drives all EN inactive first thing in `setup()`, but the resistor covers the reset gap.
- Home sensor (valve): hall **A3144** (open collector) → GPIO 34 with 10 k pull-up to
  3.3 V; magnet in the valve rotor. A microswitch to GND works on the same pin.
- Microstepping jumpers (MS1–MS3): whatever you set, record it as `microsteps` in config —
  it feeds the steps-per-mL / steps-per-port math.
- TMC2208: use in legacy step/dir mode; leave UART pads unwired for now.

## Power

```
12 V supply ──┬── driver VMOT (100 µF electrolytic across VMOT/GND at EACH driver, close)
              └── 5 V buck ── ESP32 5V/VIN pin
All grounds common (12 V, buck, ESP32, drivers, sensors).
Driver logic VDD ← ESP32 3.3 V.
```

- Set each driver's current limit (Vref) for what the pump actually needs, not the motor's
  rated max — cooler, quieter, and peristaltic pumps need little torque.
- Never hot-plug a motor with VMOT powered (kills drivers).
- Keep moisture-probe cable runs away from stepper wiring; the ADC will happily pick up
  motor noise.
