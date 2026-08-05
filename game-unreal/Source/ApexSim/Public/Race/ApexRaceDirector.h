#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/Actor.h"

#include "ApexRaceDirector.generated.h"

class AApexRaceCarActor;
class UApexMenuFlowSubsystem;
class UApexNetSubsystem;
class UCameraComponent;
class USpringArmComponent;

/**
 * Spawns a car per roster entry and drives them from telemetry.
 *
 * This is the whole "racing" client for now: no track, no physics, no
 * collision. Cars float in an empty world at the positions the server reports,
 * which is exactly enough to prove the UDP transport end to end.
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

	/** Speed of the car the camera is following, in km/h. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	float GetFollowedSpeedKph() const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Race")
	int32 GetSpawnedCarCount() const { return Cars.Num(); }

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

	UApexNetSubsystem* GetNet() const;
	UApexMenuFlowSubsystem* GetFlow() const;

	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<AApexRaceCarActor>> Cars;

	UPROPERTY(Transient)
	TObjectPtr<AApexRaceCarActor> FollowedCar;

	bool bRaceViewActive = false;
	/** Logged once per race so the transport can be confirmed from the log alone. */
	bool bLoggedFirstTelemetry = false;
};
