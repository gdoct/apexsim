#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Race/ApexLapTrace.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexLapTraceSubsystem.generated.h"

/**
 * Records the player's own laps (FApexLapRecorder) while driving and keeps
 * them: each whole lap as CSV under `Saved/Telemetry/<circuit>/<car>/`, the
 * last one and the best valid one (this session's, or a stored one for the
 * circuit and car) in memory for the pause menu's LAP TRACES page.
 * Spectating, the menu's backdrop and replays record nothing.
 *
 * `apexsim.telemetry.Laps` logs what is held and where the files go.
 */
UCLASS()
class APEXSIM_API UApexLapTraceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The last whole lap driven; empty before one. */
	const FApexLapTrace& GetLast() const { return Last; }
	/** The whole lap before the last: what the best is compared with when the last is the best. */
	const FApexLapTrace& GetPrevious() const { return Previous; }
	/** The quickest valid lap on this circuit and car: this session's or a stored one. */
	const FApexLapTrace& GetBest() const { return Best; }
	/**
	 * Every whole lap held for this circuit and car, newest first: this
	 * session's and the newest MaxListedLaps stored ones, the stored best
	 * kept whatever its age. What the LAP TRACES page picks two from.
	 */
	const TArray<FApexLapTrace>& GetLaps() const { return Laps; }
	/** Index of the best in GetLaps(); INDEX_NONE without one. */
	int32 GetBestIndex() const;
	static constexpr int32 MaxListedLaps = 40;
	/** Whether Best came from an earlier session's file. */
	bool IsBestStored() const { return bBestStored; }
	/** Where this circuit and car's laps are written. */
	FString GetFolder() const { return Folder; }
	int32 GetLapsRecorded() const { return LapsRecorded; }
	/** The recorder's lap in progress (FApexLapRecorder::Describe). */
	FString DescribeRecorder() const { return Recorder.Describe(); }

	/** The folder for a circuit and car: Saved/Telemetry/<circuit>/<car>. */
	static FString FolderFor(const FString& TrackName, const FString& CarId);

	/** Feeds one frame as if it came from the server (tests). */
	void AddFrameForTest(const FApexTelemetryFrame& Frame, int32 LocalCarIndex, const FString& TrackName, const FString& CarId);

private:
	UFUNCTION()
	void HandleTelemetry(const FApexTelemetryFrame& Frame);
	void Take(const FApexTelemetryFrame& Frame, int32 LocalCarIndex, const FString& TrackName, const FString& CarId);
	/** A new circuit or car: start over and look for a stored best. */
	void Begin(const FString& TrackName, const FString& CarId);

	FApexLapRecorder Recorder;
	FApexLapTrace Last;
	FApexLapTrace Previous;
	FApexLapTrace Best;
	TArray<FApexLapTrace> Laps;
	bool bBestStored = false;
	FString Key;
	FString Folder;
	int32 LapsRecorded = 0;
	/** Off for tests that feed frames themselves; on otherwise. */
	bool bWriteFiles = true;
};
