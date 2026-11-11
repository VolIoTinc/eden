"""Configuration: /etc/eden/edend.yaml plus a few environment overrides.

Secrets never live in the repo. On the Pi the MQTT password comes from
/etc/eden/mqtt-edend.env (MQTT_EDEND_PASSWORD) via the systemd EnvironmentFile,
and the ntfy credential from a `user:pass` file (ntfy.cred_file).
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

import yaml

PLACEHOLDERS = {"", "CHANGE_ME", "FILL_ME_IN"}


@dataclass
class MqttConfig:
    host: str = "127.0.0.1"
    port: int = 1883
    username: str = "edend"
    password: str = ""
    client_id: str = "edend"


@dataclass
class VMConfig:
    # Prometheus text-exposition import: lets us name metrics exactly.
    write_url: str = "http://127.0.0.1:8428/api/v1/import/prometheus"
    flush_interval_s: float = 10
    flush_max_lines: int = 500
    spool_dir: str = "/var/lib/edend/spool"
    spool_max_mb: float = 64


@dataclass
class NtfyConfig:
    url: str = ""
    topic: str = "eden_alerts"
    username: str = ""
    password: str = ""
    cred_file: str = "/etc/eden/ntfy.cred"  # "user:pass"; same file db-size-check uses

    @property
    def enabled(self) -> bool:
        return bool(self.url) and "<" not in self.url and self.username not in PLACEHOLDERS \
            and self.password not in PLACEHOLDERS


@dataclass
class AlertsConfig:
    node_offline_debounce_s: float = 120
    dose_done_notify: bool = False
    vm_down_notify: bool = True


@dataclass
class Config:
    mqtt: MqttConfig = field(default_factory=MqttConfig)
    victoriametrics: VMConfig = field(default_factory=VMConfig)
    ntfy: NtfyConfig = field(default_factory=NtfyConfig)
    alerts: AlertsConfig = field(default_factory=AlertsConfig)
    plants_file: str = "/etc/eden/plants.yaml"
    presence_reemit_s: float = 60  # re-write eden_node_online for every node this often


def _fill(dc_type, data: dict | None):
    data = data or {}
    known = {f for f in dc_type.__dataclass_fields__}
    return dc_type(**{k: v for k, v in data.items() if k in known})


def load_config(path: str | os.PathLike | None = None) -> Config:
    path = Path(path or os.environ.get("EDEND_CONFIG", "/etc/eden/edend.yaml"))
    raw: dict = {}
    if path.exists():
        raw = yaml.safe_load(path.read_text()) or {}

    cfg = Config(
        mqtt=_fill(MqttConfig, raw.get("mqtt")),
        victoriametrics=_fill(VMConfig, raw.get("victoriametrics")),
        ntfy=_fill(NtfyConfig, raw.get("ntfy")),
        alerts=_fill(AlertsConfig, raw.get("alerts")),
        plants_file=raw.get("plants_file", Config.plants_file),
        presence_reemit_s=raw.get("presence_reemit_s", Config.presence_reemit_s),
    )

    # Environment overrides (systemd EnvironmentFile on the Pi).
    env_pw = os.environ.get("MQTT_EDEND_PASSWORD")
    if env_pw:
        cfg.mqtt.password = env_pw
    if cfg.mqtt.password in PLACEHOLDERS:
        raise SystemExit("mqtt.password not set (edend.yaml or MQTT_EDEND_PASSWORD)")

    cred = Path(cfg.ntfy.cred_file)
    if cfg.ntfy.password in PLACEHOLDERS and cred.is_file():
        user, _, pw = cred.read_text().strip().partition(":")
        if user and pw:
            cfg.ntfy.username, cfg.ntfy.password = user, pw
    return cfg
