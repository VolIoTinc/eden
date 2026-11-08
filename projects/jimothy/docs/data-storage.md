# Jimothy data storage: discussion

**Status: open. Nothing here is decided.** This lays out what's already fixed in Eden,
what's new, the options, and the questions that drive the choice.

## What Eden already decided

- **Sensor time-series → VictoriaMetrics** on the Pi 4B (`docs/architecture.md`).
  Moisture, temp, RH, pressure, IAQ, and pump doses all fit there with no new
  infrastructure: they're just more metrics on `eden/<node>/sensor/<name>`.
- **No Docker** (Serenity NAT). Bare systemd, small RAM footprints.
- **DB size alert at 2 GiB**, with "revisit DB location" as the standing decision when it
  fires.

So the real new question is **pictures**, plus whether we want a place for
**non-numeric plant data** (events, notes, observations).

## Data we'll have

| Data | Shape | Volume (rough) | Fits VM? |
|---|---|---|---|
| Moisture, PTH, IAQ, eCO2, bVOC, lux, UVI | number + timestamp | tiny (<1 MB/day) | ✅ |
| Doses (mL, duration, result) | number + lifecycle | tiny | ✅ |
| Photos | JPEG blobs | **see below**, dominates everything | ❌ |
| Events ("repotted", "new leaf", "fertilized") | text + timestamp | tiny | ⚠️ awkward |
| Photo metadata (exposure, lights on?, linked sensor snapshot) | structured | tiny | ⚠️ awkward |

### Photo volume

| Resolution | ~Size | Every 30 min (48/day) | Every hour (24/day) |
|---|---|---|---|
| 1080p | ~0.5 MB | ~24 MB/day, ~9 GB/yr | ~4.5 GB/yr |
| 12 MP (Cam v3) | ~3–4 MB | ~170 MB/day, ~60 GB/yr | ~30 GB/yr |

Even the small option blows past the 2 GiB threshold within months. It also shouldn't
live on the Pi 4's SD card (write wear, and the card is shared with Serenity). **Photos
need their own disk** (USB SSD on the Pi 4, or a NAS) whichever option we pick.

## Options

### A. Files on disk + VM for everything numeric *(simplest)*
`/srv/eden/photos/jimothy/2026/11/07/143000.jpg`. Pi Zero pushes each shot to the Pi 4
(rsync or HTTP PUT) and edend writes a `photo_taken` metric.
- ✅ Zero new services. Easy to back up, browse, and turn into a timelapse (`ffmpeg`).
- ❌ No good place for metadata, events, or "find photos where RH < 40%".

### B. Files on disk + SQLite catalog *(recommended starting point)*
Same files as A, plus `eden.db` (SQLite) with tables like `photos(ts, plant, path,
sha256, w, h, exposure, lux)`, `events(ts, plant, kind, note)`, and `plants(id,
species, sprouted, medium…)`. Numeric time-series stays in VM.
- ✅ Still no daemon (SQLite is a file). edend owns it. Tiny RAM.
- ✅ Holds the "profiling" side of Eden: plant history, observations, and later ML labels
  ("leaf drooping" on a photo).
- ✅ Grafana can read SQLite (plugin), and can show the latest photo via a static file
  server.
- ❌ Two stores to reason about (VM + SQLite). Joins between them happen in code.

### C. Postgres (+ TimescaleDB) for everything numeric and metadata
- ✅ One SQL store, real joins between sensor data and photos/events.
- ❌ Replaces a working VM setup. Postgres is ~10× the RAM on a Pi that's also a
  cellular gateway. Overkill for one avocado, and probably still overkill for ten.

### D. Object storage (MinIO / Garage) for photos
- ✅ S3 API, nice if photos later go to the cloud or a dataset.
- ❌ Another daemon for what a directory already does. Can be added later on top of A/B,
  since paths map cleanly to object keys.

**Leaning:** B. It keeps everything Eden already built, adds no daemons, and gives
Jimothy a real history, not just graphs. But it hinges on the answers below.

## Questions for Andy

**Camera / Pi Zero**
1. Which Pi Zero (Zero W / Zero 2 W) and camera module? What's built so far, and what's
   left on the "partially made" camera?
2. Is it on the same LAN as the Pi 4 (10.0.0.0/24)? Always powered?
3. Capture cadence: fixed interval, only when grow lights are on, or event-triggered
   (e.g. after each watering)?
4. Lighting: are night shots wanted (needs IR or a light), or is daylight only fine?
5. Resolution: full 12 MP for detail, or 1080p for timelapses? (Or both: full-res
   daily, 1080p hourly.)

**Storage & retention**
6. Where can photos live: a USB SSD on the Pi 4, a NAS, or something else? How big?
7. Keep everything forever, or thin out over time (e.g. hourly for 30 days → daily
   forever)?
8. Off-site backup wanted? (Tailscale to another machine, cloud bucket?)

**Purpose**
9. What will the photos be *used* for: watching him grow (timelapse), checking on him
   remotely, or a dataset for later analysis or ML (leaf health, growth rate)? This
   decides how much metadata is worth capturing now.
10. Eden's README mentions *profiling and distribution* of food-bearing plants. Is
    Jimothy's data meant to be shared or published eventually? That favors B or D with
    clean, documented schemas.

**Pump (Phase 3, related)**
11. Is the printed peristaltic pump stepper-driven (step/dir, as Eden's firmware assumes)
    or a DC motor? What's the reservoir, and what water goes in it?
