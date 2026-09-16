#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ApexRainActor.generated.h"

class UInstancedStaticMeshComponent;

/**
 * Rain, drawn as streaks around whichever camera is active.
 *
 * There are no particle assets in the project, so the rain is an instanced
 * mesh: a few hundred thin slivers of the engine's cube in a box that
 * travels with the camera. Each falls in world space and is wrapped back into
 * the box when it leaves it, so a camera moving with the car sees the drops
 * stream past as they would; a streak is stretched along its apparent
 * velocity (fall speed against the camera's own), which is what a shutter
 * makes of rain at speed. The density is the session's `RainIntensity`.
 */
UCLASS()
class APEXSIM_API AApexRainActor : public AActor
{
	GENERATED_BODY()

public:
	AApexRainActor();

	virtual void Tick(float DeltaSeconds) override;

	/** 0 stops the rain (and hides the streaks); 1 is a downpour. */
	void SetIntensity(float Intensity);
	float GetIntensity() const { return RainIntensity; }

private:
	/** Puts the active streaks somewhere in the box the first time they are needed. */
	void EnsureStreaks(int32 Count);
	/** The box's centre this frame: the camera, nudged along where it is going. */
	FVector BoxCentre(const FVector& CameraLocation, const FVector& CameraVelocity) const;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UInstancedStaticMeshComponent> Streaks;

	/** World-space position of every streak, active or not. */
	TArray<FVector> Positions;
	TArray<FTransform> Transforms;
	float RainIntensity = 0.0f;
	int32 ActiveStreaks = 0;

	FVector LastCameraLocation = FVector::ZeroVector;
	FVector CameraVelocity = FVector::ZeroVector;
	bool bHaveCamera = false;
	FRandomStream Random;

	/** Streaks at full intensity. */
	static constexpr int32 MaxStreaks = 1500;
	/** Half-extents of the box around the camera, cm. */
	static constexpr float HalfWidthCm = 1400.0f;
	static constexpr float HalfHeightCm = 900.0f;
	/** Terminal velocity of a drop, cm/s. */
	static constexpr float FallSpeedCmPerS = 900.0f;
	/** How long a shutter sees a drop for: sets the streak's length from its speed. */
	static constexpr float ShutterSeconds = 1.0f / 60.0f;
	static constexpr float StreakThicknessCm = 1.6f;
};
