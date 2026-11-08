# Selector Valve — Firmware ↔ CAD Contract

Andy owns the mechanical design. Firmware owns indexing. For the two to meet, the printed
valve must satisfy these constraints — everything else (port count, geometry, materials)
is free.

## Hard requirements

1. **NEMA 17 drive.** The rotor mounts to (or is geared from) a NEMA 17 shaft.
2. **One home reference per revolution.** Firmware trusts position only relative to a
   detectable home. Recommended: a **3 mm disc magnet embedded in the rotor** + an
   **A3144 hall sensor** mounted on the stator (non-contact, no wear, no lever geometry
   to design around). A microswitch ramp works too — firmware supports either, it just
   needs one GPIO that changes state at exactly one rotor angle.
3. **Equal angular port spacing.** Ports at `360° / N`. Firmware does
   `steps_per_port = steps_per_rev * gear_ratio / N` — N and the ratio are config values,
   so reprinting with more ports is a config edit, not a code change.
4. **Holds position unpowered.** Drivers are EN-disabled whenever idle (no holding
   current, no heat). The mechanism must not drift between moves: detent balls, friction
   O-rings, or a worm/high-ratio gear stage all work. If the design free-wheels, tell me —
   firmware would then re-home before every move (slower, but supported).

## Nice to have

- Home position that is also a **"all ports closed"** position — gives a safe park state.
- Slight lead-in chamfer on port seals so a step or two of index error still seals.

## What firmware does with it

- Boot → state `UNHOMED`; all dose commands rejected (`reason:"unhomed"`).
- `home`: seek hall signal slow, back off, re-approach slower, apply `home_offset` steps.
- `goto port N`: shortest direction, trapezoidal accel, then state `AT_PORT(N)`.
- Interlock: pumps only run in `AT_PORT` matching the dose's `port`; valve only moves
  when pumps are idle.

## When the CAD is ready

Share the design + measured values and we fill in `config.json`:
`ports`, `steps_per_port` (or `gear_ratio`), `home_offset`, and whether home = park.
Then Phase 6 (valve integration) proceeds: 20-cycle homing repeatability test, then
port-N-wets-only-plant-N verification.
