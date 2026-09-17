#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexSessionRecorder.generated.h"

class UApexNetSubsystem;

/** What one car did over a session, accumulated from the telemetry stream. */
USTRUCT(BlueprintType)
struct FApexCarResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	int32 CarIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	FString DriverName;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	bool bIsAi = false;

	/** Completed laps, in order. A lap in progress is not in here. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	TArray<float> LapTimes;

	/** Zero when no lap was completed. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	float BestLapSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	float TopSpeedMps = 0.0f;

	/** The splits that best lap was made of, seconds; empty until it is set. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	TArray<float> BestLapSplitsSeconds;

	/** Laps the car completed inside track limits, as the server judged them. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	int32 ValidLaps = 0;

	/** Race distance covered in metres: full laps plus the station into the current one. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	float DistanceM = 0.0f;

	/** Classified position from the server once the car took the flag; 0 if it did not. */
	UPROPERTY(BlueprintReadOnly, Category = "ApexSim|Results")
	int32 FinishPosition = 0;

	int32 LapsCompleted() const { return LapTimes.Num(); }
};

/**
 * Watches the telemetry stream and turns it into a session result.
 *
 * The protocol has no results message: the server never says who won or what
 * anyone's best lap was. But every telemetry frame carries each car's lap
 * number, current lap time, speed and whether it is on the track, so the whole
 * classification can be derived here — a lap time is the last `CurrentLapTimeMs`
 * seen before the lap counter moves on.
 *
 * Lives on the game instance so it keeps recording while the menu is hidden.
 */
UCLASS()
class APEXSIM_API UApexSessionRecorder : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Results best-classified first: for the session that just ended, or — while
	 * it is still being recorded — the running order, kept up to date frame by
	 * frame so a driver who has finished can watch the rest come in.
	 */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	const TArray<FApexCarResult>& GetResults() const { return Results; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	bool HasResults() const { return Results.Num() > 0; }

	/** True while the session is still running, so the results are provisional. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	bool IsRecording() const { return bRecording; }

	/**
	 * Classification order. Cars the server classified come first, by position;
	 * then, when `bByDistance` (a race), the rest by distance covered; then most
	 * laps and the quickest lap, which is all a practice session has to go on.
	 */
	static bool ClassifiesAhead(const FApexCarResult& A, const FApexCarResult& B, bool bByDistance);

	/** The local player's row, or null if this client did not drive. */
	const FApexCarResult* FindLocalResult() const;

	/** Track the session was run on, captured when it started. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	const FString& GetTrackId() const { return TrackId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	EApexGameMode GetGameMode() const { return RecordedMode; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	int32 GetLapLimit() const { return LapLimit; }

	/**
	 * How much the local best beat the previous personal best by; positive means
	 * an improvement. Zero when there was no previous time or no improvement.
	 */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	float GetPersonalBestDelta() const { return PersonalBestDelta; }

	/** Spread of the local driver's laps, 0..1, where 1 is metronomic. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Results")
	float GetConsistency() const;

private:
	UFUNCTION() void HandleTelemetry(const FApexTelemetryFrame& Frame);
	UFUNCTION() void HandleSessionStateChanged(EApexSessionState NewState);
	UFUNCTION() void HandleRosterUpdated(const FApexSessionRoster& Roster);

	/** Starts a new recording; called when a session begins driving. */
	void BeginRecording();
	/** Freezes and classifies what was recorded, and files any personal best. */
	void FinishRecording();

	FApexCarResult& FindOrAddCar(int32 CarIndex);
	void SortResults();
	/** Names and AI flags come from the roster, not the telemetry. */
	void ApplyRosterNames();

	UPROPERTY(Transient)
	TArray<FApexCarResult> Results;

	/** Per car: the lap number seen last frame, to spot the counter rolling over. */
	TMap<int32, int32> LastLapNumber;

	FString TrackId;
	float TrackLengthM = 0.0f;
	EApexGameMode RecordedMode = EApexGameMode::Lobby;
	int32 LapLimit = 0;
	float PersonalBestDelta = 0.0f;
	bool bRecording = false;
};
