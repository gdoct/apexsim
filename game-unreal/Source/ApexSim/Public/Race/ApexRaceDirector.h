#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"

#include "ApexRaceDirector.generated.h"

class AApexRaceCarActor;
class ADirectionalLight;
class ASkyLight;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UApexSettingsSubsystem;
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

	/** Horizontal field of view of both driving cameras, in degrees. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Race")
	void SetFieldOfView(float Degrees);

	/** The car the camera is following, or null outside a race. */
	AApexRaceCarActor* GetFollowedCar() const { return FollowedCar; }

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

	UFUNCTION()
	void HandleLobbyStateUpdated(const FApexLobbyState& LobbyState);

	/** Creates or destroys car actors so they match the roster. */
	void SyncCarsToRoster(const FApexSessionRoster& Roster);
	AApexRaceCarActor* FindCar(int32 CarIndex) const;
	void DestroyAllCars();
	/** Points the camera boom at whichever car the local player is driving. */
	void UpdateCameraTarget();

	/** Placeholder keyboard driving so the transport milestone is testable. */
	void PollDrivingInput();

	/**
	 * The gear an automatic box would ask for this tick, or -128 for none.
	 *
	 * The protocol has no "automatic" flag — the server always takes the gear it
	 * is told — so the box is simply a client that sends the shift the driver
	 * would have sent.
	 */
	int32 PollAutoGearbox();

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
	 * Everything that makes the cameras feel speed: FOV that widens as the
	 * car gains pace, and a perlin micro-shake on both views. All of it is
	 * procedural — there are no camera-shake assets in the project — and all
	 * of it scales with speed squared, so a formation-lap crawl stays calm.
	 */
	void UpdateCameraFeel(float DeltaSeconds);

	/**
	 * Re-light the menu world for driving: aim its sun low and warm, and put
	 * its sky light on real-time capture so ambient light actually matches
	 * the atmosphere instead of a capture taken in the empty menu void.
	 */
	void ApplyRaceEnvironment();
	/** Put the sun back the way the menu had it. */
	void RestoreMenuEnvironment();

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
	UApexSettingsSubsystem* GetSettings() const;

	/** Shift points for the automatic box. Deliberately conservative. */
	static constexpr float AutoShiftUpRpm = 7200.0f;
	static constexpr float AutoShiftDownRpm = 3200.0f;
	/** Minimum spacing between automatic shifts, in seconds. */
	static constexpr double AutoShiftHoldSeconds = 0.35;

	double LastAutoShiftTime = 0.0;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AApexRaceCarActor>> Cars;

	UPROPERTY(Transient)
	TObjectPtr<AApexRaceCarActor> FollowedCar;

	bool bRaceViewActive = false;
	/**
	 * Chase by default — it is the view that shows the car, which is the
	 * only piece of real art on screen. The cockpit is a keypress away.
	 */
	bool bCockpitView = false;
	/** Logged once per race so the transport can be confirmed from the log alone. */
	bool bLoggedFirstTelemetry = false;

	/** How far the FOV widens at full speed, degrees (chase; cockpit gets 60%). */
	static constexpr float SpeedFovBoostDeg = 10.0f;
	/** Speed at which the FOV boost and shake saturate, km/h. */
	static constexpr float FeelTopSpeedKph = 300.0f;

	/** FOV the settings asked for, before the speed boost. */
	float BaseCockpitFov = 95.0f;
	float BaseChaseFov = 80.0f;
	float CurrentFovBoost = 0.0f;
	/** This frame's cockpit micro-shake, composed onto the car rotation. */
	FRotator CockpitShake = FRotator::ZeroRotator;

	/** Menu-world sun state, saved before the race re-aims it. */
	TWeakObjectPtr<ADirectionalLight> MenuSun;
	FRotator MenuSunRotation = FRotator::ZeroRotator;
	FLinearColor MenuSunColor = FLinearColor::White;
	float MenuSunIntensity = 10.0f;
	bool bMenuSunSaved = false;
};
