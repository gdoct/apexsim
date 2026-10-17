#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Race/ApexOwnCarPredictor.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexPredictionSubsystem.generated.h"

struct FApexRoadState;

/**
 * Client prediction of the local car in a session (docs/game/client.md,
 * docs/proposals/client-prediction.md, phase 3): loads the session's track
 * and the player's car into the linked physics library, runs an
 * FApexOwnCarPredictor on the net subsystem's inputs, snapshots and state
 * blobs, and hands the local car actor the predicted pose to draw.
 *
 * `apexsim.net.Predict` 0 off (the default), 1 on, 2 shadow (the loop runs
 * and logs, the puppet is drawn); `-ApexPredict[=N]` sets it at start.
 * Prediction stays off, and the log says why, without the library, from a
 * server that sends no `SessionJoined.Physics`, when this client has no copy
 * of the track's YAML (or of its road mesh when the server drives on one),
 * or when the YAML's or the car.toml's content checksum is not the server's.
 *
 * The puppet is drawn instead outside the race proper (the countdown, the
 * finish), in the pit lane, the garage and a recovery, while the server
 * drives the car (unless `apexsim.net.PredictServerDriven 1`), and when the
 * snapshots stop. Force feedback, the engine sound and the HUD stay on the
 * server's data.
 */
UCLASS()
class APEXSIM_API UApexPredictionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The car index prediction runs for; -1 when it runs for none. */
	int32 GetLocalCarIndex() const { return LocalCarIndex; }

	/**
	 * The pose to draw `CarIndex` at this frame, Unreal frame: false unless it
	 * is the local car and prediction is on and in use.
	 */
	bool GetRenderPose(int32 CarIndex, FVector& OutLocationCm, FQuat& OutRotation, FVector& OutVelocityCmPerS,
		float& OutSpeedMps, float& OutSteering) const;

	/** Where the puppet (the motion buffer) would draw the local car this frame, cm: the gap prediction closes. */
	void ReportPuppetPose(int32 CarIndex, const FVector& LocationCm, float SpeedMps);

	/** The loop is drawing this frame. */
	bool IsInUse() const { return bInUse; }
	/** Why prediction is not running, or what it is doing. */
	const FString& GetStatus() const { return Status; }
	const FApexOwnCarPredictor& GetPredictor() const { return Predictor; }

private:
	bool Tick(float DeltaSeconds);
	void Teardown(const FString& Why);
	void SetStatus(const FString& NewStatus, bool bWarn);
	/** Reads the files, checks their checksums and starts loading the track; false with Status set when it cannot. */
	bool StartLoad(const FString& Key, const FString& TrackFile, const FString& TrackId, const FString& CarId);
	void CreateCar();
	void HandleRoadState(const FApexRoadState& Road);
	void ApplyRoadState(const FApexRoadState& Road);
	void LogWindow(double NowSeconds, bool bForce);

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle RoadStateHandle;

	FApexOwnCarPredictor Predictor;
	TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe> Track;
	TFuture<TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe>> PendingTrack;
	bool bLoading = false;

	/** What the loaded track and car were made for; empty when nothing is. */
	FString LoadedKey;
	/** A context that could not be predicted: not retried until it changes. */
	FString FailedKey;
	TArray<uint8> CarToml;
	FString CarName;
	FString TrackName;
	FApexCarSetup CarSetup;
	int32 GridPosition = 0;
	int32 TickRateHz = 0;
	int32 RoadContact = 0;

	/** RoadState messages heard before the track loaded, replayed onto it in order. */
	TArray<FApexRoadState> PendingRoad;
	bool bRoadRefused = false;

	int32 LocalCarIndex = -1;
	uint32 LastSentSeq = 0;
	uint32 SnapshotSerial = 0;
	uint32 StateSerial = 0;
	double LastSnapshotSeconds = 0.0;
	bool bInUse = false;
	bool bDrawable = false;
	FString Status;

	/** The log window: frames drawn from the prediction, and the puppet's gap behind it. */
	double WindowStartSeconds = 0.0;
	int32 WindowFrames = 0;
	int32 WindowDrawnFrames = 0;
	double WindowGapSumM = 0.0;
	double WindowGapSumMs = 0.0;
	int32 WindowGapSamples = 0;
	FString LastWhyNotDrawn;
};
