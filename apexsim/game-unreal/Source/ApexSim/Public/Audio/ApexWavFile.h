#pragma once

#include "CoreMinimal.h"

/**
 * A plain PCM WAV: what a recorded replay's sound is written as
 * (UApexReplaySubsystem, `-ApexReplayAudio`). Pure, so the cut to a clip's
 * length is tested without a mixer (`ApexSim.Audio.Wav`).
 */
namespace ApexWav
{
	/**
	 * `Interleaved` float samples (-1..1, `Channels` per frame) as a 16-bit
	 * WAV of exactly `Frames` frames: cut when there are more, padded with
	 * silence when fewer, so the sound lasts as long as the pictures.
	 */
	inline TArray<uint8> Encode16(TConstArrayView<float> Interleaved, int32 Channels, int32 SampleRate, int32 Frames)
	{
		Channels = FMath::Max(Channels, 1);
		Frames = FMath::Max(Frames, 0);
		const uint32 DataBytes = static_cast<uint32>(Frames) * Channels * 2;
		TArray<uint8> Out;
		Out.Reserve(44 + DataBytes);
		auto U32 = [&Out](uint32 V) { for (int32 B = 0; B < 4; ++B) { Out.Add(static_cast<uint8>(V >> (8 * B))); } };
		auto U16 = [&Out](uint16 V) { Out.Add(static_cast<uint8>(V)); Out.Add(static_cast<uint8>(V >> 8)); };
		auto Tag = [&Out](const char* Four) { for (int32 B = 0; B < 4; ++B) { Out.Add(static_cast<uint8>(Four[B])); } };
		Tag("RIFF");
		U32(36 + DataBytes);
		Tag("WAVE");
		Tag("fmt ");
		U32(16);
		U16(1); // PCM
		U16(static_cast<uint16>(Channels));
		U32(static_cast<uint32>(SampleRate));
		U32(static_cast<uint32>(SampleRate) * Channels * 2);
		U16(static_cast<uint16>(Channels * 2));
		U16(16);
		Tag("data");
		U32(DataBytes);
		const int32 Have = Interleaved.Num() / Channels;
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				const float Sample = Frame < Have ? Interleaved[Frame * Channels + Channel] : 0.0f;
				U16(static_cast<uint16>(static_cast<int16>(FMath::RoundToInt(FMath::Clamp(Sample, -1.0f, 1.0f) * 32767.0f))));
			}
		}
		return Out;
	}
}
