"""Node presence from the retained `eden/<node>/status` topic, with debounce.

A node's LWT flips status to online:false the moment the broker loses it. WiFi
hiccups are common, so we only alert after `debounce_s` of continuous absence,
and send one "back" notice when it returns after having been alerted.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable

AlertFn = Callable[[str, bool, float], None]  # node, online, offline_duration_s


@dataclass
class NodeState:
    online: bool
    since: float          # when the current state began
    alerted: bool = False  # offline alert has been sent for this outage
    info: dict = field(default_factory=dict)


class Presence:
    def __init__(self, debounce_s: float, on_change: AlertFn | None = None,
                 clock: Callable[[], float] = time.monotonic):
        self.debounce_s = debounce_s
        self.on_change = on_change
        self.clock = clock
        self.nodes: dict[str, NodeState] = {}

    def update(self, node: str, online: bool, info: dict | None = None) -> None:
        now = self.clock()
        st = self.nodes.get(node)
        if st is None:
            self.nodes[node] = NodeState(online, now, info=info or {})
            return
        if info:
            st.info.update(info)
        if st.online == online:
            return
        if online and st.alerted and self.on_change:
            self.on_change(node, True, now - st.since)
        st.online, st.since, st.alerted = online, now, False

    def tick(self) -> None:
        """Call periodically: fires offline alerts whose debounce has elapsed."""
        now = self.clock()
        for node, st in self.nodes.items():
            if not st.online and not st.alerted and now - st.since >= self.debounce_s:
                st.alerted = True
                if self.on_change:
                    self.on_change(node, False, now - st.since)

    def snapshot(self) -> dict[str, bool]:
        return {n: s.online for n, s in self.nodes.items()}
