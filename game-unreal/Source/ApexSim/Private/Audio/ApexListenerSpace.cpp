#include "Audio/ApexListenerSpace.h"

namespace ApexSpace
{
	namespace
	{
		/** Freeverb's tunings, in samples at 44.1 kHz: mutually prime-ish, so the echoes never line up. */
		constexpr float CombSamples[CombCount] = {1116.0f, 1277.0f, 1422.0f, 1557.0f};
		constexpr float AllpassSamples[AllpassCount] = {556.0f, 341.0f};
		constexpr float TuningRate = 44100.0f;
		/** The right channel's lines are this much longer: two ears never get the same echo. */
		constexpr float StereoSpreadSamples = 23.0f;
		constexpr float AllpassGain = 0.5f;
		/** Into the combs: four of them sum, and each rings up by 1/(1-g). */
		constexpr float ReverbInputGain = 0.12f;
		/**
		 * Only what is above this goes into the reverb. Combs fed the low orders
		 * turn them into room modes: some notes doubled, others cancelled against
		 * the direct sound (a 70 Hz firing note lost the whole of the seat's low
		 * shelf that way), and an engine sweeps through all of them. The weight
		 * stays direct and tight; the space is in the mids and the top.
		 */
		constexpr float ReverbSendHighpassHz = 250.0f;

		float SpaceSoftClip(float Value)
		{
			const float X = FMath::Clamp(Value, -1.5f, 1.5f);
			return X - X * X * X / 6.75f;
		}

		/** The shelf adds level as well as weight; this gives some of it back so the seat is not simply louder. */
		float OutputTrim(const FParams& Params)
		{
			return 1.0f / (1.0f + 0.3f * Params.LowShelf);
		}

		float SpaceAlphaForHz(float Hz, float SampleRate)
		{
			return 1.0f - FMath::Exp(-2.0f * PI * FMath::Min(Hz, SampleRate * 0.45f) / SampleRate);
		}
	}

	FParams ForSeat(ESeat Seat)
	{
		FParams Params;
		switch (Seat)
		{
		case ESeat::Cabin:
			Params.LowShelf = 1.6f;
			Params.LowShelfHz = 150.0f;
			Params.LowpassHz = 3500.0f;
			Params.Wet = 0.6f;
			Params.DecaySeconds = 0.35f;
			Params.Size = 0.3f;
			Params.Damping = 0.5f;
			break;
		case ESeat::OpenCockpit:
			Params.LowShelf = 1.1f;
			Params.LowShelfHz = 150.0f;
			Params.Wet = 0.35f;
			Params.DecaySeconds = 0.5f;
			Params.Size = 0.5f;
			Params.Damping = 0.35f;
			break;
		case ESeat::Chase:
			Params.LowShelf = 1.4f;
			Params.LowShelfHz = 130.0f;
			Params.Wet = 0.5f;
			Params.DecaySeconds = 1.1f;
			Params.Size = 1.0f;
			Params.Damping = 0.3f;
			break;
		case ESeat::None:
		default:
			break;
		}
		return Params;
	}

	void Process(FState& State, const FParams& Params, float SampleRate, const float* Mono, float* OutStereo, int32 NumFrames)
	{
		if (SampleRate <= 0.0f || NumFrames <= 0)
		{
			return;
		}
		if (State.LastSize != Params.Size)
		{
			// Same buffers read at a new length would replay what is in them at
			// the wrong time: a seat change starts the space empty.
			// (In place: a temporary FChannel is 75 KB of the audio thread's stack.)
			for (FChannel& Channel : State.Channels)
			{
				for (int32 Index = 0; Index < CombCount; ++Index)
				{
					for (float& Slot : Channel.Comb[Index]) { Slot = 0.0f; }
					Channel.CombAt[Index] = 0;
					Channel.CombDamp[Index] = 0.0f;
				}
				for (int32 Index = 0; Index < AllpassCount; ++Index)
				{
					for (float& Slot : Channel.Allpass[Index]) { Slot = 0.0f; }
					Channel.AllpassAt[Index] = 0;
				}
			}
			State.LastSize = Params.Size;
		}

		const float Scale = FMath::Max(Params.Size, 0.05f) * SampleRate / TuningRate;
		int32 CombLength[2][CombCount];
		float CombGain[2][CombCount];
		int32 AllpassLength[2][AllpassCount];
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Spread = Side == 0 ? 0.0f : StereoSpreadSamples * SampleRate / TuningRate;
			for (int32 Index = 0; Index < CombCount; ++Index)
			{
				CombLength[Side][Index] = FMath::Clamp(static_cast<int32>(CombSamples[Index] * Scale + Spread), 8, CombCapacity);
				// Each trip round the comb loses what makes the whole fall 60 dB in DecaySeconds.
				CombGain[Side][Index] = FMath::Pow(10.0f,
					-3.0f * static_cast<float>(CombLength[Side][Index]) / (SampleRate * FMath::Max(Params.DecaySeconds, 0.05f)));
			}
			for (int32 Index = 0; Index < AllpassCount; ++Index)
			{
				AllpassLength[Side][Index] =
					FMath::Clamp(static_cast<int32>(AllpassSamples[Index] * Scale + Spread), 4, AllpassCapacity);
			}
		}
		const float Damp = FMath::Clamp(Params.Damping, 0.0f, 0.95f);
		const float ShelfAlpha = SpaceAlphaForHz(Params.LowShelfHz, SampleRate);
		const float SendAlpha = SpaceAlphaForHz(ReverbSendHighpassHz, SampleRate);
		const bool bLowpass = Params.LowpassHz > 0.0f;
		const float LowpassAlpha = bLowpass ? SpaceAlphaForHz(Params.LowpassHz, SampleRate) : 1.0f;

		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			const float In = Mono[Frame];
			State.ShelfLowpass += (In - State.ShelfLowpass) * ShelfAlpha;
			State.DirectLowpass += (In - State.DirectLowpass) * LowpassAlpha;
			// The shelf is added after the bulkhead: the structure carries the
			// low orders whatever the bulkhead does to the rest.
			const float Direct = State.DirectLowpass + Params.LowShelf * State.ShelfLowpass;
			State.SendLowpass += (State.DirectLowpass - State.SendLowpass) * SendAlpha;
			const float Send = (State.DirectLowpass - State.SendLowpass) * ReverbInputGain;

			for (int32 Side = 0; Side < 2; ++Side)
			{
				FChannel& Channel = State.Channels[Side];
				float Wet = 0.0f;
				for (int32 Index = 0; Index < CombCount; ++Index)
				{
					float& Slot = Channel.Comb[Index][Channel.CombAt[Index]];
					const float Out = Slot;
					Channel.CombDamp[Index] = Out * (1.0f - Damp) + Channel.CombDamp[Index] * Damp;
					Slot = Send + Channel.CombDamp[Index] * CombGain[Side][Index];
					if (++Channel.CombAt[Index] >= CombLength[Side][Index])
					{
						Channel.CombAt[Index] = 0;
					}
					Wet += Out;
				}
				for (int32 Index = 0; Index < AllpassCount; ++Index)
				{
					float& Slot = Channel.Allpass[Index][Channel.AllpassAt[Index]];
					const float Buffered = Slot;
					const float Out = Buffered - Wet;
					Slot = Wet + Buffered * AllpassGain;
					if (++Channel.AllpassAt[Index] >= AllpassLength[Side][Index])
					{
						Channel.AllpassAt[Index] = 0;
					}
					Wet = Out;
				}
				OutStereo[Frame * 2 + Side] = SpaceSoftClip(Direct * OutputTrim(Params) + Wet * Params.Wet);
			}
		}
	}
}
