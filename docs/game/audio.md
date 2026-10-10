# Audio

Every sound in the client is synthesised in code: there are no sound assets in
the project. Three synthesisers exist: the menu and race cues, the engine (a
simulated crank and exhaust driven by telemetry), and the local car's tyres,
kerbs, road and wind (driven by the server's `DriverFeedback`). Each renders on
the audio thread through a procedural `USoundWave` at the device's own sample
rate.

## Code

All paths under `game-unreal/Source/ApexSim/`.

- `Public/Audio/ApexUiSound.h`, `Private/Audio/ApexUiSound.cpp`: `EApexUiSound`, `ApexUiSynth` (the cue renderer), `UApexUiSoundWave`.
- `Public/Audio/ApexUiAudioSubsystem.h`: `UApexUiAudioSubsystem` (game instance) and `ApexUiAudio::Play`.
- `Public/Audio/ApexEngineSound.h`: `ApexEngineSynth` (the engine, pure maths).
- `Public/Audio/ApexEngineSoundWave.h`: `UApexEngineSoundWave`, `FApexEngineLiveState`, `ApexEngineAudio::MakeSpec`.
- `Public/Audio/ApexListenerSpace.h`: `ApexSpace` (the driver's seat: shelf, bulkhead, reverb).
- `Public/Audio/ApexRoadSound.h`, `Public/Audio/ApexRoadSoundWave.h`: `ApexRoadSynth`, `UApexRoadSoundWave`.
- `Private/Audio/ApexAudioPreview.cpp`: the `apexsim.audio.RenderCars` console command.
- `Private/Race/ApexRaceCarActor.cpp`: each car's engine, own-car engine and road audio components.
- `Private/Race/ApexRaceDirector.cpp`: `UpdateCarAudio`, `UpdateRaceBleeps`, `ApplyAudioSettings`.

## Menu and race cues

`ApexUiSynth` renders each `EApexUiSound` (Move, Accept, Back, Denied, Adjust,
Notice, Error, CountdownTick, CountdownGo, LapLine) as a short tone from a note
table. `UApexUiSoundWave::CreateSoundGenerator` renders the cue when the mixer
asks for it. `UApexUiAudioSubsystem` plays cues with `PlaySound2D` as UI sounds,
scaled by `UApexSettingsSave::UiVolume` and throttled per cue
(`MinIntervalSeconds`). Widgets call `ApexUiAudio::Play(this, EApexUiSound::X)`.

Where cues come from:

- A button's `FApexButtonSpec::Sound` (Accept by default, Back on Back buttons).
- Back presses consumed by a navigable widget, the root widget's toasts (Notice /
  Error), settings sliders and dropdowns (Adjust).
- Race cues from `AApexRaceDirector::UpdateRaceBleeps`, on telemetry: a tick for
  each of the last five countdown seconds, a higher tone on green, a double pip
  when the local car completes a lap. The demo backdrop is silent.
- **Move is never raised by a widget.** `UApexRootWidget` listens to
  `FSlateApplication::OnFocusChanging` and plays Move only for focus the player
  moved: a change with cause `Navigation`, or one made inside an
  `ApexNav::FNavigationScope` (`UI/ApexNavigation.h`). The scope separates
  a host's `ApexNav::Focus` call on a key press from the same call when a screen
  opens. Scopes wrap `ApexNav::RouteFromLeaf`, the navigable widget's key/analog
  handlers and the input processor's focus recovery. Do not add per-widget Move
  cues.

## Engine synth

`ApexEngineSynth::Render` turns a crank at the telemetry's RPM. Each cylinder's
firing angle puts an exhaust stroke into one of two exhaust **banks**: a short,
steep **blow-down** (the upper harmonics) and the piston's long, eased-in
**swell** (most of the energy), both sized by throttle. Each bank is a weakly
reflecting quarter-wave header (a delay line reflected inverted). The banks meet
in a collector, then pass through the **silencer** (two poles, 400 Hz to 8 kHz
by `muffling`), a half-wave **tailpipe** resonance and the outlet's low-end
boom. A first-difference "rasp" on the blow-down survives only open pipes.

What gives it a four-stroke character, not a two-stroke one:

- **Fixed per-cylinder differences** (`CylinderTrait`: gain and timing per
  cylinder, the same every cycle). The pattern repeats every two revs, so it puts
  energy on the crank's half-orders under the note on every engine. Random
  firing-to-firing variation instead sounds like a two-stroke.
- **Bank geometry.** The note is the firing rate, `rpm/60 x cylinders/2`. A
  crossplane V8 fires its banks L R L L R R L R, which puts energy on the crank
  frequency and its odd halves (the burble); every other layout alternates banks.
- **Unequal pipes** (`SecondBankLevel`, `SecondBankLength`). Two identical pipes
  half a period apart sum back to an even pulse train, and every crank would
  sound alike.

On top: overrun pops (a lift above a third of the rev range opens a ~1 s window
where firings randomly become oversized pulses with a noise burst; a flat-out
upshift cracks once), the limiter's spark-cut stutter, induction roar, gearbox
whine at a per-gear tooth-mesh frequency (`WhineHz`: it steps up on an upshift
at the same revs), and a turbo's whistle and blow-off.

### The `[sound]` table

An engine is described by `[sound]` in its `car.toml`: `cylinders`,
`crossplane`, `turbo`, `exhaust_length_m`, `muffling`, `pops`, `gear_whine`,
`intake_roar` (validated in `Cars/ApexCarToml.cpp`; the server ignores the
table). The game reads it onto the catalog row as `FApexEngineSoundSpec
EngineSound`, together with `[engine]`'s idle, redline and limiter. The catalog
row is the only way the client knows the rev range: it is not on the wire.
`ApexEngineAudio::MakeSpec` fills a class default for a row with no
`[sound]` table: F1 a turbo V6, LMP and Hypercar a flat-plane V8, anything else
a crossplane V8. The keys are documented one by one in
[cars](../content/cars.md).

### Feeding it

`AApexRaceCarActor` owns a `UApexEngineSoundWave` on an attenuated audio
component. Every render frame it feeds the motion buffer's **blended** RPM (the
raw 60 Hz samples would zipper on a synthesised crank) with the newest throttle
and gear. The generator reads a lock-free `FApexEngineLiveState` on the audio
thread and picks up a changed engine spec by serial. The synth state (the
exhaust delay lines, about 20 KB) lives on the heap.

## The mix

- **Other cars** are mono points in the world: full level within 4 m, then
  `NaturalSound` falloff to silence at 150 m, with an air-absorption low-pass
  (20 kHz near, 1.5 kHz far). Scaled by the "Other cars" setting
  (`UApexSettingsSave::OtherCarsVolume`, default 0.5).
- **The player's own car** (the director's `FollowedCar`, unless TV view, the
  shot camera or a ghost replay) plays a second, stereo, unspatialised engine
  instead (`UApexEngineSoundWave::MakeOwnCar`, the actor's `OwnEngineAudio`,
  `AApexRaceCarActor::SetListenerSeat`). It runs the same synth through
  `ApexSpace`: a low shelf for weight, a 3.5 kHz bulkhead low-pass in a closed
  cabin, and a Freeverb-shaped stereo reverb sized per seat (`ApexSpace::ESeat`:
  cabin 0.35 s, open cockpit 0.5 s, chase/trackside 1.1 s). `AApexRaceDirector::UpdateCarAudio` sets the
  seat every frame.
- **Volumes**: the settings overlay's Audio tab (`-ApexSettingsTab=6` opens it
  headlessly). Master volume drives the audio device's transient primary volume
  (`UApexSettingsSubsystem::ApplyAudio`). Menu-sound volume is read at play time.
  "Engines", "Other cars" and "Tyres and road" (`EngineVolume`,
  `OtherCarsVolume`, `RoadVolume`) go through
  `AApexRaceDirector::ApplyAudioSettings` to `AApexRaceCarActor::SetMixVolumes`,
  multiplied with the demo's `SetEngineVolume` scale.

## Tyres, kerbs, road and wind

`ApexRoadSynth` / `UApexRoadSoundWave` play for the **local car only**, on its
unspatialised `RoadAudio` component, because they come from the server's
`DriverFeedback` (see [input and force feedback](input-and-ffb.md)). The race
director reduces the feedback with `ApexFfb::MakeSignals`, so kerbs, grass and
hits are the same signals the pad and wheel get.

- **The tyres use their own thresholds**, from the raw per-wheel slip
  (`ApexRoadSynth::SquealFromSlipAngle`, `LockupFromSlipRatio`,
  `WheelspinFromSlipRatio`). They are silent up to and at the grip peak, because
  a car cornering well sits at its peak slip angle all the time. The howl starts
  at 1.25x the peak slip angle and is full at 2.4x. Lockup and wheelspin start at
  1.6x the peak slip ratio (ABS/TC hold 1.0). Do not drive the squeal from the
  force feedback's slide signal: that starts at 0.9x as a hint, and the car would
  screech at every turn of the wheel.
- Slide levels rise over 0.14 s, because feedback is peak-held between messages
  and a single tick over a bump would otherwise chirp.
- A howl is a **tone** (fundamental about 600 Hz rear and 760 Hz front, plus
  three falling harmonics, with a small pitch wander and level flutter) over a
  band of low-mid scrub. It is silent at a crawl and on grass, and mostly scrub
  in the wet. No voice is white noise gated by a level.
- A kerb is a rib every 0.9 m (the force feedback's spacing) thudding through a
  resonance, so its pitch follows the car's speed. Off-track is rumble and
  stones. A suspension hit over the 0.3 m/s threshold is a thud, a contact a
  crunch, each handed to the audio thread by atomic exchange so it plays once.
  Rolling noise and wind grow with speed. A flat spot thumps once a wheel turn.
- The road sound stops in the hotlap garage, during a ghost replay and when the
  feedback is more than 0.5 s stale.

## Checking it

- `apexsim.audio.RenderCars [dir]` drives every catalog car through a scripted
  run and writes `Saved/Audio/<folder>.wav` (mono, as the world hears it),
  `<folder>_own.wav` (stereo, from the seat) and `road.wav` (each road voice in
  turn). Use it to judge or tune a `[sound]` table.
- Automation tests (run as in [building](../building.md)):
  - `ApexSim.UI.SoundCuesRender` / `.SoundCuesRateIndependent`: every cue short,
    finite, click-free and the same length at 44.1 and 48 kHz.
  - `ApexSim.Audio.Engine*` (Crank, Note, Timbre, Pops, GearWhine, Spec, Render):
    energy on the firing note at both rates, half-orders only on a crossplane, a
    GT3 under 2 kHz against an F1 above, pops, limiter stutter.
  - `ApexSim.Audio.Space*` (Seats, Reverb): the shelf, the bulkhead, ring times at
    both device rates, a stereo tail.
  - `ApexSim.Audio.Road*` (Squeal, Curb, Hits, FlatSpot, Render): squeal notes,
    the kerb's rib rate, hit thresholds.
  - `ApexSim.Cars.TomlSound`: the `[sound]` scan.
- `ApexEngineSound`, `ApexRoadSound` and `ApexListenerSpace` include only
  `CoreMinimal.h` and use only `FMath`. That keeps them buildable outside the
  engine against a small shim header (not checked in) for fast tuning with a WAV
  writer and spectra, with no editor build per iteration. Keep them free of
  other engine dependencies.

## Traps

- The pulse attack must stay about two samples (`PulseAttackSeconds`). An attack
  of 0.25 ms acts as a 640 Hz low-pass on the whole engine.
- The rasp is taken from the pulse alone. A first difference applied after the
  turbulence noise is only hiss.
- The own-car reverb send is high-passed at 250 Hz (`ReverbSendHighpassHz`).
  Fed the low orders, the combs become room modes and cancel the firing note
  against the direct sound, removing the low shelf.
- Feed the engine the motion buffer's blended RPM, never the raw telemetry
  samples.
- Judge synth changes by measurement (the tests, spectra of the rendered WAVs)
  and by ear on the `RenderCars` files, not in a race.
