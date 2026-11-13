# Eden Plumbed — mains-water model (design notes)

Two Eden hardware models share the node firmware, the selector valve and edend:

| | Eden Reservoir | Eden Plumbed |
|---|---|---|
| Source | jug + peristaltic pump | household cold line (40–80 psi) |
| Dose metering | pump steps ↔ mL calibration | flow-meter pulses |
| Worst case | ~2 L on the floor | runs until someone comes home |
| For | apartments, renters, first build | permanent installs, many plants |

The plumbed model is a safety design with some irrigation attached. Nothing below is optional.

## Water path

1. 1/4" push-fit tee off a cold line (fridge / RO-filter fitting).
2. **Backflow preventer.** Required by most US plumbing codes for any irrigation connection to
   potable water. Hose-bib vacuum breaker at minimum; dual-check for a product.
3. Inline filter + pressure regulator to ~15–25 psi (household pressure destroys drip emitters).
4. **Master valve:** normally-closed 12 V solenoid. Power loss = closed (fail-dry).
5. **Flow meter** rated down to ≤0.3 L/min. The common YF-S201 starts at 1 L/min and cannot see a
   200 mL dose; use a small-bore meter (espresso-machine type, a few hundred pulses per 100 mL).
6. **Selector valve** — the rotary multi-port design in `valve-interface.md`.
7. Drip emitter per plant with a **check valve per port**, so the line after the valve does not
   drain into the last plant watered. Priming volume is per port (different tubing lengths).

## Safety layers (each independent of the others)

- NC master valve + **hardware watchdog** that cuts valve power after N seconds regardless of
  firmware state (555 / dedicated timer IC, not the MCU).
- Flow meter as **leak detector**: any flow with no active dose → close master, alert.
- **Leak puck** under the manifold wired to the same cutoff.
- Master closed whenever the node has no MQTT session; edend never dispatches to an offline
  node (already in the contract).
- Daily volume cap in firmware *and* edend.
- Paranoid tier: two solenoids in series.

## Contract fit

No new topics. The master solenoid is `pump/0`; a dose closes on flow pulses instead of motor
time, bounded by `max_ms`; `ml_actual` comes from the meter. Selector uses `cmd/valve` as today.
Node role is config, so one firmware image covers both models.

## Build order

Reservoir model first: every safety piece above except the backflow preventer applies to it
too, and it is where the selector valve gets debugged with a 2 L worst case instead of a flooded
room.
