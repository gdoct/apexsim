#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"
#include "Audio/ApexListenerSpace.h"
#include "Race/ApexCarMotion.h"
#include "Race/ApexCarDrsFlap.h"
#include "Race/ApexCarWheels.h"
#include "Race/ApexCockpitLayout.h"

#include "ApexRaceCarActor.generated.h"

class UApexEngineSoundWave;
class UApexRoadSoundWave;
namespace ApexRoadSynth { struct FInputs; }
class USpotLightComponent;
class UAudioComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;
struct FApexCarLivery;

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
	 * Repaints the body (ApexCarLivery.h); null is the model as authored.
	 * After SetCarMesh, which drops any livery the old body wore.
	 */
	void SetLivery(const struct FApexCarLivery* Livery);

	/**
	 * The wheels to draw on the body (the catalog row's `Wheels`); an
	 * unusable spec draws none, which is what a body with its own baked-in
	 * wheels wants.
	 */
	void SetWheels(const FApexWheelSpec& Spec);

	/**
	 * The DRS flap drawn apart from the body (the catalog row's `DrsFlap`;
	 * F1 cars only). It opens when the telemetry says the car's DRS is open.
	 * An unusable spec draws none: the body carries its whole wing.
	 */
	void SetDrsFlap(const FApexDrsFlapSpec& Spec);

	/** The flap's component when the car has one, for tinting and capture flags. */
	UStaticMeshComponent* GetDrsFlapComponent() const { return DrsFlap.HasFlap() ? DrsFlap.GetComponent() : nullptr; }

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
	 * Which engine this car has (the catalog row's `EngineSound`, with the
	 * class filling in for a row from before the field): cylinders, crank,
	 * exhaust and rev range, which is everything the synthesiser is built from.
	 */
	void SetEngineSound(const FApexEngineSoundSpec& Spec, const FString& InCarClass);

	/**
	 * Scale on everything this car plays, 1 as designed. The menu's demo race
	 * turns it down so the cars sit under the shell's own sounds.
	 */
	void SetEngineVolume(float Scale);

	/**
	 * The player's Audio settings, 0..1 each, on top of SetEngineVolume: every
	 * engine, this engine again when it is somebody else's car, and the
	 * tyres/road.
	 */
	void SetMixVolumes(float Engine, float OtherCars, float Road);

	/**
	 * Tyres, kerbs, road and wind for the car the player is driving: the
	 * levels for this frame and any hit that arrived with it (m/s, zero for
	 * none). Only the local car is ever fed — the signals come from the
	 * server's DriverFeedback, which no other car has.
	 */
	void UpdateRoadSound(const ApexRoadSynth::FInputs& Levels, float BumpMps, float ImpactMps);

	/** No feedback any more (garage, pause, race over): the road falls silent. */
	void StopRoadSound();

	/**
	 * Where the player hears this car from. `None` for everybody else's: a
	 * mono point in the world, attenuated and dulled by distance, at the
	 * "other cars" volume. The player's own car plays a second, stereo engine
	 * instead, unspatialised and run through ApexSpace: the cabin's weight,
	 * bulkhead and short reverb from a closed cockpit (`Cabin` becomes
	 * `OpenCockpit` by itself for an open car), the trackside's from a chase
	 * camera.
	 */
	void SetListenerSeat(ApexSpace::ESeat Seat);

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

	/** The stereo engine the player's own car plays in place of EngineAudio; see SetListenerSeat. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UAudioComponent> OwnEngineAudio;

	/** Tyres, kerbs, road and wind; silent on every car but the one being driven. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UAudioComponent> RoadAudio;

	/** Four wheel components on CarMesh, steered and spun from the telemetry. */
	UPROPERTY()
	FApexCarWheelSet Wheels;

	/** The DRS flap on CarMesh, opened from the telemetry. */
	UPROPERTY()
	FApexCarDrsFlap DrsFlap;

	/** The newest telemetry's `bDrsOpen`: where the flap is swinging to. */
	bool bDrsOpen = false;

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

protected:
	/**
	 * What the dials, the cockpit rig and the brake lights read, for a
	 * puppet that is not fed by telemetry (the ghost drives itself from a
	 * recorded lap).
	 */
	void SetPuppetState(float InSpeedMps, float InEngineRpm, int32 InGear, float InSteering, float InThrottle, float InBrake);

private:
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

	UPROPERTY(Transient)
	TObjectPtr<UApexEngineSoundWave> OwnEngineSound;

	UPROPERTY(Transient)
	TObjectPtr<UApexRoadSoundWave> RoadSound;

	/** SetEngineVolume's scale and the settings' two, multiplied onto the components. */
	float VolumeScale = 1.0f;
	float EngineMix = 1.0f;
	float OtherCarsMix = 1.0f;
	float RoadMix = 1.0f;
	ApexSpace::ESeat ListenerSeat = ApexSpace::ESeat::None;
	void ApplyVolumes();
	/** Plays whichever of the two engines the seat calls for, once the car has a place in the world. */
	void RefreshEnginePlayback();

	/**
	 * The mesh's `car_brakelight` slot as a dynamic instance, its
	 * `EmissiveFactor` switched by the brake input; null for a mesh
	 * without the slot.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BrakeLightMaterial;

	/**
	 * The mesh's `car_taillight` slot as a dynamic instance: the running
	 * lights, lit all session (brighter by day, the running share by night).
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TailLightMaterial;
	FLinearColor TailLightColor = FLinearColor::Red;

	/** A livery's instances are on the paint, accent and logo slots. */
	bool bLiveryApplied = false;
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
};
