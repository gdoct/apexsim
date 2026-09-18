#pragma once

#include "CoreMinimal.h"

/**
 * What the tyres, the road and the air sound like from the driver's seat.
 *
 * The engine note comes from telemetry every car has; this comes from the
 * server's `DriverFeedback`, which only the car's own driver is sent, so it
 * plays for the local car alone. It reads the same device-agnostic signals
 * the force feedback does (ApexFfb::FSignals): what a pad is told as a buzz
 * and a wheel as a vibration, the ear is told here.
 *
 *  - **Squeal**, one voice per axle: noise through a narrow resonance, which
 *    is what a scrubbing tread block is — nearly a tone, never a steady one.
 *    It comes in as the axle passes its grip peak, rises in pitch with
 *    sliding speed, and the two axles sit a third apart so understeer and
 *    oversteer are told apart by ear. A locked wheel is lower and harsher; a
 *    spinning one climbs with the slip. On a wet road rubber hardly sings:
 *    the squeal gives way to hiss.
 *  - **Kerbs**: a rib passes under the tyre every 0.9 m (the spacing the force
 *    feedback uses), each one a thud through the tyre's own resonance plus a
 *    slap, so a kerb is a drone whose pitch is the car's speed. The two sides
 *    run out of phase; both wheels of a side on the kerb hit harder.
 *  - **Off the road**: a low rumble and the rattle of stones against the floor.
 *  - **Hits**: a suspension bump is a thud, contact a crunch. Both arrive as
 *    one-shots (Hit), not levels.
 *  - **Rolling and wind**: always there, growing with speed — what makes
 *    200 km/h sound unlike 80 with the engine at the same revs.
 *
 * Pure maths over plain structs, rendered a buffer at a time with its
 * filters carried in FState, like ApexEngineSynth.
 */
namespace ApexRoadSynth
{
	/** Levels that hold until the next update. 0..1 unless said otherwise. */
	struct FInputs
	{
		float SpeedMps = 0.0f;
		/** The axle past its grip peak: 0 gripping, 1 well past. */
		float FrontSlide = 0.0f;
		float RearSlide = 0.0f;
		/** A wheel locked under braking. */
		float Lockup = 0.0f;
		/** A driven wheel spinning up. */
		float Wheelspin = 0.0f;
		/** Share of each side's wheels on a kerb: 0, 0.5 or 1. */
		float CurbLeft = 0.0f;
		float CurbRight = 0.0f;
		/** Share of the wheels past the kerbs. */
		float OffTrack = 0.0f;
		/** Rain on the road: squeal turns to hiss, and the tyres throw spray. */
		bool bWet = false;
	};

	/** A two-pole resonance: the building block of every voice here. */
	struct FResonator
	{
		float Y1 = 0.0f;
		float Y2 = 0.0f;
	};

	struct FState
	{
		uint32 NoiseState = 0;

		/** Inputs eased towards their targets: a level stepping at 60Hz is a buzz. */
		FInputs Smoothed;

		FResonator FrontSqueal[2];
		FResonator RearSqueal[2];
		FResonator LockSkid;
		FResonator SpinChirp;

		/** Kerb ribs: a phase per side, and the tyre's thud they excite. */
		float CurbPhase[2] = {0.0f, 0.37f};
		FResonator CurbThud;
		float CurbSlap = 0.0f;

		float RumbleLowpass = 0.0f;
		FResonator Stones;

		/** Decaying one-shots; see Hit. */
		FResonator BumpThud;
		/** A hit waiting for the next rendered sample to strike the thud with. */
		float PendingThud = 0.0f;
		float CrunchEnvelope = 0.0f;
		float CrunchLowpass = 0.0f;

		float RollLowpass = 0.0f;
		float WindLow = 0.0f;
		float WindHigh = 0.0f;
		float SprayLowpass = 0.0f;
		/** Slow wander on the wind so it gusts instead of hissing evenly. */
		float Gust = 0.0f;
	};

	/**
	 * A one-shot: a suspension hit (compression speed, m/s) and/or contact
	 * (closing speed, m/s). Call once per event; zero for the one that did not
	 * happen.
	 */
	APEXSIM_API void Hit(FState& State, float BumpMps, float ImpactMps);

	/** Kerb ribs per second at this speed. */
	APEXSIM_API float CurbRibHz(float SpeedMps);

	/** The squeal's centre frequency for an axle (0 front, 1 rear) at this speed and slide. */
	APEXSIM_API float SquealHz(int32 Axle, float SpeedMps, float Slide);

	/** Renders NumFrames of mono float samples. Never exceeds ±1. */
	APEXSIM_API void Render(FState& State, const FInputs& Inputs, float SampleRate, float* OutFrames, int32 NumFrames);
}
