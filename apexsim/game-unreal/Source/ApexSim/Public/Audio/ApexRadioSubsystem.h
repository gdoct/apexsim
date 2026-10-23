#pragma once

#include "CoreMinimal.h"
#include "Audio/ApexRadio.h"
#include "Sound/SoundWave.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexRadioSubsystem.generated.h"

class UAudioComponent;
struct FSoundGeneratorInitParams;

/** One voice line's samples, as read from its WAV. */
struct FApexRadioClip
{
	TArray<float> Samples;
	int32 SampleRate = 16000;

	double DurationSeconds() const { return SampleRate > 0 ? double(Samples.Num()) / SampleRate : 0.0; }
};

/**
 * A procedural wave that plays one radio clip, resampled to the device's
 * rate by its generator (the UApexUiSoundWave route: no decoded asset).
 */
UCLASS()
class APEXSIM_API UApexRadioSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UApexRadioSoundWave(const FObjectInitializer& ObjectInitializer);

	/** The clip this wave plays. Set once, before the first play. */
	void SetClip(TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> InClip);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> Clip;
};

/**
 * The spotter and the race engineer (docs/game/audio.md, "Radio"). The race
 * director calls Update every frame the player drives; it reads the HUD's
 * data, lets FApexRadioTriggers pick the lines and FApexRadioQueue order
 * them, and plays each from `audio/radio/<id>.wav` (the content repo in the
 * editor, `Audio/radio` beside ApexSim.exe in a package), at the settings'
 * radio volume, with each voice switched on or off there.
 *
 * Every line said is logged as `Radio: <id> (<channel>)`, with or without a
 * sound device, so an unattended run's log shows what the radio said.
 */
UCLASS()
class APEXSIM_API UApexRadioSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	static UApexRadioSubsystem* Get(const UObject* WorldContext);

	/**
	 * One frame of driving. A frame skipped (a menu, a replay, a watched
	 * race) starts the radio over: it takes stock again and says nothing of
	 * what it missed.
	 */
	void Update(bool bDriving);

	/** Stops the line playing and forgets the queue. */
	void Stop();

	/** Where the clips are read from. */
	static FString Folder();

private:
	TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe> GetClip(FName Line);
	void Say(FName Line, bool bInterrupt, double Now);
	bool IsChannelOn(EApexRadioChannel Channel) const;
	float GetVolume() const;

	FApexRadioTriggers Triggers;
	FApexRadioQueue Queue;
	uint64 LastUpdateFrame = 0;

	/** Read clips by line; a line whose file would not read maps to null and is not tried again. */
	TMap<FName, TSharedPtr<const FApexRadioClip, ESPMode::ThreadSafe>> Clips;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UApexRadioSoundWave>> Waves;

	TWeakObjectPtr<UAudioComponent> Playing;
};
