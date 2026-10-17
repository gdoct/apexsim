# Telemetry output

The client sends one UDP datagram of the car's motion, revs, pedals, tyres
and session to an app on this machine or the network: SimHub, a motion
platform's software, bass shakers, wheel rev lights, a button-box display
or a dash app. Code: `Telemetry/ApexTelemetryOutput.h` (`ApexTelemetryOutput`,
`UApexTelemetryOutputSubsystem`).

It is sent from the client, not the server: the client has the player's car
at telemetry rate, and an app on the player's PC must not need a route to
the server.

## Turning it on

`settings.yml` ([client.md](client.md#startup-settings-settingsyml)):

```yaml
telemetry_out:
  enabled: true
  host: 127.0.0.1   # the app's machine
  port: 9996
  rate_hz: 60       # packets per second while driving, 1 to 400
```

Read at startup. `apexsim.telemetry.Out on|off` switches it for this run;
`apexsim.telemetry.Out` prints where it sends and how many packets went.
The log says `Telemetry output to <addr> at <n> Hz (protocol 1, 216 bytes)`.

It sends while a race view is open (driving, watching, a replay) at
`rate_hz`, and in the menus at 2 Hz with `CarPresent` clear, so an app can
tell the game is running. Motion and the car's figures are refreshed at the
telemetry's own rate (60 Hz from the server); a higher `rate_hz` repeats
the latest.

## The packet (protocol 1)

216 bytes, little-endian, no padding. `f32` is an IEEE float, `i32` / `u32`
4-byte integers. **The layout is a public API**: a field is never moved,
retyped or dropped; new fields go at the end, raise the version, and grow
`size`. A reader checks `magic`, reads what it knows and skips the rest
up to `size`.

| Offset | Type | Field | Unit, meaning |
|---|---|---|---|
| 0 | u32 | `magic` | `APXT` (0x54585041) |
| 4 | u16 | `version` | 1 |
| 6 | u16 | `size` | bytes in this packet (216) |
| 8 | u32 | `sequence` | counts up from 0 per run; a gap is a lost packet |
| 12 | u32 | `flags` | bits below |
| 16 | f32 | `time_s` | seconds since the game started sending |
| 20 | i32 | `game_mode` | 0 lobby, 1 sandbox, 2 countdown, 3 demo lap, 4 free practice, 5 replay, 6 qualifying, 7 race, 8 hotlap, 9 practice, 10 grid |
| 24 | f32 x3 | `position` | m, server frame: x and y level, z up |
| 36 | f32 x3 | `yaw`, `pitch`, `roll` | rad: yaw from +x toward +y; pitch nose up positive; roll leaning right positive |
| 48 | f32 x3 | `velocity` | m/s in the car's axes: x forward, y left, z up |
| 60 | f32 x3 | `accel` | m/s², car's axes, gravity left out: surge (+ speeding up), sway (+ pushed left: a left-hander's centripetal pull), heave (+ up) |
| 72 | f32 x3 | `roll_rate`, `pitch_rate`, `yaw_rate` | rad/s, the rates of the angles above |
| 84 | f32 | `speed` | m/s |
| 88 | f32 | `rpm` | |
| 92 | f32 | `redline_rpm` | 0 when the car's rev range is not known |
| 96 | f32 | `max_rpm` | the limiter, else the highest seen |
| 100 | f32 | `rev_fraction` | 0 to 1: the rev lights (the HUD's `car.rpm_fraction`) |
| 104 | i32 | `gear` | -1 reverse, 0 neutral, 1 first |
| 108 | f32 | `throttle` | 0 to 1 |
| 112 | f32 | `brake` | 0 to 1 |
| 116 | f32 | `steering` | -1 full left to 1 full right |
| 120 | f32 | `fuel_l` | litres; -1 unknown |
| 124 | f32 | `ers_charge` | 0 to 1; -1 for a car with no hybrid |
| 128 | f32 x4 | `tyre_temp_c` | tread, FL FR RL RR; -1 unknown |
| 144 | f32 x4 | `tyre_pressure_kpa` | FL FR RL RR; -1 unknown |
| 160 | f32 x4 | `tyre_wear_pct` | FL FR RL RR; -1 unknown |
| 176 | i32 | `place` | 1 for the lead; 0 unknown |
| 180 | i32 | `car_count` | |
| 184 | i32 | `lap` | the lap in progress |
| 188 | i32 | `lap_limit` | 0 for none (timed race, practice, hotlap) |
| 192 | f32 | `lap_time_s` | the lap in progress |
| 196 | f32 | `last_lap_s` | 0 for none |
| 200 | f32 | `best_lap_s` | 0 for none |
| 204 | f32 | `delta_s` | against the HUD's reference lap; 0 with none |
| 208 | f32 | `station_m` | metres round the lap from the line |
| 212 | f32 | `race_left_s` | a timed race's time left; -1 otherwise |

`flags`:

| Bit | Name | Set when |
|---|---|---|
| 0 | `CarPresent` | a car is reported; clear in the menus (then only the header and `game_mode` mean anything) |
| 1 | `Spectating` | the car is being watched, not driven |
| 2 | `Replay` | a saved replay or a timeshift plays |
| 3 | `OnTrack` | on the track's surface |
| 4 | `InPitLane` | |
| 5 | `InGarage` | parked in a qualifying or hotlap garage |
| 6 | `AbsActive` | ABS worked in the last feedback interval (driven car only) |
| 7 | `TcActive` | traction control cut drive in the last feedback interval (driven car only) |
| 8 | `DrsAllowed` | |
| 9 | `DrsOpen` | |
| 10 | `Headlights` | |
| 11 | `YellowFlag` | a yellow in the car's sector or the next |
| 12 | `BlueFlag` | |
| 13 | `LapInvalid` | the lap in progress is struck for track limits |
| 14 | `Finished` | the car has taken the flag |
| 15 | `MotionValid` | velocity, acceleration and rates are known (two frames of this car seen) |

## Where the values come from

Everything but motion and the ABS/TC bits is read from the HUD's data
points (`ApexTelemetryOutput::FillFromHud`, the names in
[hud-modding.md](hud-modding.md)), so the output and the HUD cannot
disagree; with the HUD hidden the subsystem has the data built anyway
(`UApexHudDataSubsystem::RefreshIfStale`). The car is the HUD's: the
player's, or the one being watched.

Motion (`FMotionEstimator`) is differentiated from the car's telemetry
positions and attitude, on the server's tick clock, so a late datagram does
not jolt a rig. The tick's length is learnt from how fast ticks arrive
over the first second and then held (speed scales with it, so a wobbling
estimate would read as surge); a sustained 5% difference learns it again. Acceleration and rates go through a one-pole
low pass at 8 Hz. A tick running back or a jump of more than 50 m (a
recovery, a car change) starts the motion again (`MotionValid` clear for a
frame). ABS and TC come from the latest `DriverFeedback` if it is under
250 ms old.

## Checking it

Tests: `ApexSim.TelemetryOutput.*`: `Layout` pins every offset above;
`FromHud` the data-point mapping; `Motion` the car's axes, a 100 m
left-hander at 30 m/s (9 m/s² of sway, 0.3 rad/s of yaw rate) through
jittered arrival, a 2 g stop and a teleport; `Settings` the
`telemetry_out` round trip. A listener for a look by hand:

```python
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 9996))
while True:
    b, _ = s.recvfrom(512)
    flags, = struct.unpack_from("<I", b, 12)
    surge, sway, heave = struct.unpack_from("<3f", b, 60)
    rpm, = struct.unpack_from("<f", b, 88); gear, = struct.unpack_from("<i", b, 104)
    print(f"{flags:#06x} rpm {rpm:6.0f} gear {gear} surge {surge:6.1f} sway {sway:6.1f}")
```

## Traps

- `launcher.exe` and `build_release.ps1`'s `settings.sample.yml` write
  `settings.yml` too: a new key goes in all three
  (`ApexBootSettingsIo::Serialise`, `launcher/main.cpp` `SaveSettings`, the
  sample).
- Not yet tried with SimHub, a motion rig or wheel LEDs: SimHub needs a
  game profile or a custom UDP plugin reading this layout
  ([verification.md](../verification.md)).
