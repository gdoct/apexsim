#pragma once

#include "CoreMinimal.h"
#include "Audio/ApexUiSound.h"
#include "Sound/SoundWave.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexUiAudioSubsystem.generated.h"

struct FSoundGeneratorInitParams;

/**
 * A sound wave that carries no samples of its own: it renders one cue at the
 * audio device's rate the moment the mixer asks for a generator, and the
 * generator ends the sound when the cue has been played out.
 *
 * Procedural on purpose. A USoundWave built from raw PCM at runtime has to
 * pretend to be a decoded asset, and every engine version moves that goalpost;
 * the generator route is the one the mixer documents for sounds that exist
 * only in code.
 */
UCLASS()
class APEXSIM_API UApexUiSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UApexUiSoundWave(const FObjectInitializer& ObjectInitializer);

	/** Which cue this wave plays. Set once, before the first play. */
	void SetCue(EApexUiSound InCue);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	UPROPERTY()
	EApexUiSound Cue = EApexUiSound::Move;
};

/**
 * Plays the menu's cues.
 *
 * Widgets say what happened — a move, an accept, a step back — and this turns
 * it into sound, scaled by the menu-sound volume in the settings and throttled
 * so that a screen which moves focus twice while handling one press, or a
 * slider dragged across its whole range, does not machine-gun.
 *
 * Lives on the game instance because the cues outlive any one screen and the
 * waves are cheap to keep: one per cue, built on first use.
 */
UCLASS()
class APEXSIM_API UApexUiAudioSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Audio")
	void Play(EApexUiSound Sound);

private:
	UApexUiSoundWave* GetWave(EApexUiSound Sound);

	/** The menu-sound volume from the settings, 0..1. */
	float GetVolume() const;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UApexUiSoundWave>> Waves;

	/** When each cue last played, for the throttle. Zero until it has. */
	double LastPlayedSeconds[static_cast<int32>(EApexUiSound::Count)] = {};
};

namespace ApexUiAudio
{
	/**
	 * Plays a cue on behalf of any object with a world — a widget, usually.
	 * Silent when there is no game instance, no audio, or nothing to play on,
	 * so callers never have to check.
	 */
	APEXSIM_API void Play(const UObject* WorldContextObject, EApexUiSound Sound);
}
