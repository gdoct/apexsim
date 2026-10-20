#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Race/ApexOwnCarPredictor.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexPredictionSubsystem.generated.h"

struct FApexRoadState;
struct FApexTrackBundleChunk;

/**
 * Fetches a track's physics bundle a chunk at a time (RequestTrackBundle), a
 * few in flight and paced under the server's control-message rate, and puts
 * it back together. Pure over a send function and the chunks heard, so tests
 * drive it without a server.
 */
struct APEXSIM_API FApexTrackBundleDownload
{
	/** Chunks asked for and not yet heard. */
	static constexpr int32 MaxInFlight = 3;
	/** Requests a second, and the burst: the server allows 10 control messages a second, 20 at once. */
	static constexpr double RequestsPerSecond = 6.0;
	static constexpr double RequestBurst = 10.0;
	/** No chunk for this long: the download fails. */
	static constexpr double StallSeconds = 10.0;

	uint32 Crc = 0;
	int32 Chunks = 0;
	int64 ExpectedBytes = 0;
	double StartSeconds = 0.0;
	double LastHeardSeconds = 0.0;
	TArray<TArray<uint8>> Received;
	int32 NextToAsk = 0;
	int32 InFlight = 0;
	int32 Heard = 0;
	double Tokens = RequestBurst;
	double TokensSeconds = 0.0;
	FString Failure;

	void Begin(uint32 InCrc, int32 InChunks, int64 InBytes, double NowSeconds);
	/** Asks for what the pacing allows; Send(Crc, Chunk) sends one request. A stall fails it. */
	void Pump(double NowSeconds, TFunctionRef<void(uint32, int32)> Send);
	/** A chunk heard (another bundle's is ignored); false when it ended the download in failure. */
	bool OnChunk(const FApexTrackBundleChunk& Chunk, double NowSeconds);
	bool IsActive() const { return Chunks > 0 && !IsDone() && !HasFailed(); }
	bool IsDone() const { return Chunks > 0 && Heard == Chunks && Failure.IsEmpty(); }
	bool HasFailed() const { return !Failure.IsEmpty(); }
	/** The bundle's bytes, in order; empty unless done and the size is the one named. */
	TArray<uint8> Join() const;
};

/** One file in the track bundle cache. */
struct FApexCachedBundle
{
	FString Path;
	int64 Bytes = 0;
	/** When it was last saved or loaded (its file time). */
	FDateTime Used;
};

/**
 * The track bundle cache (Saved/ApexSim/TrackBundles) kept under a size:
 * every track version a server handed over is a file (2.6 MB for Monza,
 * 10 MB for the Nordschleife), so the least recently used go first.
 */
namespace ApexTrackBundleCache
{
	inline constexpr int64 MaxBytes = 64ll * 1024 * 1024;

	/** The files to delete so the rest fit in MaxCacheBytes: the least recently used first, never Keep. Pure for the tests. */
	APEXSIM_API TArray<FString> ToPrune(TArray<FApexCachedBundle> Files, const FString& Keep, int64 MaxCacheBytes = MaxBytes);
	/** Deletes from the cache folder what ToPrune names, keeping Keep (the bundle just saved). */
	APEXSIM_API void Prune(const FString& Keep);
}

/**
 * Client prediction of the local car in a session (docs/game/client.md,
 * docs/proposals/client-prediction.md, phase 3): loads the session's track
 * and the player's car into the linked physics library, runs an
 * FApexOwnCarPredictor on the net subsystem's inputs, snapshots and state
 * blobs, and hands the local car actor the predicted pose to draw.
 *
 * `apexsim.net.Predict` 0 off (the default), 1 on, 2 shadow (the loop runs
 * and logs, the puppet is drawn); `-ApexPredict[=N]` sets it at start.
 * The track is this client's own files when their bundle checksum is the
 * server's (`SessionJoined.Physics.BundleCrc`), else the server's bundle,
 * cached under Saved/ApexSim/TrackBundles or fetched a chunk at a time
 * (FApexTrackBundleDownload); from an older server, this client's YAML when
 * its content checksum is the lobby's. Prediction stays off, and the log
 * says why, without the library, from a server that sends no Physics, when
 * the car.toml is missing or not the server's, or when the track cannot be
 * had.
 *
 * The puppet is drawn instead outside the race proper (the countdown, the
 * finish), in the pit lane, the garage and a recovery, while the server
 * drives the car (unless `apexsim.net.PredictServerDriven 1`), and when the
 * snapshots stop. While the prediction is drawn, the force feedback, the
 * road and engine sound and the HUD's dash read it (GetOwnFeel, GetOwnCar);
 * otherwise they read the server's data as they always did.
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

	/**
	 * The predicted car as the dash, the engine sound and the HUD read it
	 * (revs blended across the sub-tick, the newest throttle, brake and gear),
	 * in the SERVER frame: false unless prediction is drawing the local car,
	 * when every reader falls back to the server's own figures.
	 */
	bool GetOwnCar(FApexPredictedPose& Out) const;

	/**
	 * What the driver feels from the predicted car: every tick's feedback the
	 * clock reached since the last frame, from the library (the server's own
	 * collector) folded as the net subsystem folds DriverFeedback messages.
	 * Its serial moves when a frame brought new ticks (as
	 * UApexNetSubsystem::GetDriverFeedbackSerial does with messages) and
	 * OutSeconds is when. False unless prediction is drawing the local car:
	 * the force feedback and the road sound then read the server's.
	 */
	bool GetOwnFeel(const FApexDriverFeedback*& OutFeel, uint32& OutSerial, double& OutSeconds) const;

	/** Where the puppet (the motion buffer) would draw the local car this frame, cm: the gap prediction closes. */
	void ReportPuppetPose(int32 CarIndex, const FVector& LocationCm, float SpeedMps);

	/** The loop is drawing this frame. */
	bool IsInUse() const { return bInUse; }
	/** Why prediction is not running, or what it is doing. */
	const FString& GetStatus() const { return Status; }
	const FApexOwnCarPredictor& GetPredictor() const { return Predictor; }

	/** Where a track's physics bundle is kept once it came: Saved/ApexSim/TrackBundles/<CRC>.apxtrack. */
	static FString BundleCachePath(uint32 Crc);

private:
	bool Tick(float DeltaSeconds);
	void Teardown(const FString& Why);
	void SetStatus(const FString& NewStatus, bool bWarn);
	/** Reads the files, checks their checksums and starts loading the track; false with Status set when it cannot. */
	bool StartLoad(const FString& Key, const FString& TrackFile, const FString& TrackId, const FString& CarId);
	/** The track from the bundle's bytes (just fetched), off the game thread. */
	void StartBundleLoad(TArray<uint8> Bundle);
	void CreateCar();
	void HandleRoadState(const FApexRoadState& Road);
	void ApplyRoadState(const FApexRoadState& Road);
	void HandleTrackBundleChunk(const FApexTrackBundleChunk& Chunk);
	/** The feel and revs the frame brought, and how far they lead the server's (the log window's figures). */
	void UpdateFeel(double NowSeconds);
	void LogWindow(double NowSeconds, bool bForce);

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle RoadStateHandle;
	FDelegateHandle TrackBundleHandle;

	/** What a track load gives back: the track, or that the bundle has to be fetched first. */
	struct FLoadResult
	{
		TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe> Track;
		bool bNeedBundle = false;
		/** Where it came from, for the log: this client's files, the cache, the bundle just fetched. */
		FString Source;
	};

	FApexOwnCarPredictor Predictor;
	TSharedPtr<ApexPhysics::FTrack, ESPMode::ThreadSafe> Track;
	TFuture<FLoadResult> PendingTrack;
	bool bLoading = false;

	/** The track's bundle being fetched. */
	FApexTrackBundleDownload Download;
	bool bDownloading = false;
	/** The session's bundle checksum and size (SessionJoined.Physics). */
	uint32 BundleCrc = 0;
	int64 BundleBytes = 0;
	int32 BundleChunks = 0;

	/** The feel of the predicted car, the last tick it reached, and its serial and time. */
	FApexDriverFeedback OwnFeel;
	int64 FeltTick = -1;
	uint32 OwnFeelSerial = 0;
	double OwnFeelSeconds = 0.0;
	uint32 LastServerFeelSerial = 0;
	uint32 LastServerSnapshotSerial = 0;
	/** The log window: how far the predicted feel and revs led the server's, and how far they were off it. */
	double WindowFeelLeadMs = 0.0;
	double WindowFeelTorqueError = 0.0;
	int32 WindowFeelSamples = 0;
	double WindowRpmLeadMs = 0.0;
	double WindowRpmError = 0.0;
	int32 WindowRpmSamples = 0;

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
	/** The car as the server balances it (SessionJoined.Physics), when it does. */
	TOptional<ApxBalance> Balance;

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
