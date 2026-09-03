#include "Audio/ApexUiAudioSubsystem.h"

#include "ApexSettingsSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

namespace
{
	/** Nominal rate for the asset's metadata; the generator renders at the device's real one. */
	constexpr int32 NominalSampleRate = 48000;

	/**
	 * Shortest gap between two plays of the same cue.
	 *
	 * Move and Adjust fire on every step of a held stick or a dragged slider;
	 * their gaps are just under Slate's own navigation repeat rate, so a held
	 * direction still ticks once per step while two moves made in the same
	 * frame collapse into one. The rest are one-off events, where a longer
	 * gap only guards against two handlers reporting the same press.
	 */
	double MinIntervalSeconds(EApexUiSound Sound)
	{
		switch (Sound)
		{
		case EApexUiSound::Move:   return 0.035;
		case EApexUiSound::Adjust: return 0.05;
		default:                   return 0.08;
		}
	}

	/**
	 * Plays a pre-rendered mono clip once, on however many channels the mixer
	 * asks for, and reports itself finished when the clip runs out.
	 */
	class FApexUiCueGenerator : public ISoundGenerator
	{
	public:
		FApexUiCueGenerator(TArray<float>&& InSamples, int32 InNumChannels)
			: Samples(MoveTemp(InSamples))
			, NumChannels(FMath::Max(1, InNumChannels))
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			const int32 NumFrames = NumSamples / NumChannels;
			int32 Written = 0;
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				const float Sample = Cursor < Samples.Num() ? Samples[Cursor++] : 0.0f;
				for (int32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					OutAudio[Written++] = Sample;
				}
			}
			// The mixer expects every sample it asked for to be written.
			while (Written < NumSamples)
			{
				OutAudio[Written++] = 0.0f;
			}
			return NumSamples;
		}

		virtual bool IsFinished() const override
		{
			return Cursor >= Samples.Num();
		}

	private:
		TArray<float> Samples;
		int32 NumChannels = 1;
		int32 Cursor = 0;
	};
}

// ---------------------------------------------------------------------------
// UApexUiSoundWave
// ---------------------------------------------------------------------------

UApexUiSoundWave::UApexUiSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// bProcedural is what routes this wave to CreateSoundGenerator instead of
	// a decoder; NumChannels has to be non-zero or the device refuses to
	// precache it at all.
	bProcedural = true;
	NumChannels = 1;
	SetSampleRate(NominalSampleRate);
	SoundGroup = SOUNDGROUP_UI;
	bLooping = false;
}

void UApexUiSoundWave::SetCue(EApexUiSound InCue)
{
	Cue = InCue;
	Duration = ApexUiSynth::DurationSeconds(Cue);
}

ISoundGeneratorPtr UApexUiSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	// Called on the audio thread. Cue is set once at creation, so reading it
	// here is safe, and the synth touches nothing but its own buffer.
	TArray<float> Samples;
	ApexUiSynth::Render(Cue, InParams.SampleRate > 0.0f ? InParams.SampleRate : NominalSampleRate, Samples);
	return MakeShared<FApexUiCueGenerator, ESPMode::ThreadSafe>(MoveTemp(Samples), InParams.NumChannels);
}

// ---------------------------------------------------------------------------
// UApexUiAudioSubsystem
// ---------------------------------------------------------------------------

UApexUiSoundWave* UApexUiAudioSubsystem::GetWave(EApexUiSound Sound)
{
	const int32 Index = static_cast<int32>(Sound);
	if (Index < 0 || Index >= static_cast<int32>(EApexUiSound::Count))
	{
		return nullptr;
	}

	if (Waves.Num() <= Index)
	{
		Waves.SetNum(static_cast<int32>(EApexUiSound::Count));
	}
	if (!Waves[Index])
	{
		UApexUiSoundWave* Wave = NewObject<UApexUiSoundWave>(this);
		Wave->SetCue(Sound);
		Waves[Index] = Wave;
	}
	return Waves[Index];
}

float UApexUiAudioSubsystem::GetVolume() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
	const UApexSettingsSave* Values = Settings ? Settings->Get() : nullptr;
	return Values ? FMath::Clamp(Values->UiVolume, 0.0f, 1.0f) : 1.0f;
}

void UApexUiAudioSubsystem::Play(EApexUiSound Sound)
{
	const int32 Index = static_cast<int32>(Sound);
	if (Index < 0 || Index >= static_cast<int32>(EApexUiSound::Count))
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastPlayedSeconds[Index] < MinIntervalSeconds(Sound))
	{
		return;
	}
	LastPlayedSeconds[Index] = Now;

	const float Volume = GetVolume();
	if (Volume <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// -nosound, a headless screenshot run, a machine with no device: nothing
	// to play on, and PlaySound2D would only log about it.
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || !GEngine || !GEngine->UseSound())
	{
		return;
	}

	if (UApexUiSoundWave* Wave = GetWave(Sound))
	{
		// A UI sound, so it plays through a paused game as well as the menu.
		UGameplayStatics::PlaySound2D(World, Wave, Volume, 1.0f, 0.0f, nullptr, nullptr, /*bIsUISound*/ true);
	}
}

void ApexUiAudio::Play(const UObject* WorldContextObject, EApexUiSound Sound)
{
	const UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	if (UApexUiAudioSubsystem* Audio = GameInstance ? GameInstance->GetSubsystem<UApexUiAudioSubsystem>() : nullptr)
	{
		Audio->Play(Sound);
	}
}
