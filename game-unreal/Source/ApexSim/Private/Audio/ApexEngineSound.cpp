#include "Audio/ApexEngineSound.h"

namespace ApexEngineSynth
{
	namespace
	{
		/** Tone at idle and at MaxRpm. Deliberately a synthesiser's idea of an engine, not a recording of one. */
		constexpr float IdleHz = 42.0f;
		constexpr float MaxHz = 165.0f;

		/** How quickly the tone follows telemetry rather than snapping on every 60Hz sample. */
		constexpr float SmoothingSeconds = 0.12f;

		/**
		 * A private xorshift32 generator for the "grit" layer. Not FMath::FRand:
		 * that shares state across every caller, and this one runs on the audio
		 * thread on every sample.
		 */
		float NextNoise(uint32& State)
		{
			if (State == 0)
			{
				State = 0x9E3779B9u;
			}
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			return (static_cast<float>(State) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
		}
	}

	void Render(FState& State, const FParams& Params, float TargetRpm, float TargetThrottle,
		float SampleRate, float* OutFrames, int32 NumFrames)
	{
		if (SampleRate <= 0.0f || NumFrames <= 0)
		{
			return;
		}

		if (!State.bPrimed)
		{
			State.SmoothedRpm = TargetRpm;
			State.SmoothedThrottle = TargetThrottle;
			State.bPrimed = true;
		}

		const float Alpha = 1.0f - FMath::Exp(-1.0f / (SmoothingSeconds * SampleRate));
		const float PhaseStepBase = 2.0f * PI / SampleRate;
		const float RpmRange = FMath::Max(Params.MaxRpm - Params.IdleRpm, 1.0f);

		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			State.SmoothedRpm += (TargetRpm - State.SmoothedRpm) * Alpha;
			State.SmoothedThrottle += (TargetThrottle - State.SmoothedThrottle) * Alpha;

			const float Fraction = FMath::Clamp((State.SmoothedRpm - Params.IdleRpm) / RpmRange, 0.0f, 1.0f);
			const float Throttle = FMath::Clamp(State.SmoothedThrottle, 0.0f, 1.0f);
			const float Freq = FMath::Lerp(IdleHz, MaxHz, Fraction);

			State.Phase += Freq * PhaseStepBase;
			if (State.Phase > 2.0f * PI)
			{
				State.Phase -= 2.0f * PI;
			}

			// A handful of harmonics rather than a band-limited sawtooth: cheap,
			// and the higher ones fading in with RPM/throttle is what reads as
			// the engine "opening up" instead of just getting louder.
			const float P = State.Phase;
			const float Tone =
				FMath::Sin(P) * 0.5f +
				FMath::Sin(2.0f * P) * 0.25f * (0.3f + 0.7f * Fraction) +
				FMath::Sin(3.0f * P) * 0.15f * Fraction +
				FMath::Sin(4.0f * P) * 0.10f * Throttle;

			const float Grit = NextNoise(State.NoiseState) * 0.05f * Fraction * Throttle;
			const float Gain = FMath::Lerp(0.32f, 0.8f, Fraction) * FMath::Lerp(0.75f, 1.0f, Throttle);

			OutFrames[Index] = FMath::Clamp((Tone + Grit) * Gain, -1.0f, 1.0f);
		}
	}
}
