#!/usr/bin/env python3
"""Lap traces to a MoTeC i2 log: an ``.ld`` and its ``.ldx`` beside it.

The client writes every whole lap a player drives as CSV
(``Saved/Telemetry/<circuit>/<car>/*.csv``, ``ApexLapTrace::ToCsv``): a
sample every 2 m of station. This turns one lap, or several driven back to
back, into the log MoTeC i2 (and anything else that reads ``.ld``) opens:
the samples resampled on time at ``--rate`` Hz (20 by default), the laps
one after another with a beacon at each line in the ``.ldx``.

    python scripts/motec_export.py lap.csv                  # lap.ld + lap.ldx
    python scripts/motec_export.py a.csv b.csv -o stint.ld  # two laps, one log
    python scripts/motec_export.py lap.csv --driver "Guido"

Channels: Ground Speed (km/h), Throttle Pos and Brake Pos (%), Steering
(% of lock, + right), Gear, Engine RPM, the four Tyre Temp (C, tread), Lap
Distance (m), Car Pos X / Y (m, the track's frame) and Lap Number.

The ``.ld`` layout is MoTeC's own and undocumented; it is written as the
open readers parse it (a 0x40 marker, a header with pointers to the
channel list and the event block, then a linked list of channel headers
and the samples as float32). ``read_ld`` reads it back the same way.
"""

from __future__ import annotations

import argparse
import datetime as dt
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

HEAD_FMT = "<" + (
    "I4x"  # marker 0x40
    "II"  # first channel header, first channel's data
    "20x"
    "I"  # event block
    "24x"
    "HHH"  # constants the loggers write: 1, 0x4240, 0xF
    "I"  # device serial
    "8s"  # device type
    "H"  # device version
    "H"  # constant 0xADB0
    "I"  # channel count
    "4x"
    "16s"  # date dd/mm/yyyy
    "16x"
    "16s"  # time hh:mm:ss
    "16x"
    "64s"  # driver
    "64s"  # vehicle id
    "64x"
    "64s"  # venue
    "64x"
    "1024x"
    "I"  # pro logging constant
    "66x"
    "64s"  # short comment
    "126x"
)
EVENT_FMT = "<64s64s1024sH"  # name, session, comment, venue block
VENUE_FMT = "<64s1034xH"  # name, vehicle block
VEHICLE_FMT = "<64s128xI32s32s"  # id, weight kg, type, comment
CHAN_FMT = "<" + (
    "IIII"  # previous header, next header, data, sample count
    "H"  # channel counter
    "HHH"  # data kind (7: float), its size (4), frequency Hz
    "hhhh"  # shift, multiplier, scale, decimal places
    "32s"  # name
    "8s"  # short name
    "12s"  # unit
    "40x"
)

# (CSV column, name, short name, unit, scale from the CSV's value)
CHANNELS = [
    ("speed_kph", "Ground Speed", "Speed", "km/h", 1.0),
    ("throttle", "Throttle Pos", "Thr", "%", 100.0),
    ("brake", "Brake Pos", "Brk", "%", 100.0),
    ("steering", "Steering", "Steer", "%", 100.0),
    ("gear", "Gear", "Gear", "", 1.0),
    ("rpm", "Engine RPM", "RPM", "rpm", 1.0),
    ("tyre_fl_c", "Tyre Temp FL", "TTFL", "C", 1.0),
    ("tyre_fr_c", "Tyre Temp FR", "TTFR", "C", 1.0),
    ("tyre_rl_c", "Tyre Temp RL", "TTRL", "C", 1.0),
    ("tyre_rr_c", "Tyre Temp RR", "TTRR", "C", 1.0),
    ("distance_m", "Lap Distance", "LapDist", "m", 1.0),
    ("x", "Car Pos X", "PosX", "m", 1.0),
    ("y", "Car Pos Y", "PosY", "m", 1.0),
]
# Held, not blended, between samples.
STEPPED = {"gear"}


@dataclass
class Lap:
    """One lap trace as the client wrote it."""

    track: str
    car: str
    lap: int
    lap_ms: int
    valid: bool
    recorded: dt.datetime | None
    columns: dict[str, np.ndarray] = field(default_factory=dict)


def read_lap_csv(path: Path) -> Lap:
    lines = path.read_text(encoding="utf-8").splitlines()
    if len(lines) < 3 or not lines[0].startswith("# apexsim lap trace 1;"):
        raise ValueError(f"{path}: not an apexsim lap trace")
    meta = {}
    for part in lines[0][1:].split(";")[1:]:
        if "=" in part:
            key, value = part.strip().split("=", 1)
            meta[key] = value
    names = lines[1].split(",")
    rows = np.array([[float(v) for v in line.split(",")] for line in lines[2:] if line.strip()])
    recorded = None
    if meta.get("recorded"):
        try:
            recorded = dt.datetime.fromisoformat(meta["recorded"].replace("Z", "+00:00"))
        except ValueError:
            pass
    return Lap(
        track=meta.get("track", ""),
        car=meta.get("car", ""),
        lap=int(meta.get("lap", "0") or 0),
        lap_ms=int(meta.get("lap_ms", "0") or 0),
        valid=meta.get("valid") == "1",
        recorded=recorded,
        columns={name: rows[:, i] for i, name in enumerate(names)},
    )


def resample(laps: list[Lap], rate: int) -> tuple[dict[str, np.ndarray], list[float]]:
    """The laps back to back on one clock at `rate` Hz; each channel's
    samples, and the time each lap ended, s."""
    out: dict[str, list[np.ndarray]] = {c[0]: [] for c in CHANNELS}
    out["lap_number"] = []
    ends: list[float] = []
    start = 0.0
    for lap in laps:
        t = lap.columns["time_s"]
        length = lap.lap_ms / 1000.0 if lap.lap_ms > 0 else float(t[-1])
        clock = np.arange(0.0, length, 1.0 / rate)
        for column, *_ in CHANNELS:
            values = lap.columns[column]
            if column in STEPPED:
                idx = np.clip(np.searchsorted(t, clock, side="right") - 1, 0, len(t) - 1)
                out[column].append(values[idx])
            else:
                out[column].append(np.interp(clock, t, values))
        out["lap_number"].append(np.full(len(clock), float(lap.lap)))
        start += len(clock) / rate
        ends.append(start)
    return {k: np.concatenate(v).astype(np.float32) for k, v in out.items()}, ends


def _s(text: str, n: int) -> bytes:
    return text.encode("ascii", "replace")[:n]


def write_ld(path: Path, laps: list[Lap], rate: int = 20, driver: str = "") -> list[float]:
    """Write the laps as `path`; the time each lap ended, s."""
    data, ends = resample(laps, rate)
    first = laps[0]
    when = first.recorded or dt.datetime.now()
    channels = [(c[0], c[1], c[2], c[3], c[4]) for c in CHANNELS]
    channels.append(("lap_number", "Lap Number", "Lap", "", 1.0))

    head = struct.calcsize(HEAD_FMT)
    event_ptr = head
    venue_ptr = event_ptr + struct.calcsize(EVENT_FMT)
    vehicle_ptr = venue_ptr + struct.calcsize(VENUE_FMT)
    meta_ptr = vehicle_ptr + struct.calcsize(VEHICLE_FMT)
    chan_size = struct.calcsize(CHAN_FMT)
    data_ptr = meta_ptr + len(channels) * chan_size
    count = len(data["lap_number"])

    with open(path, "wb") as f:
        f.write(
            struct.pack(
                HEAD_FMT,
                0x40,
                meta_ptr,
                data_ptr,
                event_ptr,
                1,
                0x4240,
                0xF,
                0x1F44,
                b"ADL",
                420,
                0xADB0,
                len(channels),
                when.strftime("%d/%m/%Y").encode(),
                when.strftime("%H:%M:%S").encode(),
                _s(driver, 64),
                _s(first.car, 64),
                _s(first.track, 64),
                0xC81A4,
                _s("apexsim", 64),
            )
        )
        laps_text = ", ".join(str(lap.lap) for lap in laps)
        f.write(
            struct.pack(
                EVENT_FMT,
                _s(first.track, 64),
                _s(f"lap {laps_text}", 64),
                _s("apexsim lap trace", 1024),
                venue_ptr,
            )
        )
        f.write(struct.pack(VENUE_FMT, _s(first.track, 64), vehicle_ptr))
        f.write(struct.pack(VEHICLE_FMT, _s(first.car, 64), 0, b"", b""))
        assert f.tell() == meta_ptr
        for n, (column, name, short, unit, _) in enumerate(channels):
            this = meta_ptr + n * chan_size
            prev = this - chan_size if n > 0 else 0
            nxt = this + chan_size if n + 1 < len(channels) else 0
            f.write(
                struct.pack(
                    CHAN_FMT,
                    prev,
                    nxt,
                    data_ptr + n * count * 4,
                    count,
                    0x2EE1 + n,
                    0x07,
                    4,
                    rate,
                    0,
                    1,
                    1,
                    0,
                    _s(name, 32),
                    _s(short, 8),
                    _s(unit, 12),
                )
            )
        for column, _, _, _, scale in channels:
            f.write((data[column] * np.float32(scale)).astype("<f4").tobytes())
    write_ldx(path.with_suffix(".ldx"), laps, ends)
    return ends


def lap_time_text(seconds: float) -> str:
    m, s = divmod(seconds, 60.0)
    return f"{int(m)}:{s:06.3f}"


def write_ldx(path: Path, laps: list[Lap], ends: list[float]) -> None:
    """The lap beacons, one at each line, and the fastest lap."""
    markers = "\n".join(
        f'     <Marker Version="100" ClassName="BCN" Name="Manual.{i + 1}" Flags="77" Time="{end * 1e6:.2f}"/>'
        for i, end in enumerate(ends)
    )
    times = [lap.lap_ms / 1000.0 for lap in laps]
    best = min(range(len(laps)), key=lambda i: times[i])
    path.write_text(
        f"""<?xml version="1.0"?>
<LDXFile Locale="English_United Kingdom.1252" DefaultLocale="C" Version="1.6">
 <Layers>
  <Layer>
   <MarkerBlock>
    <MarkerGroup Name="Beacons" Index="3">
{markers}
    </MarkerGroup>
   </MarkerBlock>
   <RangeBlock/>
  </Layer>
  <Details>
   <String Id="Total Laps" Value="{len(laps)}"/>
   <String Id="Fastest Time" Value="{lap_time_text(times[best])}"/>
   <String Id="Fastest Lap" Value="{best + 1}"/>
  </Details>
 </Layers>
</LDXFile>
""",
        encoding="utf-8",
    )


def read_ld(path: Path) -> dict:
    """The log as written: header strings and each channel's samples by
    name (with `freq` and `unit`)."""
    raw = Path(path).read_bytes()
    head = struct.unpack_from(HEAD_FMT, raw, 0)
    meta_ptr, n = head[1], head[11]
    text = lambda b: b.rstrip(b"\0").decode("ascii")  # noqa: E731
    out = {
        "date": text(head[12]),
        "time": text(head[13]),
        "driver": text(head[14]),
        "vehicle": text(head[15]),
        "venue": text(head[16]),
        "channels": {},
    }
    ptr = meta_ptr
    while ptr:
        c = struct.unpack_from(CHAN_FMT, raw, ptr)
        nxt, data_ptr, count, kind, size, freq = c[1], c[2], c[3], c[5], c[6], c[7]
        if (kind, size) != (0x07, 4):
            raise ValueError(f"channel at {ptr}: not float32")
        samples = np.frombuffer(raw, dtype="<f4", count=count, offset=data_ptr)
        out["channels"][text(c[12])] = {"freq": freq, "unit": text(c[14]), "data": samples}
        ptr = nxt
    if len(out["channels"]) != n:
        raise ValueError(f"{len(out['channels'])} channels linked, header says {n}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("laps", nargs="+", type=Path, help="lap trace CSVs, in driving order")
    ap.add_argument("-o", "--out", type=Path, help="the .ld to write (default: beside the first lap)")
    ap.add_argument("--rate", type=int, default=20, help="samples a second (default 20)")
    ap.add_argument("--driver", default="", help="the driver's name in the log")
    args = ap.parse_args()
    laps = [read_lap_csv(p) for p in args.laps]
    out = args.out or args.laps[0].with_suffix(".ld")
    ends = write_ld(out, laps, args.rate, args.driver)
    print(f"{out}: {len(laps)} lap(s), {ends[-1]:.1f} s at {args.rate} Hz; {out.with_suffix('.ldx').name} beside it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
