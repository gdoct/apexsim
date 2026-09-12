#pragma once

#include "CoreMinimal.h"
#include "ApexDirectInputTypes.h"

struct FApexDriverFeedback;

/**
 * Force feedback, from the server's `DriverFeedback` to a device.
 *
 * The server works out what the driver should feel, because only it has the
 * tyre forces: the steering-column torque, each tyre's slip against its peak,
 * the surface under each wheel, suspension hits and contact. This file
 * reduces that to device-agnostic signals (FSignals) and mixes them for the
 * two kinds of device the client drives: a gamepad's two rumble motors
 * (MixGamepad) and a wheelbase's forces (MixWheel).
 *
 * The two mixers read the same signals and are otherwise unalike, because the
 * devices are: a pad can only shake, so a slide has to be *described* to it,
 * while a wheel can push, so the steering torque the server worked out is the
 * whole of the feel — the front tyres letting go is the rim going light —
 * and the road textures are detail on top of it.
 *
 * Everything here is pure maths over plain structs, so the feel is covered
 * by `ApexSim.Input.ForceFeedback.*` tests rather than by holding a pad.
 */
namespace ApexFfb
{
	/**
	 * What the car is doing, as the driver would feel it. Continuous values are
	 * 0..1 unless said otherwise and hold until the next message; the hits are
	 * nonzero only on the frame their message arrived, so each fires once.
	 */
	struct FSignals
	{
		/** False with no car or no recent feedback: the mixer lets everything decay to silence. */
		bool bActive = false;

		float SpeedMps = 0.0f;

		/** Gear from telemetry; a change is felt as a shift kick. */
		int32 Gear = 0;

		/**
		 * Newest steering-column torque in SCREEN sign (positive pushes the wheel
		 * right; the wire's sign is the other way round). 1 = the car's
		 * reference torque. A pad has nothing to show it with; a wheel's
		 * constant force is this.
		 */
		float SteerTorque = 0.0f;

		/** The front tyres past their grip (understeer): 0 gripping, 1 well past the peak. */
		float FrontSlide = 0.0f;

		/** The rear tyres past their grip (oversteer). */
		float RearSlide = 0.0f;

		/** A wheel locking under braking: ABS off, or overwhelmed. */
		float Lockup = 0.0f;

		/** A wheel spinning up under power. */
		float Wheelspin = 0.0f;

		/** ABS is holding a wheel at its peak slip. */
		bool bAbs = false;

		/** Traction control is cutting drive. */
		bool bTractionControl = false;

		/** Share of each side's wheels on a curb, 0, 0.5 or 1. */
		float CurbLeft = 0.0f;
		float CurbRight = 0.0f;

		/** Share of all four wheels past the curbs (grass, gravel). */
		float OffTrack = 0.0f;

		/** Hardest suspension hit since the last message, m/s; 0 on frames without one. */
		float BumpMps = 0.0f;

		/** Closing speed of a contact with another car since the last message, m/s; 0 on frames without one. */
		float ImpactMps = 0.0f;
	};

	/**
	 * Signals from one (merged) feedback message and the car's own telemetry.
	 * `bNewMessage` is true on the frame the message arrived; the hits in it
	 * are only reported then.
	 */
	APEXSIM_API FSignals MakeSignals(const FApexDriverFeedback& Feedback, float SpeedMps, int32 Gear, bool bNewMessage);

	/** A gamepad's two motors, 0..1: the heavy low-frequency one and the light buzzing one. */
	struct FRumble
	{
		float Low = 0.0f;
		float High = 0.0f;
	};

	/** Oscillator phases and decaying hits carried from frame to frame. */
	struct FGamepadState
	{
		/** Curb ribs pass under the tyre at a rate set by speed. */
		float CurbPhase = 0.0f;
		/** Seconds, wrapped at one: drives the ABS and traction-control pulse trains. */
		float PulseClock = 0.0f;
		/** Off-track roughness is noise resampled at a fixed rate. */
		float NoiseClock = 0.0f;
		float NoiseLevel = 0.0f;
		uint32 NoiseSeed = 0x9E3779B9u;
		/** Decaying heavy-motor hit: bumps and landings. */
		float Thump = 0.0f;
		/** Decaying hit from contact with another car, on both motors. */
		float Impact = 0.0f;
		/** Decaying kick from a gear change. */
		float ShiftKick = 0.0f;
		/** INDEX_NONE until a car has been seen. */
		int32 LastGear = INDEX_NONE;
	};

	/**
	 * Settings strength (0..1) as a gain on every effect. 0.5 plays the
	 * effects as designed, 1 doubles them (each saturating on its own), 0 is off.
	 */
	APEXSIM_API float GainFromStrength(float Strength01);

	/** One frame of rumble. `Gain` from GainFromStrength. */
	APEXSIM_API FRumble MixGamepad(const FSignals& Signals, FGamepadState& State, float DeltaSeconds, float Gain);

	/** A wheelbase's settings, 0..1 each (UApexSettingsSave's wheel block). */
	struct FWheelTuning
	{
		float Force = 0.5f;
		float RoadEffects = 0.5f;
		float Damping = 0.25f;
		/** The base turns the other way to what DirectInput's sign says. */
		bool bInvert = false;
	};

	/** The smoothed torque and the decaying hits, carried from frame to frame. */
	struct FWheelState
	{
		/**
		 * The torque the rim is being held at.
		 *
		 * Smoothed, because the samples arrive in one lump per telemetry frame:
		 * stepping the force 60 times a second is felt as grain on a direct
		 * drive base, where the same steps are invisible to a rumble motor.
		 */
		float Torque = 0.0f;

		/** Decaying hits: a landing, a collision, a gear change. */
		float Bump = 0.0f;
		float Impact = 0.0f;
		float ShiftKick = 0.0f;

		/** Off-track roughness, resampled at a fixed rate like the pad's. */
		float NoiseClock = 0.0f;
		float NoiseLevel = 0.0f;
		uint32 NoiseSeed = 0x9E3779B9u;

		/** INDEX_NONE until a car has been seen. */
		int32 LastGear = INDEX_NONE;
	};

	/**
	 * One frame of forces for a wheelbase.
	 *
	 * The torque is the signal; everything else is texture. A device plays one
	 * vibration at a time, so the loudest of the road's voices — curb ribs,
	 * grass, an ABS pulse train, a hit — takes the channel, which is also how
	 * it feels in a car: the loudest thing is what comes through the rim.
	 */
	APEXSIM_API FApexWheelEffects MixWheel(
		const FSignals& Signals, FWheelState& State, float DeltaSeconds, const FWheelTuning& Tuning);
}
