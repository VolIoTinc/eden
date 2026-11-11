"""edend — the Eden orchestrator daemon.

Ingests every node's MQTT traffic into VictoriaMetrics, tracks node presence,
and raises ntfy alerts. See docs/mqtt-contract.md for the wire format.
"""

__version__ = "0.1.0"
