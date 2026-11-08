# MQTT Contract

Source of truth for every topic and payload in Eden. Firmware and `edend` both implement
this file; change it here first.

Broker: Mosquitto on the Pi (`10.0.0.1:1883`), listeners on LAN + tailscale0 only.
Auth: `eden-node` (ESP32s) and `edend` users, ACL-scoped to `eden/#`.

`<node>` is the node id from its `config.json` (e.g. `grow1`). All payloads are JSON.
Timestamps `ts` are unix seconds (node clock via SNTP; `edend` falls back to receive time
when `ts` is 0 or absent — nodes may publish before first NTP sync).

## Topics

| Topic | Dir | QoS | Retain | Purpose |
|---|---|---|---|---|
| `eden/<node>/status` | node → | 1 | yes | Online/offline. LWT payload: `{"online":false}`. On connect: `{"online":true,"fw":"0.1.0","ip":"10.0.0.23"}` |
| `eden/<node>/sensor/<name>` | node → | 0 | **no** | One reading: `{"ts":1723100000,"v":41.2,"raw":2113,"unit":"%"}` |
| `eden/<node>/telemetry` | node → | 0 | no | Heartbeat every 60 s: `{"ts":…,"rssi":-61,"uptime_s":3600,"heap":123456,"fw":"0.1.0"}` |
| `eden/<node>/cmd/pump/<idx>` | → node | 1 | no | Dose command (below) |
| `eden/<node>/cmd/valve` | → node | 1 | no | `{"id":"…","action":"home"}` or `{"id":"…","action":"goto","port":3}` |
| `eden/<node>/cmd/system` | → node | 1 | no | `{"id":"…","action":"reboot"\|"reload"\|"announce"}` |
| `eden/<node>/ack/<cmd_id>` | node → | 1 | no | Command lifecycle (below) |
| `eden/edend/status` | edend → | 1 | yes | edend's own LWT, same shape as node status |
| `eden/system/<event>` | edend → | 1 | no | Pi-side events (db-size warning publishes here too, via ntfy not MQTT — see pi/db-size-check) |

Sensor readings are **never retained**: a stale retained reading is worse than none.

## Commands

Every command carries a unique `id` (edend format: `d-YYYYMMDD-NNNN` for doses,
`v-…` valve, `s-…` system). Nodes **dedupe by last-seen id per topic** so QoS 1
redelivery can't double-dose.

### Pump dose — `eden/<node>/cmd/pump/<idx>`

```json
{"id": "d-20260810-0031", "ml": 50, "max_ms": 45000, "port": 3}
```

- `ml` — requested volume. Firmware clamps against its own `max_ml_per_dose`; a command
  over the cap is **rejected**, not clamped silently.
- `max_ms` — hard time bound. Firmware watchdog uses `min(computed_ms * 1.5, max_ms)`.
- `port` — required when the node has a valve; the dose is rejected unless the valve is
  homed and at that port. Omit on valveless nodes.

### Acks — `eden/<node>/ack/<cmd_id>`

State sequence: `accepted` → `started` → `done` | `failed`.

```json
{"id":"d-20260810-0031","state":"done","ml_actual":50.1,"ms":31400}
{"id":"d-20260810-0031","state":"failed","reason":"unhomed"}
```

Failure reasons: `unhomed`, `wrong_port`, `busy`, `over_cap`, `watchdog`, `bad_json`,
`unknown_action`.

## Conventions

- **QoS**: sensors/telemetry 0 (loss-tolerant, high volume); commands, acks, status 1.
  Nothing uses QoS 2.
- **Offline detection**: `edend` treats a node offline when the retained `status` flips
  `online:false` (LWT) and debounces 2 min before alerting.
- **Boot behavior**: node publishes retained online status, then (if it has a valve) waits —
  `edend` sends `cmd/valve {"action":"home"}` on seeing the node come online. Doses are
  rejected `unhomed` until homing completes.
