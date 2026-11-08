# Calibration

## Moisture probes (per probe — they vary)

1. Probe clean and dry, in air: note `raw` from `tools/mqtt-spy.sh` → that's `dry_raw`.
2. Probe in a glass of water up to its line (not past the electronics): note `raw` → `wet_raw`.
3. Put both in the node's `config.json` under that sensor; re-upload FS or send
   `cmd/system {"action":"reload"}`.

Sanity: dry ≈ 2600–3000 raw, wet ≈ 900–1300 raw on 3.3 V probes at 11 dB attenuation.
If dry/wet are within ~300 counts of each other, the probe or wiring is bad.

Recalibrate when: probe replaced, cable length changed, or readings drift vs. finger test.

## Pump steps ↔ mL

Peristaltic output is linear in steps, so two-point isn't needed — one good slope is.

1. Prime the line fully (water at the outlet, no air).
2. Run `tools/calibrate-pump.py --node grow1 --pump 0` — it commands a known step count,
   you weigh the output (1 g = 1 mL), repeat 3×.
3. It prints `steps_per_ml`; put it in `config.json`.

Acceptance: three 50 mL doses each within ±5% by weight. Recalibrate when tubing is
replaced or after ~6 months (tubing wear changes displacement).

## Valve indexing (Phase 6, when the valve exists)

1. `steps_per_port` = steps/rev × microsteps × gear ratio ÷ ports.
2. Home, `goto` each port, check alignment; trim `home_offset` until port 0 seals true.
3. Repeatability: 20 home cycles, mark the rotor — no visible drift allowed.
