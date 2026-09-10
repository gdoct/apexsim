#include "Audio/ApexEngineSound.h"

namespace ApexEngineSynth
{
	namespace
	{
		/**
		 * Cylinder firings per crank revolution. An engine's note is its firing
		 * rate — Rpm/60 revolutions a second, this many bangs in each — so the
		 * fundamental is *proportional* to RPM. Lerping across the rev range
		 * instead (as this did) needs the redline, which is never broadcast: any
		 * guess too low pins the pitch part way up a gear while the revs keep
		 * climbing, and any guess too high wastes most of the sweep.
		 */
		constexpr float FiringsPerRev = 3.0f;

		/**
		 * Bounds on the fundamental: below MinHz an idling engine is inaudible on
		 * anything but a subwoofer, and MaxHz keeps a 15,000rpm F1's harmonics
		 * well inside the band rather than aliasing back down it. Both are far
		 * enough outside the range real cars reach that neither is a limit
		 * anybody hears the pitch stop at.
		 */
		constexpr float MinHz = 30.0f;
		constexpr float MaxHz = 900.0f;

		/** Multiplier per gear above first, compounding: about two semitones across a six-speed. */
		constexpr float GearTrimPerGear = 0.975f;

		/** Trim stops compounding here, so an exotic gearbox cannot detune itself into a drone. */
		constexpr int32 GearTrimTopGear = 8;

		/**
		 * How quickly the tone follows telemetry rather than snapping on every
		 * 60Hz sample. Short enough that a shift's pitch drop lands as a step and
		 * not a swoop; long enough to hide the frame steps in between.
		 */
		constexpr float SmoothingSeconds = 0.06f;

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

		/** The fundamental for an RPM and an already-resolved gear trim. */
		float FundamentalHz(float Rpm, float Trim)
		{
			const float Hz = FMath::Max(Rpm, 0.0f) / 60.0f * FiringsPerRev * Trim;
			return FMath::Clamp(Hz, MinHz, MaxHz);
		}
	}

	float GearTrim(int32 Gear)
	{
		// Neutral and reverse both sit where first does: there is no taller
		// gearing to hear, and reverse is a negative index, not a seventh gear.
		const int32 Forward = FMath::Clamp(Gear, 1, GearTrimTopGear);
		return FMath::Pow(GearTrimPerGear, static_cast<float>(Forward - 1));
	}

	float NoteHz(float Rpm, int32 Gear)
	{
		return FundamentalHz(Rpm, GearTrim(Gear));
	}

	void Render(FState& State, const FParams& Params, float TargetRpm, float TargetThrottle,
		int32 TargetGear, float SampleRate, float* OutFrames, int32 NumFrames)
	{
		if (SampleRate <= 0.0f || NumFrames <= 0)
		{
			return;
		}

		const float TargetTrim = GearTrim(TargetGear);

		if (!State.bPrimed)
		{
			State.SmoothedRpm = TargetRpm;
			State.SmoothedThrottle = TargetThrottle;
			State.SmoothedGearTrim = TargetTrim;
			State.bPrimed = true;
		}

		const float Alpha = 1.0f - FMath::Exp(-1.0f / (SmoothingSeconds * SampleRate));
		const float PhaseStepBase = 2.0f * PI / SampleRate;
		const float RpmRange = FMath::Max(Params.MaxRpm - Params.IdleRpm, 1.0f);

		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			State.SmoothedRpm += (TargetRpm - State.SmoothedRpm) * Alpha;
			State.SmoothedThrottle += (TargetThrottle - State.SmoothedThrottle) * Alpha;
			// Smoothed like the rest: a shift changes the trim in one step, and
			// stepping the frequency mid-buffer is a click.
			State.SmoothedGearTrim += (TargetTrim - State.SmoothedGearTrim) * Alpha;

			// Only the timbre and the loudness ride the rev range, so an
			// unobserved redline no longer costs the pitch sweep.
			const float Fraction = FMath::Clamp((State.SmoothedRpm - Params.IdleRpm) / RpmRange, 0.0f, 1.0f);
			const float Throttle = FMath::Clamp(State.SmoothedThrottle, 0.0f, 1.0f);
			const float Freq = FundamentalHz(State.SmoothedRpm, State.SmoothedGearTrim);

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
