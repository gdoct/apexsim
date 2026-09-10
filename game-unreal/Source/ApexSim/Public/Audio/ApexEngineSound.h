#pragma once

#include "CoreMinimal.h"

/**
 * A car engine, synthesised.
 *
 * Unlike the UI cues in ApexUiSound.h this is not a fixed clip: RPM and
 * throttle change every telemetry frame (60Hz) while the audio device pulls
 * samples far more often, so rendering happens continuously, one buffer at a
 * time, with a running phase and a smoothed copy of the inputs carried in
 * FState between calls instead of starting fresh each render.
 */
namespace ApexEngineSynth
{
	/**
	 * A car's pitch range. Redline is never broadcast (see ApexHudWidget's RPM
	 * strip), so callers are expected to grow MaxRpm from the highest reading
	 * seen so far rather than know it up front.
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
		uint32 NoiseState = 0;
		bool bPrimed = false;
	};

	/**
	 * Renders NumFrames of mono float samples into OutFrames, advancing State
	 * towards TargetRpm/TargetThrottle as it goes. Never exceeds +-1.
	 */
	APEXSIM_API void Render(FState& State, const FParams& Params, float TargetRpm, float TargetThrottle,
		float SampleRate, float* OutFrames, int32 NumFrames);
}
