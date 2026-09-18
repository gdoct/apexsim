#pragma once

#include <atomic>

#include "CoreMinimal.h"
#include "Audio/ApexRoadSound.h"
#include "Sound/SoundWave.h"

#include "ApexRoadSoundWave.generated.h"

struct FSoundGeneratorInitParams;

/**
 * Shared between the wave (game thread) and its generator (audio thread), all
 * atomics. The hits are handed over by exchange: the game thread keeps the
 * largest since the generator last looked, the generator takes it and leaves
 * zero, so a kerb strike between two audio buffers is played once.
 */
struct FApexRoadLiveState
{
	std::atomic<float> SpeedMps{0.0f};
	std::atomic<float> FrontSlide{0.0f};
	std::atomic<float> RearSlide{0.0f};
	std::atomic<float> Lockup{0.0f};
	std::atomic<float> Wheelspin{0.0f};
	std::atomic<float> CurbLeft{0.0f};
	std::atomic<float> CurbRight{0.0f};
	std::atomic<float> OffTrack{0.0f};
	std::atomic<bool> bWet{false};
	std::atomic<float> PendingBumpMps{0.0f};
	std::atomic<float> PendingImpactMps{0.0f};
};

/**
 * The local car's tyres, kerbs, road and wind, synthesised continuously —
 * see ApexRoadSound.h for the voices and UApexEngineSoundWave for the shape
 * of the class.
 */
UCLASS()
class APEXSIM_API UApexRoadSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UApexRoadSoundWave(const FObjectInitializer& ObjectInitializer);

	/** The levels that hold until the next call. Game thread, every frame. */
	void SetLive(const ApexRoadSynth::FInputs& Inputs);

	/** A one-shot: suspension hit and/or contact, m/s; zero for the one that did not happen. */
	void Hit(float BumpMps, float ImpactMps);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedRef<FApexRoadLiveState, ESPMode::ThreadSafe> Live;
};
