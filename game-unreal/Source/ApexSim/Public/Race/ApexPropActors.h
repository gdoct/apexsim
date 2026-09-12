#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ApexPropActors.generated.h"

class UStaticMeshComponent;

/**
 * A sky prop that drifts: the blimp. Placed by the track import at the
 * altitude the scene gives it, it wanders back and forth along its own
 * heading and sways in yaw, purely for the eye — nothing on the server
 * knows it exists.
 */
UCLASS()
class APEXSIM_API AApexSkyDriftActor : public AActor
{
	GENERATED_BODY()

public:
	AApexSkyDriftActor();

	UStaticMeshComponent* GetMesh() const { return Mesh; }

	/** Peak drift speed along the heading, cm/s, and how far each way it goes. */
	UPROPERTY(EditAnywhere, Category = "Apex|Drift")
	float DriftSpeedCmPerS = 200.0f;
	UPROPERTY(EditAnywhere, Category = "Apex|Drift")
	float DriftRangeCm = 15000.0f;
	/** Peak yaw rate of the sway, degrees per second. */
	UPROPERTY(EditAnywhere, Category = "Apex|Drift")
	float SwayDegPerS = 0.3f;
	UPROPERTY(EditAnywhere, Category = "Apex|Drift")
	float SwayAmplitudeDeg = 4.0f;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Apex|Drift")
	TObjectPtr<UStaticMeshComponent> Mesh;

	FTransform Home;
	float Elapsed = 0.0f;
};

/**
 * A prop with a turning part: the ferris wheel. The root mesh is the
 * structure, `Rotor` the wheel, attached at the hub and spun about its
 * local Y (the wheel's axle after the glTF round trip) at a fairground's
 * pace.
 */
UCLASS()
class APEXSIM_API AApexRotorActor : public AActor
{
	GENERATED_BODY()

public:
	AApexRotorActor();

	UStaticMeshComponent* GetMesh() const { return Mesh; }
	UStaticMeshComponent* GetRotor() const { return Rotor; }

	UPROPERTY(EditAnywhere, Category = "Apex|Rotor")
	float RevolutionsPerMinute = 0.5f;

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Apex|Rotor")
	TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(VisibleAnywhere, Category = "Apex|Rotor")
	TObjectPtr<UStaticMeshComponent> Rotor;

	float AngleDeg = 0.0f;
};
