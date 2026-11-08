# Eden

Automated grow system: ESP32 nodes (moisture sensing + peristaltic dosing) talking MQTT
over WiFi to a Raspberry Pi 4B that runs the broker, time-series DB (VictoriaMetrics),
Grafana, self-hosted ntfy, and `edend` — the Python orchestrator that decides when to water.

The Pi is shared with the Serenity cellular-gateway role, so everything here runs as lean
systemd units. **No Docker** — Docker rewrites iptables FORWARD chains and would break the
camper's NAT routing.

## Layout

| Path | What |
|---|---|
| `docs/` | Contracts and guides. `mqtt-contract.md` is the source of truth. |
| `firmware/` | One PlatformIO project for every node role; behavior set by `/config.json` on LittleFS. |
| `edend/` | Python asyncio orchestrator: ingest → VictoriaMetrics, watering policy, ntfy alerts. |
| `pi/` | Idempotent provisioning: `install.sh` + per-service configs and systemd units. |
| `projects/` | Per-plant projects. [`jimothy/`](projects/jimothy/) — avocado, first resident. |
| `tools/` | `mqtt-spy.sh` (watch the bus), `calibrate-pump.py` (steps ↔ mL). |

## Quick start

1. **Pi**: `sudo pi/install.sh` (safe to re-run), then follow its output to set MQTT/ntfy passwords.
2. **Node**: copy `firmware/data/config.example.json` → `firmware/data/config.json`, edit,
   then `pio run -t upload && pio run -t uploadfs` from `firmware/`.
3. **Plants**: copy `edend/edend/plants.yaml.example` → `/etc/eden/plants.yaml`, edit,
   `sudo systemctl restart edend`.
4. Watch it: `tools/mqtt-spy.sh`, Grafana on `:3000`, ntfy topic `eden_alerts` on your phone.

## Build & test (dev box)

```sh
cd firmware && pio run           # compile ESP32 firmware
cd firmware && pio test -e native  # host-side unit tests (dosing math, state machines)
cd edend && python3 -m pytest    # policy engine tests
```
