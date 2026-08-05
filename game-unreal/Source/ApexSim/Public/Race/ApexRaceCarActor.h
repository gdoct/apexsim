#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"

#include "ApexRaceCarActor.generated.h"

class UStaticMesh;
class UStaticMeshComponent;

/**
 * One car in the world, driven entirely by server telemetry.
 *
 * There is no local physics and no prediction: the server is authoritative and
 * this actor is a puppet. Telemetry arrives at the broadcast rate (60Hz by
 * default) while the client renders faster than that, so snapping straight to
 * each frame would visibly stutter. Instead each frame becomes a target and the
 * actor eases towards it between updates.
 */
UCLASS()
class APEXSIM_API AApexRaceCarActor : public AActor
{
	GENERATED_BODY()

public:
	AApexRaceCarActor();

	virtual void Tick(float DeltaSeconds) override;

	/** Applies one telemetry sample as the new interpolation target. */
	void ApplyTelemetry(const FApexCarTelemetry& Car);

	/** Swaps the displayed mesh. Safe to call with an unset pointer. */
	void SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow);

	void SetDisplayName(const FString& InName) { DisplayName = InName; }
	const FString& GetDisplayName() const { return DisplayName; }

	int32 GetCarIndex() const { return CarIndex; }
	void SetCarIndex(int32 InIndex) { CarIndex = InIndex; }

	float GetSpeedMps() const { return SpeedMps; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> CarMesh;

	/**
	 * How quickly the actor converges on the latest telemetry, in multiples per
	 * second. High enough to stay responsive, low enough to hide the 60Hz steps.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Race")
	float InterpolationSpeed = 18.0f;

	/**
	 * Beyond this distance the actor teleports instead of easing — a respawn or
	 * the first frame after joining should not slide across the map.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Race")
	float TeleportDistanceCm = 2000.0f;

private:
	int32 CarIndex = -1;
	FString DisplayName;
	float SpeedMps = 0.0f;

	FVector TargetLocation = FVector::ZeroVector;
	FRotator TargetRotation = FRotator::ZeroRotator;
	bool bHasTarget = false;
};
