from edend.presence import Presence


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def make():
    clk = Clock()
    events = []
    p = Presence(120, lambda node, online, dur: events.append((node, online, round(dur))), clock=clk)
    return p, clk, events


def test_short_blip_no_alert():
    p, clk, ev = make()
    p.update("j", True)
    clk.t += 10
    p.update("j", False)
    clk.t += 60
    p.tick()
    p.update("j", True)
    clk.t += 300
    p.tick()
    assert ev == []
    assert p.snapshot() == {"j": True}


def test_offline_alert_after_debounce_then_back():
    p, clk, ev = make()
    p.update("j", True, {"fw": "0.2.1"})
    clk.t += 10
    p.update("j", False)
    clk.t += 119
    p.tick()
    assert ev == []
    clk.t += 1
    p.tick()
    assert ev == [("j", False, 120)]
    clk.t += 100
    p.tick()
    assert len(ev) == 1  # no repeat
    p.update("j", True)
    assert ev[-1] == ("j", True, 220)
    assert p.nodes["j"].info == {"fw": "0.2.1"}


def test_node_first_seen_offline_alerts_after_debounce():
    p, clk, ev = make()
    p.update("j", False)  # retained LWT at edend startup
    clk.t += 121
    p.tick()
    assert ev == [("j", False, 121)]


def test_duplicate_status_does_not_reset_timer():
    p, clk, ev = make()
    p.update("j", False)
    clk.t += 100
    p.update("j", False)
    clk.t += 25
    p.tick()
    assert ev == [("j", False, 125)]
