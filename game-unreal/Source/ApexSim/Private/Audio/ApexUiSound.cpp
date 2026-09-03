#include "Audio/ApexUiSound.h"

namespace ApexUiSynth
{
	namespace
	{
		/** Linear ramp in, so a note does not start with a click. */
		constexpr float AttackSeconds = 0.003f;
		/** Forced ramp out at the very end of every note, whatever the decay left. */
		constexpr float ReleaseSeconds = 0.004f;
		/** How far the exponential decay has fallen by the end of a note: -30 dB. */
		constexpr float DecayFloor = 0.03f;

		/**
		 * A note's waveform: a sine with a touch of second harmonic. The harmonic
		 * keeps a short tone from sounding like a test signal without turning it
		 * into a buzz; the denominator keeps the sum inside ±1.
		 */
		float Waveform(float Phase)
		{
			return (FMath::Sin(Phase) + 0.25f * FMath::Sin(2.0f * Phase)) / 1.25f;
		}

		/** The buzz for Denied: odd harmonics, so it reads as a raspberry rather than a beep. */
		float BuzzWaveform(float Phase)
		{
			return (FMath::Sin(Phase) + 0.33f * FMath::Sin(3.0f * Phase) + 0.2f * FMath::Sin(5.0f * Phase)) / 1.53f;
		}

		/** Amplitude at T seconds into a note lasting Seconds. */
		float Envelope(float T, float Seconds)
		{
			const float Attack = FMath::Clamp(T / AttackSeconds, 0.0f, 1.0f);
			const float Release = FMath::Clamp((Seconds - T) / ReleaseSeconds, 0.0f, 1.0f);
			// Exponential decay reaching DecayFloor at the end of the note.
			const float Decay = FMath::Pow(DecayFloor, T / Seconds);
			return Attack * Release * Decay;
		}
	}

	TArray<FNote> NotesFor(EApexUiSound Sound)
	{
		// Pitches are from one scale so the cues sound like a family: Accept
		// rises a fifth, Back falls a fourth, the toasts move a third either
		// way. Move and Adjust are single ticks, high and short, because they
		// fire constantly and have to stay out of the way.
		switch (Sound)
		{
		case EApexUiSound::Move:
			return { { 1568.0f, 0.045f, 0.28f } };

		case EApexUiSound::Accept:
			return { { 659.3f, 0.06f, 0.42f }, { 987.8f, 0.13f, 0.42f } };

		case EApexUiSound::Back:
			return { { 783.9f, 0.06f, 0.38f }, { 587.3f, 0.13f, 0.38f } };

		case EApexUiSound::Denied:
			return { { 164.8f, 0.07f, 0.34f }, { 0.0f, 0.03f, 0.0f }, { 164.8f, 0.09f, 0.34f } };

		case EApexUiSound::Adjust:
			return { { 2093.0f, 0.02f, 0.2f } };

		case EApexUiSound::Notice:
			return { { 1046.5f, 0.07f, 0.34f }, { 1318.5f, 0.14f, 0.34f } };

		case EApexUiSound::Error:
			return { { 440.0f, 0.09f, 0.4f }, { 329.6f, 0.17f, 0.4f } };

		default:
			return {};
		}
	}

	float DurationSeconds(EApexUiSound Sound)
	{
		float Total = 0.0f;
		for (const FNote& Note : NotesFor(Sound))
		{
			Total += Note.Seconds;
		}
		return Total;
	}

	void Render(EApexUiSound Sound, float SampleRate, TArray<float>& OutSamples)
	{
		OutSamples.Reset();
		if (SampleRate <= 0.0f)
		{
			return;
		}

		const bool bBuzz = Sound == EApexUiSound::Denied;
		const float Step = 1.0f / SampleRate;

		for (const FNote& Note : NotesFor(Sound))
		{
			const int32 NumSamples = FMath::Max(1, FMath::RoundToInt(Note.Seconds * SampleRate));
			OutSamples.Reserve(OutSamples.Num() + NumSamples);

			// A note of zero pitch is a rest. Rendered as explicit silence so the
			// gap is part of the buffer and the timing survives the mixer.
			if (Note.Hz <= 0.0f || Note.Gain <= 0.0f)
			{
				OutSamples.AddZeroed(NumSamples);
				continue;
			}

			// Phase is accumulated rather than computed from time so that the
			// waveform is continuous whatever the rate, and each note restarts
			// at zero phase so it begins at a zero crossing.
			const float PhaseStep = 2.0f * PI * Note.Hz * Step;
			float Phase = 0.0f;
			for (int32 Index = 0; Index < NumSamples; ++Index)
			{
				const float T = Index * Step;
				const float Sample = (bBuzz ? BuzzWaveform(Phase) : Waveform(Phase)) * Envelope(T, Note.Seconds) * Note.Gain;
				OutSamples.Add(FMath::Clamp(Sample, -1.0f, 1.0f));
				Phase += PhaseStep;
				if (Phase > 2.0f * PI)
				{
					Phase -= 2.0f * PI;
				}
			}
		}
	}
}
