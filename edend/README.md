# edend

The Eden orchestrator daemon. Runs on the Pi as a systemd unit (`pi/edend/edend.service`).

What it does today (0.1.0):

- Subscribes to `eden/+/status`, `eden/+/sensor/+`, `eden/+/telemetry`, `eden/+/ack/+`
  (see `docs/mqtt-contract.md`) and writes every reading to VictoriaMetrics through the
  Prometheus text import endpoint, batched (10 s / 500 lines), with a bounded disk spool
  for VM outages.
- Tracks node presence from the retained status topic (LWT), debounces 2 min, and alerts
  via ntfy when a node drops or returns. Re-writes `eden_node_online` every minute so the
  dashboard stat stays fresh.
- Publishes its own retained `eden/edend/status` with an LWT.
- Alerts are log-only until `/etc/eden/ntfy.cred` (`user:pass`) exists.

Not yet: watering policy / dose scheduling (`plants.yaml` is parsed for names only).

## Metric names

| MQTT | Metric |
|---|---|
| `sensor/<name>` `v` | `eden_<name>{node,unit}` |
| `sensor/<name>` `raw` (when ≠ v) | `eden_<name>_raw{node}` |
| `sensor/<name>` any other numeric key (e.g. BSEC `acc`) | `eden_<name>_<key>{node}` |
| `status` | `eden_node_online{node}` 0/1, `eden_node_info{node,fw,ip}` 1 |
| `telemetry` | `eden_node_rssi_dbm`, `eden_node_uptime_s`, `eden_node_heap_bytes` |
| `ack` done (dose) | `eden_dose_ml{node}` |
| `ack` failed | `eden_cmd_failed{node,reason}` |
| edend heartbeat | `eden_edend_up` |

Sensor timestamps come from the node (`ts`); 0/absent or implausible (>1 h ahead, >7 d
behind: unsynced clock) falls back to receive time.

## Dev

```sh
cd edend
python3 -m pytest            # pure-logic tests (no broker/VM needed)
EDEND_CONFIG=./local.yaml MQTT_EDEND_PASSWORD=... python3 -m edend   # run against the Pi
```

## Deploy

`sudo pi/install.sh` builds `/opt/edend/venv`, installs this package into it, and places
`/etc/eden/edend.yaml` from `pi/edend/edend.yaml.example` if missing. Then
`sudo systemctl enable --now edend` and `journalctl -u edend -f`.
