# Architecture

```
                         5th-wheel LAN (10.0.0.0/24)
                                                       ┌────────────────────┐
 ┌──────────────┐   WiFi    ┌──────────────────────────┤  Pi 4B (10.0.0.1)  │
 │ ESP32 "grow1"│──────────▶│ mosquitto :1883          │  also: Serenity    │
 │ moisture x6  │  MQTT     │    │                     │  LTE gateway —     │
 │ pump0 pump1  │◀──────────│    ▼                     │  DO NOT TOUCH      │
 │ valve        │  cmds     │ edend (systemd)          │  iptables/dnsmasq  │
 └──────────────┘           │  ├─ ingest ─▶ VictoriaMetrics :8428 (lo only) │
 ┌──────────────┐           │  ├─ watering policy (plants.yaml)             │
 │ ESP32 "envN" │──────────▶│  ├─ dose tracker (cmd → ack lifecycle)        │
 │ BME280, CO2… │           │  └─ alerts ─▶ ntfy :8090 ──▶ 📱 (Tailscale)   │
 └──────────────┘           │ grafana :3000 ─▶ VM (PromQL)                  │
                            └───────────────────────────────────────────────┘
```

## Decisions

| Decision | Choice | Why |
|---|---|---|
| TSDB | VictoriaMetrics single-node | Single static ARM64 binary, ~50–100 MB RSS; accepts Prometheus text import on `/api/v1/import/prometheus` (and Influx line protocol); PromQL for Grafana; `-retentionPeriod` + `-memory.allowedBytes` caps. InfluxDB 2.x is 3–6× the RAM for nothing we need. |
| Deployment | Bare systemd | Docker rewrites iptables FORWARD chains → real risk to Serenity NAT. Every service is an apt package or static binary; `MemoryMax=` per unit. |
| Firmware | One PlatformIO codebase, runtime JSON config | Node role = data, not code. Sensor-only vs pump/valve node is the same binary with a different `/config.json`. |
| Control | Pi decides, node enforces | Watering policy, schedules, daily caps live in `edend`. Firmware independently enforces per-dose caps + hardware-timer watchdog, so a hung Pi or dropped WiFi can never flood a plant. |
| DB growth | Daily size check → ntfy | Threshold starts at 2 GiB; when it fires, revisit DB location (standing decision). |

## Data flow

1. Node samples moisture (median-of-15, powered probe) → `eden/<node>/sensor/<name>` (QoS 0).
2. `edend` ingests → batches → Prometheus text lines (`eden_<sensor>{node=…}`, see
   `edend/README.md`) → VM `/api/v1/import/prometheus` (flush 10 s / 500 lines;
   bounded disk spool if VM is down).
3. Watering engine evaluates each reading against `plants.yaml` (threshold, min interval,
   quiet hours, daily cap) → publishes `cmd/pump/<idx>` with unique id + `max_ms`.
4. Node sequences valve → dose → acks; `edend` tracks `accepted → started → done|failed`,
   times out at `max_ms + 10 s`, alerts + backs off on failure.
5. Grafana reads VM; alerts ride ntfy over Tailscale to the phone.

## Failure behavior

| Failure | Behavior |
|---|---|
| WiFi drops mid-dose | Dose finishes under local watchdog; new commands refused until reconnect. |
| Node dies | LWT flips retained status → offline alert after 2 min debounce. |
| edend dies | Its own LWT flips; nodes keep sensing (RAM-buffered) but nothing doses — fail-dry, never fail-flood. |
| VM down | edend spools to disk (bounded), alerts once, replays on recovery. |
| Sensor breaks (reads air) | Daily mL cap stops runaway dosing; critical-moisture alert fires instead. |
| Pi reboot | All units `Restart=on-failure` + enabled; nodes reconnect on backoff. |
