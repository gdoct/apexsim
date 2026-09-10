#pragma once

#include <atomic>

#include "CoreMinimal.h"
#include "Audio/ApexEngineSound.h"
#include "Sound/SoundWave.h"

#include "ApexEngineSoundWave.generated.h"

struct FSoundGeneratorInitParams;

/**
 * Shared between the wave (written from the game thread) and its generator
 * (read from the audio thread). The pitch range lives here too, not just the
 * live RPM/throttle: the generator is created once when the car starts
 * playing and never rebuilt, so anything the generator reads has to be able
 * to change after that without a new one being made.
 */
struct FApexEngineLiveState
{
	std::atomic<float> Rpm{0.0f};
	std::atomic<float> Throttle{0.0f};
	std::atomic<int32> Gear{0};
	std::atomic<float> IdleRpm{800.0f};
	std::atomic<float> MaxRpm{8000.0f};
};

/**
 * A car's engine note, synthesised continuously from live RPM/throttle rather
 * than decoded from an asset — see ApexUiSoundWave for why procedural.
 *
 * Unlike the UI cues this loops for the actor's lifetime: SetLive() is called
 * every telemetry frame and the generator reads it back on the audio thread
 * through a lock-free shared state, so the note follows the car without ever
 * restarting.
 */
UCLASS()
class APEXSIM_API UApexEngineSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UApexEngineSoundWave(const FObjectInitializer& ObjectInitializer);

	/**
	 * The rev range the timbre opens up across. Neither end is broadcast, so
	 * callers grow it from what they have observed (see AApexRaceCarActor); the
	 * note itself is proportional to RPM and does not depend on this.
	 */
	void SetRpmRange(float IdleRpm, float MaxRpm);

	/** Latest telemetry sample. Safe to call from the game thread every frame. */
	void SetLive(float Rpm, float Throttle, int32 Gear);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> Live;
};
