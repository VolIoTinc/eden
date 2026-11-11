"""Turn MQTT messages (docs/mqtt-contract.md) into metric samples.

Metric naming (what Grafana queries):
  eden_<sensor>{node,unit}        sensor value `v`
  eden_<sensor>_raw{node}         sensor `raw`, only when present and != v
  eden_<sensor>_<key>{node}       any other numeric key in the payload (e.g. BSEC `acc`)
  eden_node_online{node}          1/0 from the retained status topic (and LWT)
  eden_node_info{node,fw,ip}      1, from the status topic
  eden_node_rssi_dbm / eden_node_uptime_s / eden_node_heap_bytes{node}   telemetry
  eden_dose_ml{node}              ml_actual from a `done` dose ack
  eden_cmd_failed{node,reason}    1 per failed ack

Pure functions only: no I/O, so the whole mapping is unit-testable.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

_NAME_OK = re.compile(r"[^a-zA-Z0-9_]")
_RESERVED = {"ts", "v", "raw", "unit"}
# Node clocks start at 1970 until SNTP syncs; anything implausible → receive time.
MAX_FUTURE_S = 3600
MAX_PAST_S = 7 * 86400


@dataclass(frozen=True)
class Sample:
    metric: str
    value: float
    ts: float  # unix seconds
    labels: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class Event:
    """Non-metric things the app cares about (presence, acks)."""
    kind: str  # "status" | "ack"
    node: str
    data: dict


@dataclass
class Parsed:
    samples: list[Sample] = field(default_factory=list)
    events: list[Event] = field(default_factory=list)
    note: str | None = None  # why nothing came out, for debug logging


def sanitize(name: str) -> str:
    name = _NAME_OK.sub("_", name)
    if not name or name[0].isdigit():
        name = "_" + name
    return name


def _num(x) -> float | None:
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return None
    f = float(x)
    if f != f or f in (float("inf"), float("-inf")):
        return None
    return f


def pick_ts(payload: dict, now: float) -> float:
    ts = _num(payload.get("ts"))
    if ts is None or ts <= 0:
        return now
    if ts > now + MAX_FUTURE_S or ts < now - MAX_PAST_S:
        return now
    return ts


def parse(topic: str, payload: bytes | str, now: float) -> Parsed:
    out = Parsed()
    parts = topic.split("/")
    if len(parts) < 3 or parts[0] != "eden":
        out.note = "not an eden topic"
        return out
    node, kind = parts[1], parts[2]
    if node in ("edend", "system") or kind == "cmd":
        out.note = "ignored topic"
        return out

    try:
        if isinstance(payload, (bytes, bytearray)):
            payload = payload.decode("utf-8")
        data = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        out.note = f"bad json: {e}"
        return out
    if not isinstance(data, dict):
        out.note = "payload not an object"
        return out

    nl = {"node": node}
    ts = pick_ts(data, now)

    if kind == "sensor" and len(parts) == 4:
        name = sanitize(parts[3])
        v = _num(data.get("v"))
        if v is None:
            out.note = "sensor without numeric v"
            return out
        labels = dict(nl)
        unit = data.get("unit")
        if isinstance(unit, str) and unit:
            labels["unit"] = unit
        out.samples.append(Sample(f"eden_{name}", v, ts, labels))
        raw = _num(data.get("raw"))
        if raw is not None and raw != v:
            out.samples.append(Sample(f"eden_{name}_raw", raw, ts, dict(nl)))
        for k, val in data.items():
            if k in _RESERVED:
                continue
            n = _num(val)
            if n is not None:
                out.samples.append(Sample(f"eden_{name}_{sanitize(k)}", n, ts, dict(nl)))
        return out

    if kind == "status" and len(parts) == 3:
        online = bool(data.get("online"))
        out.samples.append(Sample("eden_node_online", 1.0 if online else 0.0, now, dict(nl)))
        if online:
            info = dict(nl)
            for k in ("fw", "ip"):
                if isinstance(data.get(k), str):
                    info[k] = data[k]
            out.samples.append(Sample("eden_node_info", 1.0, now, info))
        out.events.append(Event("status", node, {"online": online, **data}))
        return out

    if kind == "telemetry" and len(parts) == 3:
        for key, metric in (("rssi", "eden_node_rssi_dbm"), ("uptime_s", "eden_node_uptime_s"),
                            ("heap", "eden_node_heap_bytes")):
            n = _num(data.get(key))
            if n is not None:
                out.samples.append(Sample(metric, n, ts, dict(nl)))
        if not out.samples:
            out.note = "telemetry without known fields"
        return out

    if kind == "ack" and len(parts) == 4:
        state = data.get("state")
        cmd_id = parts[3]
        if state == "done":
            ml = _num(data.get("ml_actual"))
            if ml is not None and cmd_id.startswith("d-"):
                out.samples.append(Sample("eden_dose_ml", ml, now, dict(nl)))
        elif state == "failed":
            labels = dict(nl)
            labels["reason"] = str(data.get("reason", "unknown"))
            out.samples.append(Sample("eden_cmd_failed", 1.0, now, labels))
        out.events.append(Event("ack", node, {"id": cmd_id, **data}))
        return out

    out.note = "unknown topic shape"
    return out
