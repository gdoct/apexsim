#pragma once

#include "CoreMinimal.h"

/**
 * Where the player hears their own car from.
 *
 * Every other car is a point in the world: a mono source the engine's
 * spatialiser pans, attenuates and dulls with distance. The player's own car
 * is not a point, it is where they are — sat inside it, or a few metres
 * behind it — and heard dry and panned like the rest it sounds like a
 * recording of an engine played at them. What is missing is the space:
 *
 *  - **Weight.** A car's structure carries the engine's low orders straight
 *    to the seat, and a cabin is a small box with its resonances under
 *    200 Hz. A low shelf is that.
 *  - **The cabin.** A closed car's bulkhead takes the top off the engine (a
 *    low-pass), and the sound that does get in bounces round a space a
 *    couple of metres across: a short, dense, quickly damped reverb.
 *  - **The trackside.** From a chase camera the car is heard in the open,
 *    off barriers and stands: less of it, longer.
 *
 * Mono in, stereo out. The reverb is the classic Schroeder/Freeverb shape —
 * four damped combs into two all-passes, the right channel's delays a few
 * samples longer than the left's so the two decorrelate — sized and timed
 * per seat, and fed only what is above 250 Hz, so the low end stays direct.
 * Pure maths over plain structs, like the synths that feed it.
 */
namespace ApexSpace
{
	enum class ESeat : uint8
	{
		/** Not the player's car: no processing, the 3D path plays it. */
		None,
		/** Inside a closed cabin (GT, prototype). */
		Cabin,
		/** In an open cockpit: the engine is right there, unfiltered, with the bodywork's reflections. */
		OpenCockpit,
		/** Behind the car, outside. */
		Chase,
	};

	struct FParams
	{
		/** Gain of the low shelf's added band (0 = none; 1 = +6 dB under LowShelfHz). */
		float LowShelf = 0.0f;
		float LowShelfHz = 150.0f;
		/** One-pole low-pass on the direct sound, Hz; 0 for none. */
		float LowpassHz = 0.0f;
		/** Reverb level against the direct sound. */
		float Wet = 0.0f;
		/** Time for the reverb to fall 60 dB. */
		float DecaySeconds = 0.5f;
		/** Scale on the delay lengths: 1 is a large space, 0.3 a cabin. */
		float Size = 1.0f;
		/** 0..1: how quickly the reverb loses its top end. */
		float Damping = 0.3f;
	};

	APEXSIM_API FParams ForSeat(ESeat Seat);

	constexpr int32 CombCount = 4;
	constexpr int32 AllpassCount = 2;
	/** The longest comb at the highest device rate (1580 samples at 44.1 kHz, scaled to 96). */
	constexpr int32 CombCapacity = 4096;
	constexpr int32 AllpassCapacity = 1536;

	struct FChannel
	{
		float Comb[CombCount][CombCapacity] = {};
		int32 CombAt[CombCount] = {};
		float CombDamp[CombCount] = {};
		float Allpass[AllpassCount][AllpassCapacity] = {};
		int32 AllpassAt[AllpassCount] = {};
	};

	/** ~150 KB: keep it on the heap. */
	struct FState
	{
		FChannel Channels[2];
		float ShelfLowpass = 0.0f;
		float DirectLowpass = 0.0f;
		/** What the reverb send's high-pass takes out. */
		float SendLowpass = 0.0f;
		/** The size the delay lines were last run at: a change empties them rather than replaying them at a new pitch. */
		float LastSize = 0.0f;
	};

	/**
	 * NumFrames of mono in, NumFrames interleaved stereo frames out (2 x NumFrames
	 * floats). Never exceeds ±1.
	 */
	APEXSIM_API void Process(FState& State, const FParams& Params, float SampleRate,
		const float* Mono, float* OutStereo, int32 NumFrames);
}
