#pragma once

#include <atomic>

#include "CoreMinimal.h"
#include "Audio/ApexEngineSound.h"
#include "Sound/SoundWave.h"

#include "ApexEngineSoundWave.generated.h"

struct FApexEngineSoundSpec;
struct FSoundGeneratorInitParams;

namespace ApexEngineAudio
{
	/**
	 * The synthesiser's engine for a catalog row. A row carries the car.toml's
	 * `[sound]` table and rev range; one imported before the table existed
	 * (Cylinders == 0) gets the engine its class usually has — a turbo V6 for
	 * F1, a crossplane V8 for GT3, a flat-plane V8 for a prototype — and a row
	 * with no rev range the class's usual one, so every car makes a sound.
	 */
	APEXSIM_API ApexEngineSynth::FEngineSpec MakeSpec(const FApexEngineSoundSpec& Row, const FString& CarClass);
}

/**
 * Shared between the wave (written from the game thread) and its generator
 * (read from the audio thread). The engine itself lives here too, not just
 * the live RPM/throttle: the generator is created once when the car starts
 * playing and never rebuilt, while the catalog row that says what engine
 * this is arrives whenever the roster does — so anything the generator reads
 * has to be able to change after that without a new one being made.
 */
struct FApexEngineLiveState
{
	std::atomic<float> Rpm{0.0f};
	std::atomic<float> Throttle{0.0f};
	std::atomic<int32> Gear{0};

	/** Bumped on every SetSpec; the generator copies Spec (under SpecLock) when it has moved on. */
	std::atomic<uint32> SpecSerial{0};
	FCriticalSection SpecLock;
	ApexEngineSynth::FEngineSpec Spec;
};

/**
 * A car's engine, synthesised continuously from live RPM/throttle/gear rather
 * than decoded from an asset — see ApexEngineSound.h for the model and
 * ApexUiSoundWave for why procedural.
 *
 * Unlike the UI cues this loops for the actor's lifetime: SetLive() is called
 * every frame and the generator reads it back on the audio thread through a
 * lock-free shared state, so the engine follows the car without ever
 * restarting.
 */
UCLASS()
class APEXSIM_API UApexEngineSoundWave : public USoundWave
{
	GENERATED_BODY()

public:
	UApexEngineSoundWave(const FObjectInitializer& ObjectInitializer);

	/** Which engine this is. Safe at any time; the running generator picks it up at its next buffer. */
	void SetSpec(const ApexEngineSynth::FEngineSpec& Spec);

	/** What the engine is doing now. Safe to call from the game thread every frame. */
	void SetLive(float Rpm, float Throttle, int32 Gear);

	virtual ISoundGeneratorPtr CreateSoundGenerator(const FSoundGeneratorInitParams& InParams) override;

private:
	TSharedRef<FApexEngineLiveState, ESPMode::ThreadSafe> Live;
};
