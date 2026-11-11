"""ntfy client. Silently a no-op (with a log line) until credentials exist."""

from __future__ import annotations

import logging

import httpx

from .config import NtfyConfig

log = logging.getLogger("edend.alerts")


class Ntfy:
    def __init__(self, cfg: NtfyConfig, client: httpx.AsyncClient | None = None):
        self.cfg = cfg
        self._client = client
        self._warned = False

    async def send(self, title: str, message: str, *, priority: str = "default",
                   tags: tuple[str, ...] = ()) -> bool:
        if not self.cfg.enabled:
            if not self._warned:
                log.warning("ntfy not configured (url/credentials); alerts are log-only")
                self._warned = True
            log.info("ALERT [%s] %s: %s", priority, title, message)
            return False
        headers = {"Title": title, "Priority": priority}
        if tags:
            headers["Tags"] = ",".join(tags)
        url = f"{self.cfg.url.rstrip('/')}/{self.cfg.topic}"
        try:
            client = self._client or httpx.AsyncClient(timeout=10)
            try:
                r = await client.post(url, content=message.encode(), headers=headers,
                                      auth=(self.cfg.username, self.cfg.password))
            finally:
                if self._client is None:
                    await client.aclose()
            if r.status_code // 100 != 2:
                log.warning("ntfy %s: %s", r.status_code, r.text[:200])
                return False
            return True
        except httpx.HTTPError as e:
            log.warning("ntfy unreachable: %s", e)
            return False
