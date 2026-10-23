#include "Audio/ApexRadioSubsystem.h"

#include "ApexRepoPaths.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Hud/ApexHudDataSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Sound/SoundGenerator.h"
#include "Sound/SoundGroups.h"

namespace
{
	/** Nominal rate for the asset's metadata; the generator renders at the device's real one. */
	constexpr int32 RadioNominalSampleRate = 48000;

	/** Plays a mono clip once, resampled linearly to the device's rate, on every channel the mixer asks for. */
	class FApexRadioGenerator : public ISoundGenerator
	{
	public:
		FApexRadioGenerator(TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> InClip, float DeviceRate, int32 InNumChannels)
			: Clip(MoveTemp(InClip))
			, Step(Clip && DeviceRate > 0.0f ? double(Clip->SampleRate) / DeviceRate : 1.0)
			, NumChannels(FMath::Max(1, InNumChannels))
		{
		}

		virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override
		{
			const int32 NumFrames = NumSamples / NumChannels;
			const int32 Count = Clip ? Clip->Samples.Num() : 0;
			int32 Written = 0;
			for (int32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				float Sample = 0.0f;
				const int32 Index = static_cast<int32>(Cursor);
				if (Index + 1 < Count)
				{
					const float Alpha = static_cast<float>(Cursor - Index);
					Sample = FMath::Lerp(Clip->Samples[Index], Clip->Samples[Index + 1], Alpha);
				}
				else if (Index < Count)
				{
					Sample = Clip->Samples[Index];
				}
				Cursor += Step;
				for (int32 Channel = 0; Channel < NumChannels; ++Channel)
				{
					OutAudio[Written++] = Sample;
				}
			}
			while (Written < NumSamples)
			{
				OutAudio[Written++] = 0.0f;
			}
			return NumSamples;
		}

		virtual bool IsFinished() const override
		{
			return !Clip || Cursor >= Clip->Samples.Num();
		}

	private:
		TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> Clip;
		double Step = 1.0;
		int32 NumChannels = 1;
		double Cursor = 0.0;
	};

	const UApexSettingsSave* GetRadioSettings(const UGameInstance* GameInstance)
	{
		const UApexSettingsSubsystem* Settings = GameInstance ? GameInstance->GetSubsystem<UApexSettingsSubsystem>() : nullptr;
		return Settings ? Settings->Get() : nullptr;
	}

	const TCHAR* ChannelName(EApexRadioChannel Channel)
	{
		return Channel == EApexRadioChannel::Spotter ? TEXT("spotter") : TEXT("engineer");
	}
}

// ---------------------------------------------------------------------------
// UApexRadioSoundWave
// ---------------------------------------------------------------------------

UApexRadioSoundWave::UApexRadioSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bProcedural = true;
	NumChannels = 1;
	SetSampleRate(RadioNominalSampleRate);
	SoundGroup = SOUNDGROUP_Voice;
	bLooping = false;
}

void UApexRadioSoundWave::SetClip(TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> InClip)
{
	Clip = MoveTemp(InClip);
	Duration = Clip ? static_cast<float>(Clip->DurationSeconds()) : 0.0f;
}

ISoundGeneratorPtr UApexRadioSoundWave::CreateSoundGenerator(const FSoundGeneratorInitParams& InParams)
{
	// The audio thread: the clip is immutable and shared, so reading it here is safe.
	return MakeShared<FApexRadioGenerator, ESPMode::ThreadSafe>(
		Clip, InParams.SampleRate > 0.0f ? InParams.SampleRate : RadioNominalSampleRate, InParams.NumChannels);
}

// ---------------------------------------------------------------------------
// UApexRadioSubsystem
// ---------------------------------------------------------------------------

UApexRadioSubsystem* UApexRadioSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UApexRadioSubsystem>() : nullptr;
}

FString UApexRadioSubsystem::Folder()
{
	// A package carries them beside the game as `Audio/radio`
	// (scripts/build_release.ps1); the editor reads the content repo's.
	return FPlatformProperties::RequiresCookedData()
		? FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Audio"), TEXT("radio")))
		: ApexRepoPaths::ContentPath(TEXT("audio/radio"));
}

bool UApexRadioSubsystem::IsChannelOn(EApexRadioChannel Channel) const
{
	const UApexSettingsSave* Values = GetRadioSettings(GetGameInstance());
	if (!Values)
	{
		return true;
	}
	return Channel == EApexRadioChannel::Spotter ? Values->bRadioSpotter : Values->bRadioEngineer;
}

float UApexRadioSubsystem::GetVolume() const
{
	const UApexSettingsSave* Values = GetRadioSettings(GetGameInstance());
	return Values ? FMath::Clamp(Values->RadioVolume, 0.0f, 1.0f) : 1.0f;
}

void UApexRadioSubsystem::Stop()
{
	if (UAudioComponent* Component = Playing.Get())
	{
		Component->Stop();
	}
	Playing.Reset();
	Queue.Reset();
	Triggers.Reset();
}

void UApexRadioSubsystem::Update(bool bDriving)
{
	// A gap of a frame or more: the radio was off (a menu, a replay). It
	// starts over rather than reporting everything that changed meanwhile.
	if (LastUpdateFrame + 1 < GFrameCounter)
	{
		Stop();
	}
	LastUpdateFrame = GFrameCounter;

	UApexHudDataSubsystem* Hud = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexHudDataSubsystem>() : nullptr;
	if (!Hud || !Hud->IsRaceActive())
	{
		Stop();
		return;
	}
	Hud->RefreshIfStale();
	const FApexRadioInputs Inputs = ApexRadio::InputsFromHud(Hud->GetData(), bDriving);
	const double Now = FPlatformTime::Seconds();

	TArray<FName> Lines;
	Triggers.Step(Inputs, Now, Lines);
	for (const FName Line : Lines)
	{
		if (IsChannelOn(ApexRadio::SpecOf(Line).Channel))
		{
			Queue.Push(Line, Now);
		}
	}

	// No fuel talk while a car is alongside.
	bool bInterrupt = false;
	const FName Next = Queue.Pop(Now, Inputs.bLeft || Inputs.bRight, bInterrupt);
	if (!Next.IsNone())
	{
		Say(Next, bInterrupt, Now);
	}
}

TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> UApexRadioSubsystem::GetClip(FName Line)
{
	if (const TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe>* Found = Clips.Find(Line))
	{
		return *Found;
	}
	const FString Path = FPaths::Combine(Folder(), Line.ToString() + TEXT(".wav"));
	TArray<uint8> Bytes;
	TSharedPtr<FApexRadioClip, ESPMode::ThreadSafe> Clip = MakeShared<FApexRadioClip, ESPMode::ThreadSafe>();
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent) || !ApexRadio::ParseWav(Bytes, Clip->Samples, Clip->SampleRate))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Radio: %s would not read (16-bit PCM WAV expected)"), *Path);
		Clip.Reset();
	}
	Clips.Add(Line, Clip);
	return Clip;
}

void UApexRadioSubsystem::Say(FName Line, bool bInterrupt, double Now)
{
	const EApexRadioChannel Channel = ApexRadio::SpecOf(Line).Channel;
	const TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> Clip = GetClip(Line);
	// Without its file a line still takes the time a spoken one would, so the
	// queue paces the same.
	const double Duration = Clip ? Clip->DurationSeconds() : 1.0;
	Queue.Started(Line, Now, Duration);
	UE_LOG(LogApexSim, Log, TEXT("Radio: %s (%s%s)"), *Line.ToString(), ChannelName(Channel), bInterrupt ? TEXT(", cuts in") : TEXT(""));

	if (bInterrupt)
	{
		if (UAudioComponent* Component = Playing.Get())
		{
			Component->Stop();
		}
	}
	Playing.Reset();

	const float Volume = GetVolume();
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!Clip || Volume <= KINDA_SMALL_NUMBER || !World || !GEngine || !GEngine->UseSound())
	{
		return;
	}
	TObjectPtr<UApexRadioSoundWave>& Wave = Waves.FindOrAdd(Line);
	if (!Wave)
	{
		Wave = NewObject<UApexRadioSoundWave>(this);
		Wave->SetClip(Clip);
	}
	Playing = UGameplayStatics::SpawnSound2D(World, Wave, Volume);
}
