#pragma once

#include "CoreMinimal.h"

/**
 * A car engine, synthesised by simulating what the exhaust hears.
 *
 * Not a tone at the firing frequency: the sound is built the way the engine
 * makes it. A crank turns at the telemetry's RPM; every time it passes a
 * cylinder's firing angle that cylinder's blow-down puts a pressure pulse —
 * sized by the load, roughened by combustion — into one of two exhaust
 * banks, and each bank is a resonant pipe (a waveguide with the muffler's
 * loss in its loop) that rings with the pulses it is fed. What comes out is
 * decided by the engine's geometry rather than by a waveform picked per car:
 *
 *  - The note is the firing rate, RPM/60 × cylinders/2 (four-stroke), so the
 *    pitch is proportional to RPM and an upshift is heard as the interval
 *    the revs fell by. Nothing saturates part way up a gear.
 *  - Which bank each firing goes into is what separates a crossplane V8 from
 *    a flatplane one at the same RPM: a crossplane's banks get an uneven
 *    90-180-270-180 pattern, so each pipe carries a rhythm that repeats
 *    every two revs — half-orders of the crank under the firing note, the
 *    low burble — while a flatplane, a V6, a V10 or a flat-six alternate
 *    banks evenly and each pipe carries a clean multiple of the crank speed,
 *    which is the scream. The two pipes are heard unequally (one is nearer),
 *    or the banks would sum back to an even pulse train and no crank could
 *    be told from another.
 *  - Load (throttle) sets the pulse size and the combustion noise; a closed
 *    throttle at speed is the overrun, where unburnt fuel lighting in the
 *    pipe is a pop — a random oversized pulse and a burst of noise — for a
 *    second or so after the lift, as often as the car's `pops` says.
 *  - The pipe's length sets where it resonates, its muffling how much of the
 *    pulse's edge survives, and on top of the exhaust there is the intake's
 *    roar (throttle-gated noise through a resonance), a race gearbox's
 *    straight-cut whine (a tooth-mesh tone whose count changes per gear), a
 *    turbo's spool whistle and blow-off, and the rev limiter's stutter.
 *
 * RPM, gear and throttle change every telemetry frame (60Hz) while the audio
 * device pulls samples far more often, so rendering happens continuously,
 * one buffer at a time, with the crank angle, the pipes and the smoothed
 * inputs carried in FState between calls. Everything here is plain maths
 * over plain structs: no engine calls beyond FMath, so the same code can be
 * rendered offline (`apexsim.audio.RenderCars` writes every catalog car's
 * drive to Saved/Audio as a WAV; see ApexAudioPreview.cpp).
 */
namespace ApexEngineSynth
{
	/** The pipes are sized for the longest exhaust at the highest device rate (3 m at 96 kHz). */
	constexpr int32 PipeCapacity = 2048;
	constexpr int32 TailCapacity = 1024;

	/**
	 * An engine as the exhaust hears it: the `[sound]` table in car.toml,
	 * carried on the catalog row (FApexEngineSoundSpec) and reduced to plain
	 * numbers here. Every figure has a default that makes a plausible race V8,
	 * so a car with no `[sound]` table still runs.
	 */
	struct FEngineSpec
	{
		/** Four-stroke cylinders; firing intervals are 720°/Cylinders. */
		int32 Cylinders = 8;

		/**
		 * Which exhaust bank each firing, in firing order, blows into: bit i
		 * set puts the i-th firing in the right bank. A crossplane 90° V8 is
		 * L R L L R R L R; every other layout alternates. See BankMask().
		 */
		uint32 BankMask = 0xB2u;

		float IdleRpm = 900.0f;
		float RedlineRpm = 8000.0f;
		/** Where the limiter's cut begins; the redline when the car.toml has none. */
		float LimiterRpm = 8100.0f;

		/** Primary pipe length, metres: sets the exhaust's resonance (quarter-wave). */
		float ExhaustLengthM = 1.4f;
		/** 0 open pipes, 1 a road muffler: loss in the pipe and the top end of what leaves it. */
		float Muffling = 0.3f;
		/** How readily the overrun pops and a full-throttle upshift cracks, 0..1. */
		float Pops = 0.6f;
		/** Straight-cut gearbox whine, 0..1. */
		float GearWhine = 0.3f;
		/** Induction roar under throttle, 0..1. */
		float IntakeRoar = 0.5f;
		/** A turbocharger's whistle, spool and blow-off. */
		bool bTurbo = false;
	};

	/** The bank pattern for an engine: crossplane V8s are the odd one out; everything else alternates. */
	APEXSIM_API uint32 BankMask(int32 Cylinders, bool bCrossplane);

	/**
	 * The firing fundamental in Hz — the note the ear takes from the engine.
	 * Proportional to RPM; the exhaust's resonance colours it but never moves it.
	 */
	APEXSIM_API float NoteHz(const FEngineSpec& Spec, float Rpm);

	/**
	 * The gearbox whine's mesh frequency in Hz at this RPM and gear. Each gear
	 * pair has its own tooth count, so the whine steps *up* on an upshift at
	 * the same revs — the cue a driver hears before the revs fall.
	 */
	APEXSIM_API float WhineHz(float Rpm, int32 Gear);

	/** What the telemetry says the engine is doing. */
	struct FInputs
	{
		float Rpm = 0.0f;
		float Throttle = 0.0f;
		/** -1 reverse, 0 neutral, 1.. forward. */
		int32 Gear = 0;
	};

	/** One exhaust bank: its blow-down envelope and the pipe it feeds. */
	struct FBankState
	{
		/** The blow-downs so far: steep, short, decaying between firings. */
		float Envelope = 0.0f;
		/** The pistons' pushes: the same charges again, eased in and long. */
		float Swell = 0.0f;
		float SwellPending = 0.0f;
		/** Pending pulse energy, let into the envelope over a few samples so a firing is not a click. */
		float Pending = 0.0f;
		/** Combustion roughness, low-passed so it rides the pulse rather than hissing. */
		float Roughness = 0.0f;
		/** A pop lighting in this pipe: a noise burst that decays over ~15 ms. */
		float PopBurst = 0.0f;
		float Pipe[PipeCapacity] = {};
		int32 Write = 0;
		float LoopLowpass = 0.0f;
		/** Gas-flow hiss, band-limited, riding the pulse on open pipes. */
		float Hiss = 0.0f;
		/** The previous sample's pulse: the rasp is its first difference. */
		float LastBody = 0.0f;
	};

	/** Running state between render calls. Default-constructed is silent and unprimed. */
	struct FState
	{
		bool bPrimed = false;
		float SmoothedRpm = 0.0f;
		float SmoothedThrottle = 0.0f;

		/** Crank angle within the 720° four-stroke cycle. */
		float CrankDeg = 0.0f;
		/** Index in firing order of the next cylinder to fire. */
		int32 NextFiring = 0;

		FBankState Banks[2];
		uint32 NoiseState = 0;

		/** Two-pole resonance the intake noise is pushed through. */
		float IntakeY1 = 0.0f;
		float IntakeY2 = 0.0f;
		float IntakeNoise = 0.0f;

		float WhinePhase = 0.0f;
		float SmoothedWhineHz = 0.0f;

		/** 0..1, follows load with a spool lag; drives the whistle and the blow-off. */
		float TurboSpeed = 0.0f;
		float TurboPhase = 0.0f;
		float BlowOff = 0.0f;
		float BlowOffHighpass = 0.0f;

		/** Pop likelihood after a throttle lift, decaying; see the Pops field. */
		float PopBudget = 0.0f;
		/** The most throttle seen lately, decaying: a lift is this dropping to nothing. */
		float ThrottlePeak = 0.0f;
		int32 LastGear = 0;
		/** Seconds into the limiter's cut/fire cycle. */
		float LimiterClock = 0.0f;

		/** After the collector: the silencer's two poles, the tailpipe, and the outlet's low end. */
		float MufflerLowpass = 0.0f;
		float MufflerLowpass2 = 0.0f;
		float Tail[TailCapacity] = {};
		int32 TailWrite = 0;
		float TailLowpass = 0.0f;
		float BoomLowpass = 0.0f;
		/** A DC blocker: a one-sided pulse train has a mean. */
		float DcX1 = 0.0f;
		float DcY1 = 0.0f;
	};

	/**
	 * Renders NumFrames of mono float samples into OutFrames, advancing State
	 * towards Inputs as it goes. Never exceeds ±1 (soft-clipped, so a pop
	 * saturates rather than clicks). An RPM under ~100 is a dead engine: silence.
	 */
	APEXSIM_API void Render(FState& State, const FEngineSpec& Spec, const FInputs& Inputs,
		float SampleRate, float* OutFrames, int32 NumFrames);
}
