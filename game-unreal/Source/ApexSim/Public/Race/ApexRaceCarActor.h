#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"
#include "Race/ApexCarMotion.h"
#include "Race/ApexCarWheels.h"
#include "Race/ApexCockpitLayout.h"

#include "ApexRaceCarActor.generated.h"

class UApexEngineSoundWave;
class USpotLightComponent;
class UAudioComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * One car in the world, driven entirely by server telemetry.
 *
 * There is no local physics: the server is authoritative and this actor is a
 * puppet. Telemetry arrives at the broadcast rate (60Hz by default) while the
 * client renders faster than that, and the frames arrive in lumps, so each
 * sample goes into an `ApexMotion::FApexCarMotionBuffer` and every render
 * frame reads the pose a couple of telemetry frames back, blended between the
 * samples on either side, or dead-reckoned past the newest one when a packet
 * is late. Steering, speed and revs are blended the same way, so the cockpit
 * wheel and the dials move with the car.
 */
UCLASS()
class APEXSIM_API AApexRaceCarActor : public AActor
{
	GENERATED_BODY()

public:
	AApexRaceCarActor();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;

	/**
	 * Queues one telemetry sample for the motion buffer to blend through.
	 * `ServerTick` is the frame's tick: the buffer's clock runs in ticks.
	 */
	void ApplyTelemetry(const FApexCarTelemetry& Car, int64 ServerTick);

	/** Swaps the displayed mesh. Safe to call with an unset pointer. */
	void SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow);

	/**
	 * The wheels to draw on the body (the catalog row's `Wheels`); an
	 * unusable spec draws none, which is what a body with its own baked-in
	 * wheels wants.
	 */
	void SetWheels(const FApexWheelSpec& Spec);

	/**
	 * Show or hide just this car's bodywork.
	 *
	 * Used for the car the cockpit camera sits inside when the player would
	 * rather not see its own mesh from within. Per-actor rather than a render
	 * flag because the rest of the field must stay visible.
	 */
	void SetMeshVisible(bool bVisible);

	/**
	 * Headlights on or off: two spot lights at the nose, made the first time
	 * they are asked for, and the tail lights glowing dimly between brakings.
	 * The race director sets them from the session's sky (night, or rain).
	 */
	void SetHeadlights(bool bOn);
	bool HasHeadlights() const { return bHeadlightsOn; }

	UStaticMeshComponent* GetMeshComponent() const { return CarMesh; }

	/** Calls `Fn` for each wheel component, e.g. to hide them from a capture. */
	void ForEachWheelComponent(TFunctionRef<void(UStaticMeshComponent&)> Fn) const { Wheels.ForEachComponent(Fn); }

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

	/** The body's bounds, wheels included, in the car's frame (cm, +X nose, +Z up), for framing it from outside. */
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

	/** Four wheel components on CarMesh, steered and spun from the telemetry. */
	UPROPERTY()
	FApexCarWheelSet Wheels;

	/**
	 * Beyond this distance between two samples the actor teleports instead of
	 * blending — a respawn or the first frame after joining should not slide
	 * across the map.
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

	/** The samples still to be shown; see ApexCarMotion.h. */
	ApexMotion::FApexCarMotionBuffer Motion;
	bool bHasTarget = false;

	/** Body mesh bounds grown by the wheels, in the mesh's own frame. */
	FBoxSphereBounds BodyBounds() const;

	/** The buffer's knobs, read from the console variables each frame. */
	ApexMotion::FSettings MotionSettings() const;

	FString CarClass;
	FApexCockpitOverrides CockpitOverrides;
	FApexCockpitLayout CockpitLayout;
	bool bCockpitLayoutValid = false;

	UPROPERTY(Transient)
	TObjectPtr<UApexEngineSoundWave> EngineSound;

	/**
	 * The mesh's `car_brakelight` slot as a dynamic instance, its
	 * `EmissiveFactor` switched by the brake input; null for a mesh
	 * without the slot.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BrakeLightMaterial;
	/** The slot's authored emissive colour, scaled up when the lights are on. */
	FLinearColor BrakeLightColor = FLinearColor::Red;
	bool bBrakeLightsOn = false;

	/** Lights or darkens the brake lights from `Brake`, writing only on a change. */
	void UpdateBrakeLights();

	/** Seats the headlights at the nose of the current mesh. */
	void PlaceHeadlights();

	UPROPERTY(Transient)
	TObjectPtr<USpotLightComponent> HeadlightLeft;
	UPROPERTY(Transient)
	TObjectPtr<USpotLightComponent> HeadlightRight;
	bool bHeadlightsOn = false;
	/** What the tail lights show: 0 dark, 1 running lights, 2 braking. */
	int32 TailLightState = -1;

	/**
	 * The rev range seen so far for this car. Neither idle nor redline is
	 * broadcast, so the engine note's timbre rides what has been observed:
	 * lowest reading for idle, highest for the redline (see ApexHudWidget).
	 */
	float ObservedIdleRpm = 800.0f;
	float ObservedMaxRpm = 8000.0f;
	bool bHasEngineRange = false;
};
