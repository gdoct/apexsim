#include "Audio/ApexEngineSoundWave.h"

#include "AudioDefines.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

namespace
{
	/** Nominal rate for the asset's metadata; the generator renders at the device's real one. */
	constexpr int32 EngineNominalSampleRate = 48000;

	/** Continuously renders the engine tone from the live RPM/gear/throttle the wave last set. */
	class FApexEngineSoundGenerator : public ISoundGenerator
	{
	public:
		FApexEngineSoundGenerator(TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> InLive,
			float InSampleRate, int32 InNumChannels)
			: Live(MoveTemp(InLive))
			, SampleRate(InSampleRate > 0.0f ? InSampleRate : static_cast<float>(EngineNominalSampleRate))
			, NumChannels(FMath::Max(1, InNumChannels))
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			const int32 NumFrames = NumSamples / NumChannels;
			Mono.SetNumUninitialized(NumFrames);

			// Read fresh every buffer: ObservedMaxRpm keeps growing for as long as
			// the car plays, and a copy taken once at construction would freeze it.
			const ApexEngineSynth::FParams Params{
				Live->IdleRpm.load(std::memory_order_relaxed), Live->MaxRpm.load(std::memory_order_relaxed)
			};
			ApexEngineSynth::Render(State, Params, Live->Rpm.load(std::memory_order_relaxed),
				Live->Throttle.load(std::memory_order_relaxed), Live->Gear.load(std::memory_order_relaxed),
				SampleRate, Mono.GetData(), NumFrames);

			int32 Written = 0;
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				for (int32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					OutAudio[Written++] = Mono[Frame];
				}
			}
			// The mixer expects every sample it asked for to be written.
			while (Written < NumSamples)
			{
				OutAudio[Written++] = 0.0f;
			}
			return NumSamples;
		}

		// Runs for as long as the audio component plays it; nothing here ever finishes on its own.
		virtual bool IsFinished() const override { return false; }

	private:
		TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> Live;
		ApexEngineSynth::FState State;
		float SampleRate;
		int32 NumChannels;
		TArray<float> Mono;
	};
}

UApexEngineSoundWave::UApexEngineSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Live(MakeShared<FApexEngineLiveState, ESPMode::ThreadSafe>())
{
	// bProcedural routes this wave to CreateSoundGenerator instead of a decoder;
	// bLooping plus the sentinel duration below tell the mixer never to stop it on its own.
	bProcedural = true;
	bLooping = true;
	NumChannels = 1;
	SetSampleRate(EngineNominalSampleRate);
	SoundGroup = SOUNDGROUP_Effects;
	Duration = INDEFINITELY_LOOPING_DURATION;
}

void UApexEngineSoundWave::SetRpmRange(float IdleRpm, float MaxRpm)
{
	Live->IdleRpm.store(IdleRpm, std::memory_order_relaxed);
	Live->MaxRpm.store(FMath::Max(MaxRpm, IdleRpm + 1.0f), std::memory_order_relaxed);
}

void UApexEngineSoundWave::SetLive(float Rpm, float Throttle, int32 Gear)
{
	Live->Rpm.store(Rpm, std::memory_order_relaxed);
	Live->Throttle.store(FMath::Clamp(Throttle, 0.0f, 1.0f), std::memory_order_relaxed);
	Live->Gear.store(Gear, std::memory_order_relaxed);
}

ISoundGeneratorPtr UApexEngineSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	// Called on the audio thread; everything it needs is read from Live through atomics.
	return MakeShared<FApexEngineSoundGenerator, ESPMode::ThreadSafe>(Live, InParams.SampleRate, InParams.NumChannels);
}
