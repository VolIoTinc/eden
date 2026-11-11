"""Wiring: MQTT in → ingest → VictoriaMetrics; presence → ntfy.

Runs forever under systemd. Reconnects to the broker with backoff; publishes its own
retained status with an LWT so the dashboard can show when edend itself is down.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import socket
import time

import aiomqtt
import httpx

from . import __version__
from .alerts import Ntfy
from .config import Config, load_config
from .ingest import Sample, parse
from .presence import Presence
from .vm import VMWriter

log = logging.getLogger("edend")

STATUS_TOPIC = "eden/edend/status"
SUBSCRIPTIONS = ("eden/+/status", "eden/+/sensor/+", "eden/+/telemetry", "eden/+/ack/+")


class App:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.http = httpx.AsyncClient(timeout=10)
        self.ntfy = Ntfy(cfg.ntfy, self.http)
        self.vm = VMWriter(self._post_vm, flush_interval_s=cfg.victoriametrics.flush_interval_s,
                           flush_max_lines=cfg.victoriametrics.flush_max_lines,
                           spool_dir=cfg.victoriametrics.spool_dir,
                           spool_max_mb=cfg.victoriametrics.spool_max_mb,
                           on_state=self._vm_state)
        self.presence = Presence(cfg.alerts.node_offline_debounce_s, self._presence_change)
        self._alert_q: asyncio.Queue[tuple] = asyncio.Queue()
        self.stop = asyncio.Event()
        self.msgs = 0

    # -- callbacks -------------------------------------------------------------
    async def _post_vm(self, body: str) -> bool:
        try:
            r = await self.http.post(self.cfg.victoriametrics.write_url, content=body.encode())
            if r.status_code // 100 == 2:
                return True
            log.warning("VM import %s: %s", r.status_code, r.text[:200])
            return False
        except httpx.HTTPError as e:
            log.debug("VM post failed: %s", e)
            return False

    def _vm_state(self, ok: bool) -> None:
        if self.cfg.alerts.vm_down_notify:
            if ok:
                self._alert_q.put_nowait(("VictoriaMetrics back", "ingest resumed, spool replaying", "default", ("white_check_mark",)))
            else:
                self._alert_q.put_nowait(("VictoriaMetrics unreachable", "edend is spooling readings to disk", "high", ("warning",)))

    def _presence_change(self, node: str, online: bool, dur: float) -> None:
        mins = int(dur // 60)
        if online:
            self._alert_q.put_nowait((f"{node} back online", f"was offline {mins} min", "default", ("seedling",)))
        else:
            self._alert_q.put_nowait((f"{node} offline", f"no status for {mins} min", "high", ("warning",)))

    # -- tasks -----------------------------------------------------------------
    async def alert_worker(self) -> None:
        while True:
            title, msg, prio, tags = await self._alert_q.get()
            await self.ntfy.send(title, msg, priority=prio, tags=tags)

    async def presence_loop(self) -> None:
        """Debounce ticks every 10 s; re-emit every node's online state every reemit_s so the
        dashboard's 'online' stat never goes stale between (rare) status messages."""
        tick_s = 10.0
        since_emit = 0.0
        while True:
            await asyncio.sleep(tick_s)
            self.presence.tick()
            since_emit += tick_s
            if since_emit >= self.cfg.presence_reemit_s:
                since_emit = 0.0
                now = time.time()
                for node, online in self.presence.snapshot().items():
                    self.vm.add(Sample("eden_node_online", 1.0 if online else 0.0, now, {"node": node}))
                self.vm.add(Sample("eden_edend_up", 1.0, now, {}))

    async def _consume(self, client: aiomqtt.Client) -> None:
        async for msg in client.messages:
            self._handle(msg.topic.value, msg.payload)

    async def mqtt_loop(self) -> None:
        m = self.cfg.mqtt
        backoff = 2.0
        while not self.stop.is_set():
            try:
                will = aiomqtt.Will(STATUS_TOPIC, json.dumps({"online": False}), qos=1, retain=True)
                async with aiomqtt.Client(m.host, m.port, username=m.username, password=m.password,
                                          identifier=m.client_id, will=will, keepalive=30) as client:
                    log.info("MQTT connected to %s:%d as %s", m.host, m.port, m.username)
                    backoff = 2.0
                    await client.publish(STATUS_TOPIC, json.dumps({
                        "online": True, "fw": __version__, "host": socket.gethostname()}), qos=1, retain=True)
                    for t in SUBSCRIPTIONS:
                        await client.subscribe(t, qos=1)
                    consume = asyncio.create_task(self._consume(client))
                    stopper = asyncio.create_task(self.stop.wait())
                    done, _ = await asyncio.wait({consume, stopper}, return_when=asyncio.FIRST_COMPLETED)
                    if consume in done:
                        stopper.cancel()
                        consume.result()  # re-raises MqttError on broker disconnect
                    else:
                        consume.cancel()
                        await client.publish(STATUS_TOPIC, json.dumps({"online": False, "reason": "shutdown"}),
                                             qos=1, retain=True)
                        return
            except aiomqtt.MqttError as e:
                if self.stop.is_set():
                    return
                log.warning("MQTT: %s; retry in %.0fs", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 60)

    def _handle(self, topic: str, payload) -> None:
        self.msgs += 1
        p = parse(topic, payload, time.time())
        if p.note:
            log.debug("%s: %s", topic, p.note)
        self.vm.add_many(p.samples)
        for ev in p.events:
            if ev.kind == "status":
                online = ev.data["online"]
                self.presence.update(ev.node, online, {k: ev.data.get(k) for k in ("fw", "ip") if k in ev.data})
                log.info("%s %s%s", ev.node, "online" if online else "OFFLINE",
                         f" fw={ev.data.get('fw')} ip={ev.data.get('ip')}" if online else
                         f" reason={ev.data.get('reason')}" if ev.data.get("reason") else "")
            elif ev.kind == "ack":
                st = ev.data.get("state")
                log.info("ack %s %s %s", ev.node, ev.data.get("id"), st)
                if st == "failed":
                    self._alert_q.put_nowait((f"{ev.node}: command failed",
                                              f"{ev.data.get('id')}: {ev.data.get('reason')}", "high", ("x",)))
                elif st == "done" and self.cfg.alerts.dose_done_notify and "ml_actual" in ev.data:
                    self._alert_q.put_nowait((f"{ev.node}: dosed", f"{ev.data['ml_actual']} mL", "low", ("droplet",)))

    async def stats_loop(self) -> None:
        while True:
            await asyncio.sleep(600)
            log.info("stats: msgs=%d vm_lines=%d spooled=%d nodes=%s", self.msgs, self.vm.lines_written,
                     self.vm.lines_spooled, self.presence.snapshot())

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, self.stop.set)
        tasks = [asyncio.create_task(c(), name=n) for n, c in (
            ("vm", self.vm.run), ("alerts", self.alert_worker), ("presence", self.presence_loop),
            ("stats", self.stats_loop), ("mqtt", self.mqtt_loop))]
        await self.stop.wait()
        log.info("shutting down")
        # Let the MQTT loop publish offline status, then flush what we have.
        mqtt_task = tasks[-1]
        try:
            await asyncio.wait_for(mqtt_task, timeout=5)
        except (asyncio.TimeoutError, Exception):
            mqtt_task.cancel()
        for t in tasks[:-1]:
            t.cancel()
        await self.vm.flush()
        await self.http.aclose()


def cli() -> None:
    logging.basicConfig(level=os.environ.get("EDEND_LOG", "INFO").upper(),
                        format="%(asctime)s %(levelname).1s %(name)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)  # one line per flush otherwise
    cfg = load_config()
    log.info("edend %s starting; mqtt=%s:%d vm=%s ntfy=%s", __version__, cfg.mqtt.host, cfg.mqtt.port,
             cfg.victoriametrics.write_url, "on" if cfg.ntfy.enabled else "log-only")
    asyncio.run(App(cfg).run())
