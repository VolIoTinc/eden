"""Batched writer to VictoriaMetrics (Prometheus text import) with a bounded disk spool.

Lines are buffered and POSTed every `flush_interval_s` or `flush_max_lines`.
If VM is unreachable the batch goes to `spool_dir` as a file; once a live POST
succeeds again the spool is replayed oldest-first. Spool is capped at `spool_max_mb`
by deleting the oldest files, so a long VM outage costs history, not the disk.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from pathlib import Path

from .ingest import Sample

log = logging.getLogger("edend.vm")

PostFn = Callable[[str], Awaitable[bool]]  # body -> ok?


def _esc(v: str) -> str:
    return v.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def format_line(s: Sample) -> str:
    """Prometheus exposition line with a millisecond timestamp."""
    labels = ",".join(f'{k}="{_esc(str(v))}"' for k, v in sorted(s.labels.items()))
    lbl = f"{{{labels}}}" if labels else ""
    return f"{s.metric}{lbl} {s.value!r} {int(s.ts * 1000)}"


class VMWriter:
    def __init__(self, post: PostFn, *, flush_interval_s: float = 10, flush_max_lines: int = 500,
                 spool_dir: str | Path = "/var/lib/edend/spool", spool_max_mb: float = 64,
                 on_state: Callable[[bool], None] | None = None):
        self._post = post
        self.flush_interval_s = flush_interval_s
        self.flush_max_lines = flush_max_lines
        self.spool_dir = Path(spool_dir)
        self.spool_max_bytes = int(spool_max_mb * 1024 * 1024)
        self.on_state = on_state
        self._buf: list[str] = []
        self._lock = asyncio.Lock()
        self._kick = asyncio.Event()
        self.up: bool | None = None  # unknown until first flush
        self.lines_written = 0
        self.lines_spooled = 0

    # -- producer side -------------------------------------------------------
    def add(self, sample: Sample) -> None:
        self._buf.append(format_line(sample))
        if len(self._buf) >= self.flush_max_lines:
            self._kick.set()

    def add_many(self, samples) -> None:
        for s in samples:
            self.add(s)

    # -- flushing ------------------------------------------------------------
    async def run(self) -> None:
        while True:
            try:
                await asyncio.wait_for(self._kick.wait(), timeout=self.flush_interval_s)
            except asyncio.TimeoutError:
                pass
            self._kick.clear()
            await self.flush()

    async def flush(self) -> None:
        async with self._lock:
            if self._buf:
                batch, self._buf = self._buf, []
                ok = await self._post("\n".join(batch) + "\n")
                self._set_up(ok)
                if ok:
                    self.lines_written += len(batch)
                else:
                    self._spool(batch)
            if self.up is not False:
                await self._replay()

    def _set_up(self, ok: bool) -> None:
        if ok != self.up:
            if self.up is not None or not ok:
                (log.info if ok else log.warning)("VictoriaMetrics %s", "reachable" if ok else "UNREACHABLE; spooling")
            self.up = ok
            if self.on_state:
                self.on_state(ok)

    # -- spool ---------------------------------------------------------------
    def _spool_files(self) -> list[Path]:
        if not self.spool_dir.is_dir():
            return []
        return sorted(p for p in self.spool_dir.glob("*.prom") if p.is_file())

    def _spool(self, lines: list[str]) -> None:
        try:
            self.spool_dir.mkdir(parents=True, exist_ok=True)
            path = self.spool_dir / f"{time.time_ns()}.prom"
            path.write_text("\n".join(lines) + "\n")
            self.lines_spooled += len(lines)
            self._enforce_cap()
        except OSError as e:
            log.error("spool write failed, dropping %d lines: %s", len(lines), e)

    def _enforce_cap(self) -> None:
        files = self._spool_files()
        total = sum(p.stat().st_size for p in files)
        for p in files:
            if total <= self.spool_max_bytes:
                break
            total -= p.stat().st_size
            p.unlink(missing_ok=True)
            log.warning("spool over %d MB; dropped oldest %s", self.spool_max_bytes // 1048576, p.name)

    async def _replay(self, max_files: int = 20) -> None:
        for p in self._spool_files()[:max_files]:
            try:
                body = p.read_text()
            except OSError:
                p.unlink(missing_ok=True)
                continue
            if not await self._post(body):
                self._set_up(False)
                return
            n = body.count("\n")
            self.lines_written += n
            p.unlink(missing_ok=True)
            log.info("replayed %d spooled lines from %s", n, p.name)
