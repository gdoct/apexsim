# Smaller recorded sessions

Status: phases 1 and 2 built (column blocks of ten seconds, version 2 files; docs/game/spectator.md): 6.41 MB -> 2.56 MB for 16 GT3s over 6.4 min. Phases 3 and 4 not built.

Every race the server finishes, every session a client is in, every
showcase and every promo clip is a spectator stream (`.apxs`,
[../game/spectator.md](../game/spectator.md)). The format is already
compressed, but it is laid out for the wire, not for the disk, and nothing
ever deletes a server's recordings. This proposal cuts a stream file to
about 40% of its size without losing a bit, keeps zlib on both sides, and
bounds what the recordings may occupy.

## Where the bytes go (measured)

Two renders with `apexsim-replay render --class GT3 --cars 20 --seed 7`
(the class's grid seats 16): Zandvoort, 3 laps, dry; Spa, 2 laps, rain.

| | Zandvoort dry | Spa rain |
|---|---|---|
| Length | 6.3 min | 6.6 min |
| File | 5.99 MB | 4.92 MB |
| Rate | 15.8 kB/s | 12.4 kB/s |
| Inflated records | 14.2 MB | 15.3 MB |
| zlib ratio | 2.4x | 3.1x |

At 16 cars that is 45 to 57 MB an hour of racing, on the server and again
on every client that was in the session.

Share of the compressed blocks by record:

| Record | Share |
|---|---|
| `Frame` (30 Hz, 64-byte row per car) | 87-88% |
| `Road` | 6-7% |
| `State` (4 Hz, 44 bytes per car) | 6% |
| `Event` | under 0.1% |

Frames are the whole problem. A frame is self-contained: every car's
absolute pose in mm, angles, speed, rpm and station, interleaved car after
car (`StreamCarRow::write`). That is right on UDP, where any datagram may be
lost, but zlib's 32 kB window over interleaved rows finds little to
fold: the same field of the same car sits 64 bytes x 16 cars apart, and its
value changes every frame.

## What was tried

Same two files, the record bodies taken out of today's blocks and packed
again. Everything below is lossless: the records that come out are the
records that went in.

| Packing | Zandvoort | Spa | vs today |
|---|---|---|---|
| zlib-6, 1 s blocks (today) | 5.98 MB | 4.90 MB | 1.00x |
| zlib-9, 1 s | 5.93 MB | 4.87 MB | 1.01x |
| zstd-3, 1 s | 5.54 MB | 4.46 MB | 1.08-1.10x |
| zstd-19, 1 s | 4.71 MB | 3.88 MB | 1.26-1.27x |
| zstd-19, 10 s | 4.47 MB | 3.59 MB | 1.34-1.36x |
| zstd-19, whole file (no index; a bound) | 4.23 MB | 3.38 MB | 1.41-1.45x |
| columns, zstd-3, 1 s | 4.20 MB | 3.45 MB | 1.42x |
| **columns + delta, zlib-6, 1 s** | **3.19 MB** | **2.51 MB** | **1.88-1.95x** |
| **columns + delta, zlib-6, 10 s** | **2.55 MB** | **1.83 MB** | **2.35-2.68x** |
| columns + delta, zstd-3, 10 s | 2.59 MB | 1.88 MB | 2.30-2.61x |
| columns + delta, zstd-19, 10 s | 2.20 MB | 1.56 MB | 2.72-3.15x |
| frames at 15 Hz instead of 30 (lossy) | 3.62 MB | 3.07 MB | 1.60-1.65x |

Two conclusions:

- **The codec is not the lever, the layout is.** Swapping zlib for zstd
  buys 8-10% at a comparable speed, while rearranging the same bytes
  buys 90% with zlib unchanged. On the rearranged bytes zlib-6 even edges out
  zstd-3. The client already inflates with `FCompression` `NAME_Zlib`
  (`ApexSpectatorWriter.cpp`), so keeping zlib keeps Unreal free of a new
  third-party library.
- **Columns + delta beats halving the frame rate**, and loses nothing.

"Columns + delta" means, inside one block:

1. The frame headers apart from the rows: tick as the difference from the
   frame before, then roster revision, state, countdown, part, parts.
2. Each frame's car order (the `car_index` bytes), so the rows can be put
   back where they were.
3. Per car, per field, the field's values over the block's frames. `x`,
   `y`, `z`, `yaw` (wrapped), `pitch`, `roll`, `speed`, `rpm` and `station`
   are stored as the difference from the car's previous frame. The first
   frame in the block is the difference from 0, so a block still stands on
   its own.
4. Each multi-byte column split into byte planes (all low bytes, then all
   high bytes), so the mostly zero high bytes of a delta run together.
5. The other records (`Event`, `State`, `Road`, `Ghost`) framed as today,
   after the columns.

What is left after that, Zandvoort, 10 s blocks:

| Field | Share |
|---|---|
| wheel slip ratio and angle (8 bytes) | 25% |
| `x`, `y` | 10% each |
| `yaw` | 9% |
| `pitch` | 7% |
| `rpm` | 7% |
| `z` | 6% |
| `speed`, `roll` | 5% each |
| steering, station, pedals | 4%, 4%, 5% |
| everything else | under 3% |

Wheel slip is now the largest single item: eight noisy bytes per car per
frame, which only the watched car's road sound reads
(`AApexRaceDirector::UpdateWatchedCarRoadSound`). See phase 3.

The scripts are a few dozen lines of Python (`msgpack`, `numpy`) over the
records `Block::bodies` returns. They can be added under `scripts/` as
`replay_size.py` alongside phase 1, so a change to the row layout can be
measured the same way.

## Plan

Each phase lands on its own.

### Phase 1: a columnar block (file only)

A new record, `ColumnBlock` (type 11), with the same fields as `Block`
plus a layout byte (1 = the columns + delta above). The `Index` is
unchanged: a block is still found by its first tick and offset, and still
inflates to the same record bodies in the same order, so everything that
takes bodies out of a block (`showcase::catch_up_bodies`, `Recording`,
`FApexSpectatorPlayer`, `info`, `cut`, `convert`) sees no difference.

- **Server**: `spectator.rs` gains `ColumnBlock::pack(bodies)` and
  `::bodies()`; `stream_recorder::write_blocks` and the render path
  (`spectator.rs` around `block_ticks`) write it. `FILE_VERSION` goes to 2,
  so an older client or tool refuses the file by its version instead of
  playing a race with no frames; readers keep reading version 1.
- **Client**: `FApexStreamFile` unpacks a `ColumnBlock` into bodies;
  `FApexStreamWriter` writes one. The client's zlib need not match the
  server's byte for byte (as today, `ApexSpectatorWriter.h`), but the
  unpacked bodies must, so the golden `File` in `ApexSpectatorGoldenBlobs.h`
  and `BackdropTests.cpp` gets a version 2 copy, and the version 1 copy
  stays to prove old files still play.
- **The wire does not change.** Showcases and live streams send record
  bodies, never blocks: frames stay self-contained datagrams.
- **Determinism**: same seed, same bodies, same packing. `render`'s
  byte-identical guarantee holds as long as both use the same `flate2`
  level, which `write_blocks` already pins.
- **Checks**: a round trip test (`bodies` of `pack` of any recorded block is
  the input), the existing `a_session_is_recorded_as_a_stream_file`, and
  `apexsim-replay info --check` on every file in `build/showcase/` after a
  re-render.

Expected: about 1.9x smaller files; 16 cars from 15.8 kB/s to about 8 kB/s.

### Phase 2: ten-second blocks

`block_ticks` from one second to ten (`BLOCK_SECONDS`, one constant on
both sides). Another 1.25-1.4x on top of phase 1.

The cost is seek granularity, and today nothing pays it: the client reads
and inflates the whole file on the thread pool before playing
(`UApexSpectatorSubsystem`, File source), and a showcase inflates its file
on the first viewer's join. Only a tool that reads one block by the index
reads ten times more per lookup. The server's writer queue holds blocks:
`QUEUE_BLOCKS` 512 becomes 51 for the same minutes of headroom.

### Phase 3: what the frames carry

Lossy choices, each measured with the script before it is taken:

- **Wheel slip** for every car at 30 Hz is a quarter of a phase 1 file.
  Options: slip at 10 Hz in the `State` record instead of every row, or
  coarser quantisation (`ROW_SLIP_SCALE`). The road sound is the judge
  ([../game/audio.md](../game/audio.md)).
- **Pose precision**: mm for `x`, `y`, `z` is finer than the motion buffer
  can show; cm halves the entropy of those three columns' deltas.
- **A second-order delta** (velocity, not position, as the predictor) for
  `x`, `y`, `yaw`. Not measured yet.

### Phase 4: how much is kept

Smaller files do not stop a folder from growing:

- **Server**: `replays/` has no limit. `stream_recorder::history` lists the
  newest 200, but the files beyond stay on disk. Add
  `[records] keep_replays` (count) and `keep_replays_mb` (size), oldest
  first, the `.apxs` and its `.meta.json` together, at the end of
  `Recording::finish`. Default: unlimited, so nothing changes for an
  operator who has not asked ([../server/operations.md](../server/operations.md)).
- **Client**: `Saved/Replays/Recent/` keeps 10 (`MaxRecent`); kept replays
  in `Saved/Replays/` have no limit. A size line on the Replays screen is
  enough there; the player decides.
- **Timeshift**: a live stream is kept inflated up to `LiveBufferCapBytes`
  (256 MB). At the inflated rate (about 37 kB/s for 16 cars) that is two
  hours. Holding it packed by phase 1's blocks would hold ten times as much
  in the same memory, at the cost of unpacking a block on a seek.

## Housekeeping found on the way

- `replays/` at the code root holds two committed
  `.recording_<session>.frames` files (2.7 MB): temporary files of an older
  recorder (one zlib stream of named-field MessagePack telemetry,
  little-endian lengths), which the current recorder neither writes nor
  reads (it writes `.recording_<session>.blocks`). They can be deleted.
- `.gitignore` ignores `server/replays/` but not `/replays/`, which is
  where a server run from the code root writes (`./replays`,
  [../server/operations.md](../server/operations.md)). That is how those
  two got in.
- `replay.rs`'s `.bin` (the offline tools' format) is full named-field
  telemetry per frame, the same shape as those old files (about 6.5 kB a
  frame for 8 cars before zlib). It is scratch for the promo tools and
  never shipped, so this proposal leaves it alone.
