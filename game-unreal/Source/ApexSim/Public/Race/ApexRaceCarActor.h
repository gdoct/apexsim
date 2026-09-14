#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"
#include "Race/ApexCockpitLayout.h"

#include "ApexRaceCarActor.generated.h"

class UApexEngineSoundWave;
class UAudioComponent;
class UMaterialInstanceDynamic;
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
	virtual void BeginPlay() override;

	/** Applies one telemetry sample as the new interpolation target. */
	void ApplyTelemetry(const FApexCarTelemetry& Car);

	/** Swaps the displayed mesh. Safe to call with an unset pointer. */
	void SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow);

	/**
	 * Show or hide just this car's bodywork.
	 *
	 * Used for the car the cockpit camera sits inside when the player would
	 * rather not see its own mesh from within. Per-actor rather than a render
	 * flag because the rest of the field must stay visible.
	 */
	void SetMeshVisible(bool bVisible);

	UStaticMeshComponent* GetMeshComponent() const { return CarMesh; }

	/**
	 * What the catalog knows about this car's cockpit: its class, which
	 * decides open or closed, and any hand-placed points. Invalidates the
	 * derived layout.
	 */
	void SetCockpitSpec(const FString& InCarClass, const FApexCockpitOverrides& InOverrides);

	/**
	 * Where the seat, wheel and mirrors sit in this car's frame, derived
	 * from its mesh the first time it is asked for.
	 */
	const FApexCockpitLayout& GetCockpitLayout();

	/** The body's bounds in the car's frame (cm, +X nose, +Z up), for framing it from outside. */
	FBox GetBodyBox() const;

	/**
	 * Scale on the engine note, 1 as designed. The menu's demo race turns it
	 * down so the cars sit under the shell's own sounds.
	 */
	void SetEngineVolume(float Scale);

	void SetDisplayName(const FString& InName) { DisplayName = InName; }
	const FString& GetDisplayName() const { return DisplayName; }

	int32 GetCarIndex() const { return CarIndex; }
	void SetCarIndex(int32 InIndex) { CarIndex = InIndex; }

	float GetSpeedMps() const { return SpeedMps; }

	/** Gear the server last reported: -1 reverse, 0 neutral, 1.. forward. */
	int32 GetGear() const { return Gear; }

	float GetEngineRpm() const { return EngineRpm; }
	float GetThrottle() const { return Throttle; }
	float GetBrake() const { return Brake; }
	/** Steering input as the server holds it: -1..1, positive to the LEFT. */
	float GetSteering() const { return Steering; }
	int32 GetCurrentLap() const { return CurrentLap; }
	int32 GetCurrentLapTimeMs() const { return CurrentLapTimeMs; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> CarMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UAudioComponent> EngineAudio;

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
	int32 Gear = 0;
	float EngineRpm = 0.0f;
	float Throttle = 0.0f;
	float Brake = 0.0f;
	float Steering = 0.0f;
	int32 CurrentLap = 0;
	int32 CurrentLapTimeMs = 0;

	FVector TargetLocation = FVector::ZeroVector;
	FRotator TargetRotation = FRotator::ZeroRotator;
	bool bHasTarget = false;

	FString CarClass;
	FApexCockpitOverrides CockpitOverrides;
	FApexCockpitLayout CockpitLayout;
	bool bCockpitLayoutValid = false;

	UPROPERTY(Transient)
	TObjectPtr<UApexEngineSoundWave> EngineSound;

	/**
	 * The mesh's `car_brakelight` slot as a dynamic instance, its
	 * `EmissiveStrength` switched by the brake input; null for a mesh
	 * without the slot.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BrakeLightMaterial;
	bool bBrakeLightsOn = false;

	/** Lights or darkens the brake lights from `Brake`, writing only on a change. */
	void UpdateBrakeLights();

	/**
	 * The rev range seen so far for this car. Neither idle nor redline is
	 * broadcast, so the engine note's timbre rides what has been observed:
	 * lowest reading for idle, highest for the redline (see ApexHudWidget).
	 */
	float ObservedIdleRpm = 800.0f;
	float ObservedMaxRpm = 8000.0f;
	bool bHasEngineRange = false;
};
