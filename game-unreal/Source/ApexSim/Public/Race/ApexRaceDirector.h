#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"

#include "ApexRaceDirector.generated.h"

class AApexRaceCarActor;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UCameraComponent;
class ULevelStreamingDynamic;
class USpringArmComponent;

/**
 * Spawns a car per roster entry and drives them from telemetry.
 *
 * Cars are driven purely from telemetry: the client runs no physics and no
 * collision of its own, it just places each car where the server says it is.
 *
 * The circuit itself is streamed in alongside them, from the level the
 * `ApexTrackImport` commandlet generated for the session's track. It is a
 * level *instance* rather than a map travel on purpose — travelling would
 * tear down the menu world this director and the whole UI live in.
 *
 * Placed once in the menu level. It does nothing until a session is joined and
 * the UDP handshake completes.
 */
UCLASS()
class APEXSIM_API AApexRaceDirector : public AActor
{
	GENERATED_BODY()

public:
	AApexRaceDirector();

	virtual void Tick(float DeltaSeconds) override;

	/** Finds the director in the current world, or null. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race", meta = (WorldContext = "WorldContextObject"))
	static AApexRaceDirector* Find(const UObject* WorldContextObject);

	/** Takes over the view: possesses the camera and starts following the local car. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void BeginRaceView();

	/** Hands the view back and despawns the cars. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void EndRaceView();

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsRaceViewActive() const { return bRaceViewActive; }

	/** Swap between the cockpit view and the chase camera. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetCockpitView(bool bCockpit);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsCockpitView() const { return bCockpitView; }

	/** Speed of the car the camera is following, in km/h. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	float GetFollowedSpeedKph() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	int32 GetSpawnedCarCount() const { return Cars.Num(); }

	/** True once the session's track level has finished streaming in. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	bool IsTrackLevelLoaded() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> Root;

	/** Chase camera. Follows the local car, or car 0 if the roster has no local entry. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> ChaseCamera;

	/**
	 * Driver's-eye camera. Not on the boom: the boom deliberately lags and
	 * ignores pitch and roll, which is right for a chase view and wrong for
	 * a cockpit, where the horizon tilting with the car is most of the point.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> CockpitCamera;

	/** Eye position in the car's own frame: +X is the nose, +Z is up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Race")
	FVector CockpitEyeOffset = FVector(20.0f, 0.0f, 110.0f);

	/** Fallback mesh for cars with no catalog row (AI drivers have no car id). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ApexSim|Race")
	TSoftObjectPtr<UStaticMesh> DefaultCarMesh;

private:
	UFUNCTION()
	void HandleRosterUpdated(const FApexSessionRoster& Roster);

	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);

	UFUNCTION()
	void HandleSessionLeft();

	/** Creates or destroys car actors so they match the roster. */
	void SyncCarsToRoster(const FApexSessionRoster& Roster);
	AApexRaceCarActor* FindCar(int32 CarIndex) const;
	void DestroyAllCars();
	/** Points the camera boom at whichever car the local player is driving. */
	void UpdateCameraTarget();

	/** Placeholder keyboard driving so the transport milestone is testable. */
	void PollDrivingInput();

	/** Camera-view and other non-driving keys. */
	void PollViewInput();

	/**
	 * Give the player controller key input while racing, and hand it back to
	 * the menu afterwards.
	 *
	 * The menu shell runs in UI-only input, where the controller sees no key
	 * state at all and every driving key silently does nothing. Game-and-UI
	 * is what lets the driving keys through while Escape still reaches the
	 * root widget, which only handles Escape and lets everything else fall
	 * past it to the controller.
	 */
	void ApplyRaceInputMode(bool bRacing);

	/** Point the active camera and hide the car when sitting inside it. */
	void ApplyCameraMode();

	/** Ride the followed car's full orientation, banking and all. */
	void UpdateCockpitCamera();

	/**
	 * Content path of the level for the session's track, or empty if there
	 * is no session, no track, or no imported level for it.
	 *
	 * Resolved by convention from the track file the server names —
	 * `tracks/real/Monza.yaml` -> `/Game/Tracks/Monza/L_Monza` — which is
	 * exactly how the importer names what it generates, so the two cannot
	 * drift apart without the lookup failing loudly.
	 */
	FString ResolveTrackLevelPath() const;

	void LoadTrackLevel();
	void UnloadTrackLevel();

	UPROPERTY(Transient)
	TObjectPtr<ULevelStreamingDynamic> TrackLevel;

	UApexNetSubsystem* GetNet() const;
	UApexMenuFlowSubsystem* GetFlow() const;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AApexRaceCarActor>> Cars;

	UPROPERTY(Transient)
	TObjectPtr<AApexRaceCarActor> FollowedCar;

	bool bRaceViewActive = false;
	/** Cockpit by default; the chase camera is a keypress away. */
	bool bCockpitView = true;
	/** Logged once per race so the transport can be confirmed from the log alone. */
	bool bLoggedFirstTelemetry = false;
};
