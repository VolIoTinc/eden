import asyncio

import pytest

from edend.ingest import Sample
from edend.vm import VMWriter, format_line


def test_format_line():
    s = Sample("eden_moisture", 21.36, 1794382686.0, {"unit": "%", "node": "jimothy"})
    assert format_line(s) == 'eden_moisture{node="jimothy",unit="%"} 21.36 1794382686000'
    assert format_line(Sample("eden_edend_up", 1.0, 1.5, {})) == "eden_edend_up 1.0 1500"
    assert format_line(Sample("m", 1.0, 0, {"a": 'q"\\\n'})) == 'm{a="q\\"\\\\\\n"} 1.0 0'


class FakeVM:
    def __init__(self):
        self.ok = True
        self.bodies: list[str] = []

    async def post(self, body: str) -> bool:
        if not self.ok:
            return False
        self.bodies.append(body)
        return True


def run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def test_flush_posts_buffer(tmp_path):
    async def go():
        vm = FakeVM()
        w = VMWriter(vm.post, spool_dir=tmp_path)
        w.add(Sample("a", 1, 1, {}))
        w.add(Sample("b", 2, 2, {}))
        await w.flush()
        assert vm.bodies == ["a 1 1000\nb 2 2000\n"]
        assert w.up is True and w.lines_written == 2
        await w.flush()  # empty buffer: no post
        assert len(vm.bodies) == 1
    run(go())


def test_spool_and_replay(tmp_path):
    async def go():
        vm = FakeVM()
        states = []
        w = VMWriter(vm.post, spool_dir=tmp_path, on_state=states.append)
        vm.ok = False
        w.add(Sample("a", 1, 1, {}))
        await w.flush()
        assert w.up is False and states == [False]
        files = list(tmp_path.glob("*.prom"))
        assert len(files) == 1 and files[0].read_text() == "a 1 1000\n"
        assert w.lines_spooled == 1

        w.add(Sample("b", 2, 2, {}))
        await w.flush()
        assert len(list(tmp_path.glob("*.prom"))) == 2

        vm.ok = True
        w.add(Sample("c", 3, 3, {}))
        await w.flush()
        # live batch first, then spool replayed oldest-first
        assert vm.bodies == ["c 3 3000\n", "a 1 1000\n", "b 2 2000\n"]
        assert list(tmp_path.glob("*.prom")) == []
        assert states == [False, True]
        assert w.lines_written == 3
    run(go())


def test_spool_cap_drops_oldest(tmp_path):
    async def go():
        vm = FakeVM()
        vm.ok = False
        w = VMWriter(vm.post, spool_dir=tmp_path, spool_max_mb=0.0001)  # ~105 bytes
        for i in range(5):
            w.add(Sample("metric_with_a_longish_name", i, i, {"node": "x"}))  # ~50 bytes each
            await w.flush()
            await asyncio.sleep(0.001)
        files = sorted(tmp_path.glob("*.prom"))
        assert 1 <= len(files) <= 2
        assert "4 4000" in files[-1].read_text()  # newest kept
    run(go())


def test_replay_skipped_while_down(tmp_path):
    async def go():
        vm = FakeVM()
        (tmp_path / "1.prom").write_text("old 1 1000\n")
        w = VMWriter(vm.post, spool_dir=tmp_path)
        vm.ok = False
        w.add(Sample("a", 1, 1, {}))
        await w.flush()
        assert vm.bodies == []
        assert len(list(tmp_path.glob("*.prom"))) == 2
        vm.ok = True
        await w.flush()  # nothing buffered, but up is unknown→ replay attempted
        assert vm.bodies == []  # up is False: replay waits for a live success
        w.add(Sample("b", 2, 2, {}))
        await w.flush()
        assert vm.bodies[0] == "b 2 2000\n" and len(vm.bodies) == 3
    run(go())


def test_kick_on_max_lines(tmp_path):
    w = VMWriter(FakeVM().post, spool_dir=tmp_path, flush_max_lines=2)
    w.add(Sample("a", 1, 1, {}))
    assert not w._kick.is_set()
    w.add(Sample("b", 1, 1, {}))
    assert w._kick.is_set()
