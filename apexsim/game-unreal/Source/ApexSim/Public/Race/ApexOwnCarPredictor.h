#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexUdpConnection.h"
#include "Race/ApexPhysicsLib.h"

/** The predicted car as it is drawn, in the SERVER frame and units. */
struct FApexPredictedPose
{
	FVector PositionM = FVector::ZeroVector;
	float YawRad = 0.0f;
	float PitchRad = 0.0f;
	float RollRad = 0.0f;
	FVector VelocityMps = FVector::ZeroVector;
	float SpeedMps = 0.0f;
	/** -1..1, the server's sign (positive left), as the car applied it. */
	float Steering = 0.0f;
	/** Blended across the sub-tick, as the motion buffer blends the puppet's. */
	float EngineRpm = 0.0f;
	int32 Gear = 0;
	/** 0..1 as the car applied them, the newest tick's (a lift is heard at once). */
	float Throttle = 0.0f;
	float Brake = 0.0f;
	/** The tick the pose is drawn up to (the head). */
	int64 Tick = -1;
};

/** What the loop did over a stretch of time (UApexPredictionSubsystem logs a window of it). */
struct APEXSIM_API FApexPredictionStats
{
	int32 Snapshots = 0;
	int32 Rewinds = 0;
	int32 BlobLoads = 0;
	/** Corrections past the re-seat distance, and seats on a server state the timeline did not reach. */
	int32 Reseats = 0;
	/** Times the clock jumped to its target instead of slewing to it. */
	int32 ClockSnaps = 0;
	/** Snapshots older than the ring holds. */
	int32 Stale = 0;
	int64 ReplayTicks = 0;
	int32 MaxReplayTicks = 0;
	double LeadTicksSum = 0.0;
	int32 LeadSamples = 0;
	/** Each rewind's change to the predicted pose at the head, mm. */
	TArray<float> CorrectionsMm;

	float MeanReplayTicks() const { return Rewinds > 0 ? static_cast<float>(ReplayTicks) / Rewinds : 0.0f; }
	float MeanLeadTicks() const { return LeadSamples > 0 ? static_cast<float>(LeadTicksSum / LeadSamples) : 0.0f; }
	float MeanCorrectionMm() const;
	/** The P-th percentile (0..1) of the corrections, mm. */
	float CorrectionPercentileMm(float P) const;
	float MaxCorrectionMm() const;
	FString Describe(int32 TickRateHz) const;
};

/**
 * Client prediction of the local car (docs/proposals/client-prediction.md,
 * docs/game/client.md): the server's own car step (ApexPhysics), stepped
 * ahead of the server by the round trip, reconciled with every state the
 * server sends back.
 *
 * - The timeline is in **server ticks**. The car is stepped one tick at a
 *   time at the session's rate with the tick numbers the server uses, so a
 *   replay steps exactly as the server did. A ring keeps each tick's input
 *   and pose, and the full state at the ticks a snapshot can land on.
 * - Inputs are the ones the UDP worker numbered and sent. Each is laid on
 *   the timeline at the tick the client's clock read when it was sent, and
 *   drives every tick until the next: the server holds an input the same
 *   way, and the clock runs a round trip ahead so that tick is the one the
 *   server applies it at.
 * - The clock: the server's tick now is estimated from the snapshots'
 *   ticks and arrival times (the earliest-arriving of a sliding window), the
 *   round trip from each snapshot's input number against when that input
 *   was sent (the shortest of the window). The clock runs at the session's
 *   rate, slews toward that target by at most a few percent, and jumps when
 *   it is far off.
 * - A snapshot (`DriverFeedback.own_car`) is laid over the client's own
 *   state of its tick and the inputs since are replayed to the head; a whole
 *   state blob (`own_state`) is loaded as the state of its tick and replayed
 *   the same way. The newest snapshot is laid again whenever a replay
 *   crosses its tick. The fields the session's passes own (pit lane, DRS
 *   allowed, headlights, wake) stay as the newest blob had them.
 * - The pose drawn is the head blended back by the sub-tick remainder, plus
 *   a correction offset: what a rewind moved the head by, decayed over a
 *   tenth of a second. A move past the re-seat distance snaps.
 * - Server-driven (a snapshot naming no input, or the same input for longer
 *   than a moment while newer ones go out: the stand-in, an away driver,
 *   the cool-down lap): the replay drives with the controls the server last
 *   applied, held. The subsystem draws the puppet then unless told otherwise.
 *
 * Pure logic over the library and plain values: the clock is the caller's,
 * so tests drive it with a synthetic stream.
 */
class APEXSIM_API FApexOwnCarPredictor
{
public:
	struct FSettings
	{
		/** The visible correction's decay time constant, s. */
		float CorrectionSeconds = 0.1f;
		/** A rewind that moves the head further than this snaps instead of blending (the motion buffer's 20 m). */
		float ReseatDistanceM = 20.0f;
		/** How far back the ring reaches, s: the deepest rewind. */
		float RingSeconds = 1.5f;
		/** The clock jumps to its target when further off than this, s. */
		float ClockSnapSeconds = 0.25f;
		/** The clock's rate is trimmed by at most this fraction. */
		float ClockSlew = 0.05f;
		/** Time constant of the clock's trim, s. */
		float ClockGainSeconds = 0.5f;
		/** Ticks ahead of the measured round trip: the input's wait for the server's next tick. */
		float LeadMarginTicks = 0.5f;
		/** The window the round trip's minimum and the server clock's earliest arrival are taken over, s. */
		float TimingWindowSeconds = 2.0f;
		/**
		 * How much of the gap between the input the server held at a
		 * snapshot's tick and the one this timeline held goes into the
		 * clock's target each snapshot, and the most it can add up to, ticks.
		 */
		float AlignGain = 0.25f;
		float MaxAlignTicks = 14.0f;
		/** The same input number for this long while newer go out: the server is driving, s. */
		float ServerDrivenSeconds = 0.3f;
	};

	FApexOwnCarPredictor() = default;
	explicit FApexOwnCarPredictor(const FSettings& InSettings) : Settings(InSettings) {}

	/** Starts a timeline for a car on a track (the track is the caller's, the car becomes ours). */
	void Begin(ApexPhysics::FTrack* InTrack, TUniquePtr<ApexPhysics::FCar> InCar, int32 InTickRateHz);
	/** Drops the car and the timeline. */
	void End();

	bool IsRunning() const { return Car.IsValid() && Track != nullptr && TickRateHz > 0; }
	/** A server state has seated the timeline: there is a pose to draw. */
	bool IsSeated() const { return bSeated; }
	/** The server, not this client's inputs, drives the car (see the class comment). */
	bool IsServerDriven() const { return bServerDriven; }

	/** The inputs sent since the last call (FApexUdpConnection::CopySentInputs), oldest first. */
	void AddSentInputs(TArrayView<const FApexSentInput> Sent, double NowSeconds);
	/** A whole state blob of the own car at `Tick` (`DriverFeedback.own_state`). */
	void OnStateBlob(int64 Tick, TArrayView<const uint8> Blob, double NowSeconds);
	/**
	 * A snapshot of the own car (`DriverFeedback.own_car`), and when its
	 * datagram came in (the UDP worker's time, which the clock is timed by).
	 */
	void OnSnapshot(const FApexOwnCarSnapshot& Snapshot, double ArrivalSeconds);
	/** Reconciles what came in, moves the clock and steps the car to it. */
	void Advance(double NowSeconds, double DeltaSeconds);

	/** The pose to draw: the head at the clock, with the correction offset. False until seated. */
	bool GetPose(FApexPredictedPose& Out) const;
	/** The same without the correction offset. */
	bool GetRawPose(FApexPredictedPose& Out) const;
	/** The car as predicted now at a tick the ring holds (the library's pose after that tick). */
	bool GetPoseAtTick(int64 Tick, ApxCarPose& Out) const;

	/**
	 * What the driver feels over the ticks after `AfterTick` up to `UpToTick`
	 * (the head): each tick's feedback from the library (the server's own
	 * collector, apx_car_feedback_take) folded together as the net subsystem
	 * folds a frame's DriverFeedback messages (FApexDriverFeedback::Absorb).
	 * Each tick is marked felt at `NowSeconds` the first time, with its torque
	 * and revs, for GetFelt. Returns how many ticks went in.
	 */
	int32 CollectFeel(int64 AfterTick, int64 UpToTick, double NowSeconds, FApexDriverFeedback& Out);

	/** When a tick was first felt (CollectFeel), and its torque (the server's sign) and revs then; false if never. */
	bool GetFelt(int64 Tick, double& OutSeconds, float& OutTorque, float& OutRpm) const;

	/** The round trip to fall back on while the server drives (no input numbers to measure it by), s. */
	void SetFallbackRttSeconds(double Seconds) { FallbackRttSeconds = FMath::Max(0.0, Seconds); }
	/** Only what happens while counting reaches the stats (prediction in use, not the pit lane's puppet). */
	void SetCounting(bool bInCounting) { bCounting = bInCounting; }

	const FApexPredictionStats& GetStats() const { return Stats; }
	/** The stats since the last call, and a fresh window. */
	FApexPredictionStats TakeStats();

	int64 GetHeadTick() const { return Head; }
	double GetClockTick() const { return Clock; }
	/** The clock's lead over the server's estimated tick now, ticks. */
	double GetLeadTicks(double NowSeconds) const;
	/** The measured round trip (the window's minimum), s; negative before one is measured. */
	double GetRttSeconds() const { return RttSeconds; }
	/** The clock target's trim from the input alignment, ticks (positive: later). */
	double GetAlignTicks() const { return AlignTicks; }
	int32 GetTickRateHz() const { return TickRateHz; }
	/** The visible correction left, m. */
	float GetCorrectionOffsetM() const { return static_cast<float>(OffsetPos.Size()); }

private:
	struct FEntry
	{
		int64 Tick = -1;
		ApxInput Input = {};
		uint32 Seq = 0;
		ApxCarPose Pose = {};
		/** The full state after this tick, at the ticks a snapshot can land on. */
		TArray<uint8> State;
		bool bHasState = false;
		/** State is the server's own blob: a replay loads it rather than stepping over it. */
		bool bExact = false;
		/** What the step of this tick gave the driver to feel (apx_car_feedback_take); none for a loaded state. */
		ApxFeedback Feedback = {};
		bool bHasFeedback = false;
		/** When this tick was first felt (CollectFeel), and its torque and revs then; -1 never. Kept across replays of the tick. */
		double FeltSeconds = -1.0;
		float FeltTorque = 0.0f;
		float FeltRpm = 0.0f;
	};

	struct FInputRecord
	{
		uint32 Seq = 0;
		ApxInput Input = {};
		double SentSeconds = 0.0;
		/** The clock when it was sent: the tick it drives from. */
		double FromTick = 0.0;
		/** FromTick was worked out on a seated timeline. */
		bool bPlaced = false;
	};

	struct FTimingSample
	{
		double Seconds = 0.0;
		double Value = 0.0;
	};

	FSettings Settings;
	ApexPhysics::FTrack* Track = nullptr;
	TUniquePtr<ApexPhysics::FCar> Car;
	int32 TickRateHz = 0;

	TArray<FEntry> Ring;
	bool bSeated = false;
	int64 Head = -1;
	/** The first tick of this timeline: nothing before it is in the ring. */
	int64 SeatTick = -1;
	double Clock = 0.0;
	/** The oldest tick a pending server state (or an input heard late) asks to replay from; -1 none. */
	int64 DirtyFrom = -1;
	/** The pending replay carries a server state: it is a correction, counted as one. */
	bool bServerCorrection = false;
	/** The wall time the clock was last worked out for. */
	double ClockSeconds = 0.0;

	TArray<FInputRecord> Inputs;
	ApxInput LastInput = {};
	uint32 LastSeq = 0;

	bool bHaveSnapshot = false;
	FApexOwnCarSnapshot Latest;
	ApxCarSnapshot LatestApx = {};
	int64 SnapshotSpacing = 1;
	int64 PreviousSnapshotTick = -1;
	int32 SpacingAge = 0;

	int64 LastSnapshotSeq = -1;
	double LastSeqChangeSeconds = 0.0;
	bool bServerDriven = false;

	TArray<FTimingSample> ServerClock;
	TArray<FTimingSample> RoundTrips;
	double RttSeconds = -1.0;
	/** The clock target's trim from the input alignment (AlignInputs), ticks. */
	double AlignTicks = 0.0;
	double FallbackRttSeconds = 0.0;
	bool bHaveServerClock = false;

	FVector OffsetPos = FVector::ZeroVector;
	FVector OffsetAttitude = FVector::ZeroVector;

	bool bCounting = true;
	FApexPredictionStats Stats;

	FEntry& EntryAt(int64 Tick) { return Ring[static_cast<int32>(Tick % Ring.Num())]; }
	const FEntry* FindEntry(int64 Tick) const;
	int64 OldestTick() const;
	bool IsSaveTick(int64 Tick) const;
	double TargetClock(double NowSeconds) const;
	double ServerTickNow(double NowSeconds) const;
	ApxInput InputForTick(int64 Tick, uint32& OutSeq) const;
	ApxInput HeldServerInput() const;
	/**
	 * Lays the inputs on the timeline by when they were sent: the new ones,
	 * or `bAll` after the clock was set. A new one that belongs on ticks
	 * already stepped is written into them and they are stepped again.
	 */
	void PlaceInputs(bool bAll);
	/** Trims the clock's target by which input the server held at a snapshot's tick against the ring's. */
	void AlignInputs(const FApexOwnCarSnapshot& Snapshot);
	/** Steps tick `Tick` (Head + 1) and records it; `bReplay` keeps the ring's input. */
	bool StepTick(int64 Tick, bool bReplay);
	/** Records the car's pose (and state at a save tick) as tick `Tick`'s. */
	void Record(FEntry& Entry, int64 Tick);
	void Seat(int64 Tick, TArrayView<const uint8> Blob, bool bFromBlob, double NowSeconds);
	void Reconcile();
	void Trim(TArray<FTimingSample>& Samples, double NowSeconds) const;
};
