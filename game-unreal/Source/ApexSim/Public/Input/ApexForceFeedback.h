#pragma once

#include "CoreMinimal.h"

struct FApexDriverFeedback;

/**
 * Force feedback, from the server's `DriverFeedback` to a device.
 *
 * The server works out what the driver should feel, because only it has the
 * tyre forces: the steering-column torque, each tyre's slip against its peak,
 * the surface under each wheel, suspension hits and contact. This file
 * reduces that to device-agnostic signals (FSignals) and mixes them for the
 * one device the client drives today, a gamepad's two rumble motors.
 *
 * A racing wheel is a second mixer over the same signals. Its constant force
 * is SteerTorque (every physics tick's sample is on the wire, see
 * FApexDriverFeedback::SteerTorque), with the curb and slip textures as
 * periodic effects on top. Nothing on the server or the wire changes for
 * it: a wheel needs a device layer that reads its axes (DirectInput, or the
 * GameInput plugin) and plays effects on it.
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
}
