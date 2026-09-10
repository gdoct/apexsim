#pragma once

#include "CoreMinimal.h"

/**
 * A car engine, synthesised.
 *
 * Unlike the UI cues in ApexUiSound.h this is not a fixed clip: RPM, gear and
 * throttle change every telemetry frame (60Hz) while the audio device pulls
 * samples far more often, so rendering happens continuously, one buffer at a
 * time, with a running phase and a smoothed copy of the inputs carried in
 * FState between calls instead of starting fresh each render.
 */
namespace ApexEngineSynth
{
	/**
	 * The car's rev range, used for timbre and loudness only — how "open" the
	 * engine sounds, not what note it plays. Redline is never broadcast (see
	 * ApexHudWidget's RPM strip), so callers are expected to grow MaxRpm from
	 * the highest reading seen so far rather than know it up front; getting it
	 * wrong now costs some brightness rather than the whole pitch sweep.
	 */
	struct FParams
	{
		float IdleRpm = 800.0f;
		float MaxRpm = 8000.0f;
	};

	/** Running state between render calls: phase, smoothed inputs, and a private noise generator. */
	struct FState
	{
		float Phase = 0.0f;
		float SmoothedRpm = 0.0f;
		float SmoothedThrottle = 0.0f;
		float SmoothedGearTrim = 1.0f;
		uint32 NoiseState = 0;
		bool bPrimed = false;
	};

	/**
	 * How much a gear lowers the note at an unchanged RPM. 1.0 in first (and in
	 * neutral or reverse), a little less in each gear above it: taller gearing
	 * reads as a longer, lazier engine even at the same crank speed, and it is
	 * what makes an upshift audible in the moment the RPM has yet to fall.
	 */
	APEXSIM_API float GearTrim(int32 Gear);

	/**
	 * The fundamental the engine plays at this RPM and gear, in Hz.
	 *
	 * Proportional to RPM — an engine's note *is* its firing rate — so the drop
	 * across an upshift is heard as the same interval the revs fell by, and
	 * nothing saturates part way up a gear.
	 */
	APEXSIM_API float NoteHz(float Rpm, int32 Gear);

	/**
	 * Renders NumFrames of mono float samples into OutFrames, advancing State
	 * towards TargetRpm/TargetThrottle/TargetGear as it goes. Never exceeds +-1.
	 */
	APEXSIM_API void Render(FState& State, const FParams& Params, float TargetRpm, float TargetThrottle,
		int32 TargetGear, float SampleRate, float* OutFrames, int32 NumFrames);
}
