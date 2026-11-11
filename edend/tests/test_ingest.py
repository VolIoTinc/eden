import json

from edend.ingest import parse, pick_ts, sanitize

NOW = 1_794_400_000.0


def by_metric(samples):
    return {s.metric: s for s in samples}


def test_sensor_value_raw_and_extra_keys():
    p = parse("eden/jimothy/sensor/iaq", json.dumps({"ts": NOW - 5, "v": 52.3, "raw": 52.3, "unit": "", "acc": 1}), NOW)
    m = by_metric(p.samples)
    assert set(m) == {"eden_iaq", "eden_iaq_acc"}  # raw == v is dropped, unit "" dropped
    assert m["eden_iaq"].value == 52.3 and m["eden_iaq"].ts == NOW - 5
    assert m["eden_iaq"].labels == {"node": "jimothy"}
    assert m["eden_iaq_acc"].value == 1.0
    assert p.events == []


def test_sensor_with_distinct_raw_and_unit():
    p = parse("eden/jimothy/sensor/moisture", b'{"ts":1794382686,"v":21.36,"raw":3536,"unit":"%"}', NOW)
    m = by_metric(p.samples)
    assert m["eden_moisture"].labels == {"node": "jimothy", "unit": "%"}
    assert m["eden_moisture_raw"].value == 3536


def test_sensor_ts_zero_uses_receive_time():
    p = parse("eden/n1/sensor/lux", b'{"ts":0,"v":40.7,"unit":"lux"}', NOW)
    assert p.samples[0].ts == NOW


def test_pick_ts_rejects_implausible_clocks():
    assert pick_ts({"ts": 12345}, NOW) == NOW            # 1970 clock before SNTP
    assert pick_ts({"ts": NOW + 7200}, NOW) == NOW       # future
    assert pick_ts({"ts": NOW - 60}, NOW) == NOW - 60
    assert pick_ts({}, NOW) == NOW
    assert pick_ts({"ts": "nope"}, NOW) == NOW


def test_sensor_name_sanitized():
    p = parse("eden/n1/sensor/co2-ppm", b'{"v":400}', NOW)
    assert p.samples[0].metric == "eden_co2_ppm"
    assert sanitize("9lives") == "_9lives"


def test_sensor_without_numeric_v_is_skipped():
    p = parse("eden/n1/sensor/x", b'{"v":"NaN"}', NOW)
    assert p.samples == [] and "numeric" in p.note
    p = parse("eden/n1/sensor/x", b'{"v":true}', NOW)
    assert p.samples == []


def test_status_online_and_offline():
    p = parse("eden/jimothy/status", b'{"online":true,"fw":"0.2.1","ip":"192.168.1.21"}', NOW)
    m = by_metric(p.samples)
    assert m["eden_node_online"].value == 1.0 and m["eden_node_online"].ts == NOW
    assert m["eden_node_info"].labels == {"node": "jimothy", "fw": "0.2.1", "ip": "192.168.1.21"}
    assert p.events[0].kind == "status" and p.events[0].data["online"] is True

    p = parse("eden/jimothy/status", b'{"online":false}', NOW)
    assert by_metric(p.samples)["eden_node_online"].value == 0.0
    assert "eden_node_info" not in by_metric(p.samples)
    assert p.events[0].data["online"] is False


def test_telemetry():
    p = parse("eden/jimothy/telemetry", b'{"ts":1794382687,"rssi":-24,"uptime_s":427,"heap":103956,"fw":"0.2.1"}', NOW)
    m = by_metric(p.samples)
    assert m["eden_node_rssi_dbm"].value == -24
    assert m["eden_node_uptime_s"].value == 427
    assert m["eden_node_heap_bytes"].value == 103956
    assert m["eden_node_rssi_dbm"].ts == 1794382687


def test_ack_done_dose_and_failed():
    p = parse("eden/n1/ack/d-20260810-0031", b'{"id":"d-20260810-0031","state":"done","ml_actual":50.1,"ms":31400}', NOW)
    assert by_metric(p.samples)["eden_dose_ml"].value == 50.1
    assert p.events[0].kind == "ack" and p.events[0].data["state"] == "done"

    p = parse("eden/n1/ack/s-1", b'{"id":"s-1","state":"done"}', NOW)  # non-dose done: no metric
    assert p.samples == [] and p.events[0].data["id"] == "s-1"

    p = parse("eden/n1/ack/d-2", b'{"id":"d-2","state":"failed","reason":"unhomed"}', NOW)
    s = by_metric(p.samples)["eden_cmd_failed"]
    assert s.labels == {"node": "n1", "reason": "unhomed"}


def test_ignored_and_malformed():
    assert parse("eden/n1/cmd/system", b'{"id":"x","action":"reboot"}', NOW).samples == []
    assert parse("eden/edend/status", b'{"online":true}', NOW).samples == []
    assert parse("eden/system/db", b'{}', NOW).samples == []
    assert parse("other/topic", b'{}', NOW).samples == []
    assert "bad json" in parse("eden/n1/sensor/x", b'{not json', NOW).note
    assert parse("eden/n1/sensor/x", b'[1,2]', NOW).samples == []
    assert parse("eden/n1/sensor/x", b'\xff\xfe', NOW).samples == []
