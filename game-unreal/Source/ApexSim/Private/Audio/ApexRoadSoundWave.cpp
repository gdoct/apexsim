#include "Audio/ApexRoadSoundWave.h"

#include "AudioDefines.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

namespace
{
	constexpr int32 RoadNominalSampleRate = 48000;

	class FApexRoadSoundGenerator : public ISoundGenerator
	{
	public:
		FApexRoadSoundGenerator(TSharedRef<FApexRoadLiveState, ESPMode::ThreadSafe> InLive,
			float InSampleRate, int32 InNumChannels)
			: Live(MoveTemp(InLive))
			, SampleRate(InSampleRate > 0.0f ? InSampleRate : static_cast<float>(RoadNominalSampleRate))
			, NumChannels(FMath::Max(1, InNumChannels))
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			const int32 NumFrames = NumSamples / NumChannels;
			Mono.SetNumUninitialized(NumFrames);

			ApexRoadSynth::FInputs Inputs;
			Inputs.SpeedMps = Live->SpeedMps.load(std::memory_order_relaxed);
			Inputs.FrontSlide = Live->FrontSlide.load(std::memory_order_relaxed);
			Inputs.RearSlide = Live->RearSlide.load(std::memory_order_relaxed);
			Inputs.Lockup = Live->Lockup.load(std::memory_order_relaxed);
			Inputs.Wheelspin = Live->Wheelspin.load(std::memory_order_relaxed);
			Inputs.CurbLeft = Live->CurbLeft.load(std::memory_order_relaxed);
			Inputs.CurbRight = Live->CurbRight.load(std::memory_order_relaxed);
			Inputs.OffTrack = Live->OffTrack.load(std::memory_order_relaxed);
			Inputs.bWet = Live->bWet.load(std::memory_order_relaxed);

			const float Bump = Live->PendingBumpMps.exchange(0.0f, std::memory_order_relaxed);
			const float Impact = Live->PendingImpactMps.exchange(0.0f, std::memory_order_relaxed);
			if (Bump > 0.0f || Impact > 0.0f)
			{
				ApexRoadSynth::Hit(State, Bump, Impact);
			}
			ApexRoadSynth::Render(State, Inputs, SampleRate, Mono.GetData(), NumFrames);

			int32 Written = 0;
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				for (int32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					OutAudio[Written++] = Mono[Frame];
				}
			}
			while (Written < NumSamples)
			{
				OutAudio[Written++] = 0.0f;
			}
			return NumSamples;
		}

		virtual bool IsFinished() const override { return false; }

	private:
		TSharedRef<FApexRoadLiveState, ESPMode::ThreadSafe> Live;
		ApexRoadSynth::FState State;
		float SampleRate;
		int32 NumChannels;
		TArray<float> Mono;
	};
}

UApexRoadSoundWave::UApexRoadSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Live(MakeShared<FApexRoadLiveState, ESPMode::ThreadSafe>())
{
	bProcedural = true;
	bLooping = true;
	NumChannels = 1;
	SetSampleRate(RoadNominalSampleRate);
	SoundGroup = SOUNDGROUP_Effects;
	Duration = INDEFINITELY_LOOPING_DURATION;
}

void UApexRoadSoundWave::SetLive(const ApexRoadSynth::FInputs& Inputs)
{
	Live->SpeedMps.store(Inputs.SpeedMps, std::memory_order_relaxed);
	Live->FrontSlide.store(FMath::Clamp(Inputs.FrontSlide, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->RearSlide.store(FMath::Clamp(Inputs.RearSlide, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->Lockup.store(FMath::Clamp(Inputs.Lockup, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->Wheelspin.store(FMath::Clamp(Inputs.Wheelspin, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->CurbLeft.store(FMath::Clamp(Inputs.CurbLeft, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->CurbRight.store(FMath::Clamp(Inputs.CurbRight, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->OffTrack.store(FMath::Clamp(Inputs.OffTrack, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->bWet.store(Inputs.bWet, std::memory_order_relaxed);
}

void UApexRoadSoundWave::Hit(float BumpMps, float ImpactMps)
{
	// Largest since the generator last took them; see FApexRoadLiveState.
	if (BumpMps > Live->PendingBumpMps.load(std::memory_order_relaxed))
	{
		Live->PendingBumpMps.store(BumpMps, std::memory_order_relaxed);
	}
	if (ImpactMps > Live->PendingImpactMps.load(std::memory_order_relaxed))
	{
		Live->PendingImpactMps.store(ImpactMps, std::memory_order_relaxed);
	}
}

ISoundGeneratorPtr UApexRoadSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	return MakeShared<FApexRoadSoundGenerator, ESPMode::ThreadSafe>(Live, InParams.SampleRate, InParams.NumChannels);
}
