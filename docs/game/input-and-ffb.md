# Input and force feedback

How the player drives: Enhanced Input actions built in C++, DirectInput wheels,
pedals and button boxes as ordinary `FKey`s, and force feedback that the server
computes from the tyre forces and the client mixes for a pad's rumble motors or
a wheelbase's motor.

## Code

Client paths under `game-unreal/Source/`.

- `ApexSim/Public/Input/ApexInputConfig.h`: `ApexInput` (actions, slot table,
  binding rules, steering lock gain), `UApexInputConfig`, the input modifiers.
- `ApexSim/Public/ApexPlayerController.h`: owns the input config, the drive
  handlers and `UpdateForceFeedback`.
- `ApexSim/Public/UI/ApexMenuInputProcessor.h`: focus recovery while driving.
- `ApexSim/Public/Input/ApexForceFeedback.h`: `ApexFfb`, signals and the two
  mixers (pure maths).
- `ApexSimInput/`: the DirectInput device module (`ApexDirectInputTypes.h`,
  `Windows/ApexDirectInputDevice.cpp`, `ApexSimInputModule.cpp`).
- `server/src/feedback.rs` (`FeedbackTick`, `FeedbackAccumulator`,
  `DriverFeedback`), `server/src/physics.rs` (`steering_column_torque`,
  `steering_column_stiffness`, `impact_steer_kick`),
  `server/src/game_loop/broadcast.rs` (sends it).

## Actions and bindings

The driving controls are Enhanced Input actions with the mapping context built
in C++ (`UApexInputConfig::Create`) rather than as `.uasset`s, so bindings are
readable in a diff. `AApexPlayerController` adds the context (`DriveContext`)
only while a race is running and removes it on the way out.

`ApexInput::Slots()` is the single table of bindable slots. A slot is one key on
one action in one column (Gamepad, Keyboard, Wheel). Slot numbers and action ids
are stable because they are written into the settings save. Slots 4-6
(`Slot::Wheel`, `WheelLow`, `WheelHigh`) are the wheel column. Adding a control
is one entry in that table.

| Action | Kind | Keyboard | Pad | Goes to |
|---|---|---|---|---|
| `Throttle` / `Brake` | axis 0..1 | W / S (and the arrows) | RT / LT | `PlayerInput` |
| `Steer` | axis, +1 right | A / D (and the arrows) | left stick X | `PlayerInput` |
| `GearUp` / `GearDown` | press | E / Q | RB / LB | `PlayerInput` |
| `ToggleCamera` | press | C | Y | local |
| `Look` / `LookBack` | axis / held | `,` `.` / B | right stick X / RS | local |
| `Drs` | held | Left Shift | X | `PlayerInput.drs` |
| `Headlights` | press | L | D-pad up | `PlayerInput.headlights` |
| `FlashLights` | held | H | D-pad down | `PlayerInput.flash` |
| `ErsMode` | press | M | D-pad right | `PlayerInput.ers_mode` |
| `ErsBoost` | held | Space | A | `PlayerInput.ers_boost` |
| `PauseMenu` | press | Escape | Start | local |

Every action also has a wheel slot, unbound by default (unbound steering falls
back to the first wheelbase's axis, `ApexInput::GetWheelDefaultKey`).

- `PauseMenu` and the wheel-only `MenuUp/Down/Left/Right/Accept/Back` are not
  Enhanced Input actions. The root widget tests the pause key directly, and the
  Menu* wheel bindings become Slate navigation rules
  (`ApexInput::ApplyMenuNavigation`, applied by `UApexSettingsSubsystem`), so a
  wheel's hat and buttons navigate the menus like the D-pad and A/B.
- Steering is in screen sign (+1 right). The server's frame has positive
  steering to the left; the race director sends `-Steer`. Keep that flip at the
  network boundary.
- The headlight switch (`AApexRaceDirector::HeadlightSwitch`) is -1 until the
  first press, leaving the lights to the sky's rule; the first press flips what
  the server shows. A flash tap is held at least 0.2 s
  (`AApexPlayerController::IsFlashingLights`) so other drivers see it.
- `ErsMode` steps Balanced, Attack, Harvest (`ApexErs::NextMode`) from what the
  server last reported (`LocalErsMode`, `ErsModeSwitch`) and plays the Adjust
  cue. What the server does with DRS and ERS is in
  [vehicle physics](../server/vehicle-physics.md).
- Gears: the race director sets `PlayerInput.gear` to the current gear plus the
  shift presses since the last send (`ConsumeGearDelta`). There is no handbrake
  action and no H-pattern shifter support.

Bindings are edited on the settings overlay's Controls tab (pad and keyboard)
and Wheel tab (devices, forces, the wheel's slots, live pedal and steering
meters).

### Modifiers

Device-specific shaping lives on the mapping, never in the handler, which cannot
tell which device a value came from: `UApexInputModifierPedal` folds a
DirectInput axis's -1..1 into 0..1 (a trigger and a key feeding the same action
are 0..1 already), `UApexInputModifierPadSteering` applies the pad's deadzone and
curve (a wheel must not get them), `UApexInputModifierWheelSteering` the steering
lock gain. All three read the settings live, so a slider needs no context
rebuild. `UApexInputConfig::ApplyBindings` refills the existing context object
rather than replacing it, because the Enhanced Input subsystem already holds it.

## DirectInput wheels and pedals

XInput is the engine's pad path and knows nothing else; wheelbases, pedal sets,
shifters and button boxes speak DirectInput. `ApexSimInput` is an input-device
plugin like the engine's XInput one: `FApexDirectInputDevice` is created on the
platform application's first poll, ticked every frame and told about
`WM_DEVICECHANGE` (debounced), so a device plugged in mid-session works. XInput
devices (HID path containing `IG_`) are skipped, or an Xbox pad would arrive
twice with its triggers merged.

**Keys and slots.** Every control is an ordinary `FKey` (`DInput1_X`,
`DInput2_Button7`, `DInput1_Hat1Up`; `ApexDirectInput::MakeKey` / `ParseKey`),
registered with `EKeys` at module startup, so Enhanced Input, the rebinding
screen and the settings save need no second input path. The number is a device
**slot**, not an enumeration index: a slot belongs to one physical device by
instance GUID (falling back to the product GUID when the device moves USB port)
and is remembered in `Saved/ApexInputDevices.json`, so plugging in a button box
cannot shift the pedals' bindings onto it.

**Reading devices.** Axes are sent when they move and every frame they rest away
from zero: a pedal rests at -1, and `FlushPressedKeys` at the end of a race
clears the key state, so the resting value has to keep arriving. A lost device
sends its axes back to where they were when it arrived, not to zero (zero is
half throttle to a pedal binding); it is asked to reacquire twice a second and
reopened only after 2 s of failed reads.

**Per-device bindings** (`ApexInput::FindBinding` / `StoreBinding`). The wheel
column keeps one binding per device: rebinding with one base attached leaves
another base's mapping alone, and unbinding only unbinds the device in front of
the player. Binding an axis reads how far it moves, not where it sits, and the
direction of the move sets `FApexKeyBinding::bInvert`, which is how a pedal that
rests at the top of its travel configures itself. Forces go to the device the
steering is bound to (`ApexInput::FindForceFeedbackDevice`).

### Steering lock

The Wheel tab's "Wheel rotation" (what the base's own driver is set to,
`WheelRotationDeg`, default 900: DirectInput only reports where the rim is
between its ends) and "Steering lock" (rim degrees lock to lock for the car's
full lock, `WheelSteeringLockDeg`, default 480) make a gain,
`ApexInput::WheelSteeringScale` (rotation / lock, never under 1), applied by
`UApexInputModifierWheelSteering`.

The default lock is **Auto** (`UApexSettingsSave::bWheelSteeringLockAuto`): twice
the driven car's `[cockpit] wheel_lock_deg`. `AApexRaceDirector::UpdateSteeringLock`
pushes it every frame through `UApexSettingsSubsystem::SetCarSteeringLock`;
`GetWheelSteeringLockDeg` is the lock in use. The same function hands the cockpit
rig the lock (`AApexCockpitRig::SetDriverRimLockDeg`) while a wheel steers the
player's own car, so the rim on screen turns as far as the one in the player's
hands.

## Force feedback: the server side

The server computes what the driver should feel, because only it has the tyre
forces. Every tick `update_car_3d` records a `FeedbackTick` into
`CarState::feedback` (a `FeedbackAccumulator`): steering-column torque, the
input it was worked out at and its slope, front axle load, slip per wheel as a
multiple of the tyre's peak, the surface under each wheel (`ContactSurface`
from `RoadContact::feedback`: road, curb or off; tarmac run-off and the pit lane
read as road), suspension speed, ABS/TC activity, contact closing speed and flat
spots.

On each telemetry tick the game loop drains it (`FeedbackAccumulator::take`)
into a `DriverFeedback` for the car's human driver: every torque sample kept,
the transients peak-held. It goes **over UDP only**, with no TCP fallback,
because a force that arrives late is worse than none. Fields are positional and
only ever appended (`steer_kick`, `steer_input`, `steer_stiffness`,
`front_load`, `flat_spot`), so an older client skips what it does not know. See
[protocol](../server/protocol.md). Golden bytes: `cargo test
driver_feedback_wire_format -- --nocapture` -> `ApexUdpGolden::S_DriverFeedback`
(an older server's frame) and `S_DriverFeedbackFlatSpot`.

### The steering torque

`physics::steering_column_torque` (1.0 = the front axle at its static grip
limit; positive turns the wheel left) is summed per front tyre from that tyre's
own forces and load:

- **Aligning**: `-Fy x (pneumatic + caster trail)`. The pneumatic trail grows
  with the square root of the tyre's load and shrinks to zero at 1.4x the peak
  slip angle (`PNEUMATIC_TRAIL_ZERO_AT_SLIP`), slightly negative past it. Caster
  (`MECHANICAL_TRAIL_SHARE`) never shrinks. The rim crests at about half the peak
  slip and goes clearly light by the grip peak: that lightening is the
  understeer cue. Load, braking and downforce make it heavier.
- **Scrub** (a front tyre braking harder than its partner tugs the rim toward
  it) and **jacking** (the axle's weight centres a steered wheel).
- **Hits** add `steer_kick` (`physics::impact_steer_kick`, from the velocity
  change a contact gave the front axle), toward the side that was hit.

### Latency correction

The torque is a network round trip old; on its own it is a spring that answers
late, so a rim let go of in a corner holds still, then jumps, then rings. The
server therefore also sends `steer_input` (the input the newest sample was
worked out at) and `steer_stiffness` (`physics::steering_column_stiffness`:
the torque's slope per unit of input there, from each front tyre's lateral force
re-solved at the Ackermann angles either side, times the steering aid's own
slope when the aid is on). The client adds `slope x (input now - steer_input)`
to the torque (`FWheelState::Correction` in `ApexFfb::MixWheel`), so the rim
answers its own movement at once and only the car's motion arrives late.

- Only a slope that pushes back is used: past the aligning crest a positive slope
  would feed itself.
- The correction is capped at 0.15 of input (`MaxCorrectionInput`) and faded in
  from 3 to 12 m/s: at a crawl the car turns with its wheels, so the slope is a
  spring that is gone a moment later.
- `front_load` (front axle load over static) roughly doubles the slope under hard
  braking in a downforce car; that is what braking feels like through the rim.

## Force feedback: the client side

`UApexNetSubsystem` merges every feedback message since its last tick
(`FApexDriverFeedback::Absorb`). `ApexFfb::MakeSignals` turns it into
device-agnostic `FSignals` (screen sign: positive pushes the rim right), which
two mixers read. `AApexPlayerController::UpdateForceFeedback` runs both every
frame: it is the one hook that ticks with no pawn, no race and a pause menu up.
The caller fills in what the device knows (`RimDegrees`, `LocalSteer`) and the
station for the road texture.

### Gamepad (`ApexFfb::MixGamepad`)

A pad can only shake, so a slide has to be described: front slide on the light
motor, rear slide on the heavy one (understeer and oversteer never feel alike),
kerb ribs at a speed-set rate, ABS/TC pulse trains, grass noise, and decaying
thumps for bumps, contact, shifts and flat spots. It assumes XInput's channel
layout (under the GameInput plugin the Small channels are the trigger motors).
Strength: the Controls tab's "Pad vibration" (`UApexSettingsSave::Vibration`)
through `ApexFfb::GainFromStrength`; 0.5 plays the effects as designed.

### Wheelbase (`ApexFfb::MixWheel`)

The torque is the feel: the rim going light is the front tyres letting go.
Everything else is texture on top.

- **Scale**: the car's reference torque is `WheelTorqueReference` (0.6) of the
  base's peak at Force 50%, soft-limited past 0.8 (`WheelSoftKnee`) so a car
  loaded past its reference still feels stronger rather than clipping.
- **Stiffness limit**: `MaxRimStiffnessPerDeg` (0.025 of the base per rim degree,
  using `FWheelTuning::RimDegreesPerInput`) scales the whole torque down where the
  tyres' slope would make the rim stiffer than the delayed loop can hold. Above it
  a rim let go of at speed oscillates indefinitely whatever the damper. Corners
  keep their weight, because the slope is small near the grip limit.
- **Smoothing**: the torque passes through a 12 ms pole
  (`WheelTorqueSmoothingSeconds`), because samples arrive in 60 Hz lumps and a
  direct-drive base feels the steps as grain. A hit's `SteerKick` is added
  unsmoothed and decays over 70 ms.
- **Road texture** (`RoadTug`): the physics road is smooth, so the client lays
  value noise (0.45, 1.6 and 6 m wavelengths) along the lap, read at the
  telemetry's station run on by speed, so a bump is in the same place every lap.
  It rides on the constant force after the soft limit, scaled by Road effects,
  speed, `front_load^0.7` and 3x off track.
- **Vibration channel**: a device plays one vibration at a time, so the loudest
  of kerbs, grass, ABS, lockup, a hit, braking grain (62 Hz, the fronts' braking
  slip from half to all of the peak, `FrontBrakeSlip`), front scrub (55 Hz) or a
  flat spot (a shake at the wheel's turning rate) takes it.
- **Damper**: heaviest at a standstill, 35% of the setting at speed, and while
  driving never under 0.3 of the force (`MinDriveDamper`): a belt-driven rim has
  almost no friction and overshoots the centre without it. A centring spring is
  used only in the menus.
- **Soft lock**: past half the steering lock either way the rim meets a soft stop
  (0.9 of the base over 6 degrees, with a damper), from the rim angle read off the
  device (`ApexInput::ReadWheelSteering`, `FSignals::RimDegrees`).
- **Centring**: when a car arrives standing still (a session start, leaving the
  hotlap garage) a position loop on the constant force, damped by the rim's speed,
  brings the rim to the middle and lets go once it has settled, the car rolls or
  3 s pass (`ApexFfb::RequestCentre`). DirectInput's own spring cannot: its force
  is a share of the base's whole travel, a few percent for a rim a quarter turn off.
- **Settings**: the Wheel tab's Force, Road effects, Damping and Direction
  (`UApexSettingsSave::WheelForce`, `WheelRoadEffects`, `WheelDamping`,
  `bWheelInvertForce`; the Direction test pushes the rim right and asks).

### Sending forces safely

Devices are opened shared. Only the wheel the steering is bound to is taken
exclusively, the first frame a race asks for forces
(`FApexDirectInputDevice::AcquireForForces`), so an open editor and a launched
game can share a wheelbase. The centring and gain properties are only touched
once exclusive access is granted. Effects are a constant force, a sine, a damper
and a spring, each updated only when it changes.

Wheel drivers are treated as fragile (a driver has hung, needing a reboot, when
every effect was sent every rendered frame):

- The constant force is sent at most 250 times a second, the sine, damper and
  spring at most 30 (`ConstantMinIntervalSeconds`,
  `SlowEffectMinIntervalSeconds`); starting and stopping never wait.
- A refused update is logged once and backs off 0.5 s; twenty refused rounds
  hand the wheel back.

Three independent mechanisms stop a wheel pulling when the game does not:

1. A watchdog on the game thread drops every force after 0.3 s without an update
   (`EffectsWatchdogSeconds`: a hitch).
2. The constant force is created with a 0.5 s duration **on the device** and
   restarted every 0.15 s while it plays (`ApexDirectInput::PlanConstantSend`,
   `ConstantForceLifeSeconds`, `ConstantForceRenewSeconds`), so a hung game
   thread, a crash or a killed process stops it in the base itself. The damper and
   spring only resist the rim and the sine is zero-mean, so they stay infinite.
3. On a crash, `FCoreDelegates::OnHandleSystemError` asks a dedicated thread to
   send `DISFFC_STOPALL`, waiting at most 250 ms so a DirectInput lock held by
   the crashed thread cannot hang the crash handler.

A NaN is never a force: `FMath::Clamp` returns its upper bound for NaN, so
`MixWheel` cleans its inputs and resets a poisoned state, and
`ApexDirectInput::SanitiseEffects` zeroes anything non-finite before the
hardware.

## Checking it

- Automation tests (run as in [building](../building.md)):
  `ApexSim.Input.ForceFeedback.*` (both mixers; `WheelLetGo` lets go of a rim in
  a corner against a 40 ms, 60 Hz server and expects it home inside 0.3 s),
  `ApexSim.Input.DirectInput.*` (slots, keys, readings, records file,
  `ConstantSchedule`, `Sanitise`), `ApexSim.Input.Wheel.*` (binding rules,
  mapping, menu navigation), `ApexSim.Net.Udp.*` (feedback goldens),
  `ApexSim.Net.Ers.Modes`.
- Server: `column_stiffness_predicts_the_torque_after_the_rim_moves`
  (`physics.rs`); `cargo test --release --test grip_probe_test
  steering_feel_probe -- --ignored --nocapture` (`PROBE_CAR=<folder>`) prints
  torque against lateral g and front slip while a car winds on lock, the harness
  for tuning the torque.
- Console: `apexsim.input.Devices` (slots, capabilities, HID paths, live
  readings), `apexsim.input.Rescan`, `apexsim.ffb.Debug 1` (signals, motor
  levels, wheel forces on screen).
- Log: a `Wheel forces over 15 s driving:` line (torque in, constant force out,
  time at the base's limit, correction, road, vibration, settings). Read it
  against a "feels weak" report before changing any gain.

## Traps

- **Input mode.** The menu shell runs in `FInputModeUIOnly`, where the viewport
  discards game input and no binding fires. The controller switches to
  `FInputModeGameAndUI` for the race.
- **Focus while driving.** Game-and-UI does not move focus by itself, so the
  controller names the viewport as the widget to focus and
  `FApexMenuInputProcessor` puts focus back on it whenever an event arrives while
  driving. Focus left in the shell routes events through the focusable
  `WBP_Root`, whose default Slate handler eats the left stick, D-pad and arrows
  as menu navigation: pad steering dies while throttle and shoulders still work.
- **Game mode override.** A Blueprint game mode can serialise its own
  `PlayerControllerClass` and silently win; `AApexMenuGameModeBase` logs an
  error when it does.
- **DirectInput force direction.** DirectInput gives a force's direction as where
  it comes from: a positive constant force on the steering axis pushes the rim
  toward negative X (left). `FApexDirectInputDevice::ApplyEffects` therefore sends
  `-Constant` to turn the mixer's "positive = right" into the device convention.
  Sent unflipped, self-centring pushes the rim away from centre. The Direction
  toggle is only for a driver that breaks the convention.
- **Fanatec firmware.** After a Fanatec driver-package update the base's firmware
  must be updated in the Fanatec App's firmware manager. Until then every
  DirectInput call succeeds and the motor plays nothing, while the Fanatec app's
  own FFB test works. Rule out both hardware traps with a standalone DirectInput
  probe (play a constant force, read the rim; the first `GetDeviceState` after
  `Acquire` reads 0, so let it settle) before tuning the mixer.
- **COL01 / COL02.** One base can be two DirectInput devices: Fanatec's driver
  shows a ClubSport V2.5 as HID collections COL01 and COL02, both claiming
  forces. Bind the steering on COL01; each device's HID path is in the log and
  in `apexsim.input.Devices`.
- **Never send effects every frame**, and keep the on-device constant-force
  duration and the crash stop (see "Sending forces safely").
