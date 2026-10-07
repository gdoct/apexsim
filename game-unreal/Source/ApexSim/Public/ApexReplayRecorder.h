#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexSpectatorStream.h"
#include "ApexSpectatorWriter.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexReplayRecorder.generated.h"

class UApexNetSubsystem;

/** One replay on disk, as the Replays screen lists it. */
struct APEXSIM_API FApexReplayInfo
{
	FString Path;
	FApexStreamHeader Header;
	int32 Cars = 0;
	/** The first human driver in the roster, or empty for an AI race. */
	FString Driver;
	FDateTime When;
	int64 Bytes = 0;
	/** Kept by the player (Replays/) rather than one of the recent sessions (Replays/Recent/). */
	bool bSaved = false;
};

/**
 * Records every session this client is in (a race, practice, a hotlap, a
 * live race watched) as a spectator stream, and saves it as an `.apxs`
 * replay (docs/SPECTATOR.md, "Replays"). A replay plays back through the
 * watch view like a showcase: any car, any camera, the timing tower.
 *
 * What is recorded is what the client received: every car's telemetry,
 * thinned to every other frame (30 Hz from a 60 Hz broadcast), the roster,
 * the lap timing and the session's state, written as the server's own
 * records (`FApexStreamCarRow::FromTelemetry`) and compressed a second at a
 * time, so an hour of a full grid is tens of megabytes. Time with every car
 * parked in its hotlap garage is cut out.
 *
 * When a session ends (left, finished, disconnected) what was recorded is
 * written to `Saved/Replays/Recent/` (the newest MaxRecent kept); SaveReplay
 * (the pause menu, the hotlap garage, `apexsim.replay.Save`) writes it to
 * `Saved/Replays/` for good, as does Keep for a recent one.
 */
UCLASS()
class APEXSIM_API UApexReplayRecorder : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsRecording() const { return bRecording; }
	/** Seconds of race on record (garage time cut). */
	double GetRecordedSeconds() const;
	bool HasSomethingToSave() const { return GetRecordedSeconds() >= MinSeconds; }

	/**
	 * Write what has been recorded so far to Replays/ under a name made from
	 * the date, the circuit and the mode. Recording goes on. False with a
	 * reason when there is nothing yet or the file could not be written.
	 */
	bool SaveReplay(FString& OutPath, FString& OutError);

	/**
	 * Keep the whole of this session (the results screen's Save replay):
	 * while it is still recording, it will go to Replays/ rather than Recent/
	 * when it ends (bOutWhenFinished); once ended, its recent file moves to
	 * Replays/. False with a reason when there is nothing of it to keep.
	 */
	bool KeepThisSession(FString& OutPath, bool& bOutWhenFinished, FString& OutError);
	/** There is a recording, or a file of the session just ended, to keep. */
	bool CanKeepThisSession() const;
	/** KeepThisSession has been asked for this session (or it was kept). */
	bool IsThisSessionKept() const;

	/** `Saved/Replays` (the kept ones) and its `Recent` folder. */
	static FString ReplayDirectory();
	static FString RecentDirectory();
	/** Every replay on disk, newest first, kept ones before recent ones. */
	static TArray<FApexReplayInfo> ListReplays();
	/** Move a recent replay into the kept ones. */
	static bool KeepReplay(const FString& Path, FString& OutNewPath);
	static bool DeleteReplay(const FString& Path);
	/** "2026-10-03 14.05 Zandervoort Hotlap": a file name from what the replay is. */
	static FString MakeReplayName(const FDateTime& When, const FString& Track, EApexGameMode Mode);

	/** Recent replays kept before the oldest goes. */
	static constexpr int32 MaxRecent = 10;
	/** Shorter than this is not worth a file. */
	static constexpr double MinSeconds = 10.0;
	/** A recording stops growing past this much compressed data. */
	static constexpr int64 MaxBytes = 512ll * 1024 * 1024;

private:
	UFUNCTION() void HandleSessionJoined(const FString& SessionId, int32 GridPosition);
	UFUNCTION() void HandleSessionLeft();
	UFUNCTION() void HandleTelemetry(const FApexTelemetryFrame& Frame);
	UFUNCTION() void HandleRosterUpdated(const FApexSessionRoster& Roster);
	UFUNCTION() void HandleLapTiming(const FApexLapTiming& Timing);
	UFUNCTION() void HandleSessionStateChanged(EApexSessionState NewState);
	UFUNCTION() void HandleDisconnected(const FString& Reason);

	UApexNetSubsystem* GetNet() const;
	/** The server's tick rate, measured from the frames' ticks against their arrival. */
	int32 EstimateTickRate() const;
	/** The net subsystem is in a real session (not the menu's backdrop). */
	bool IsRecordableSession() const;

	void Begin();
	/** Into Recent/, if there is enough, then forget it. */
	void FinishAndKeepRecent();
	/** The file's bytes for what is on record. */
	bool BuildFile(TArray<uint8>& OutBytes, FApexStreamHeader& OutHeader, FString& OutError) const;
	FApexStreamRoster MakeRoster(const FApexSessionRoster& Roster, int32 Revision) const;
	static void PruneRecent();

	bool bRecording = false;
	FApexStreamWriter Writer;
	FString SessionId;
	/** What the session looked like as recording began (the file's preamble). */
	FApexStreamRoster FirstRoster;
	bool bHaveFirstRoster = false;
	int32 RosterRevision = 0;
	bool bRosterChanged = false;
	FApexSessionRoster LatestRoster;

	/** Ticks cut out (garage time), taken off every tick written after. */
	int64 TickOffset = 0;
	int64 FirstTick = -1;
	int64 LastTick = -1;
	/** The server tick of the last frame received, before the cut. */
	int64 LastServerTick = -1;
	int64 RaceStartTick = -1;
	double FirstSeconds = 0.0;
	double LastSeconds = 0.0;
	int64 FirstServerTick = -1;
	int64 LastSeenServerTick = -1;
	int32 FramesSeen = 0;
	int32 FramesWritten = 0;
	EApexSessionState LastState = EApexSessionState::Lobby;
	EApexGameMode GameMode = EApexGameMode::Lobby;
	TSet<int32> Finished;
	bool bStoppedForSize = false;
	/** Write the session to Replays/ instead of Recent/ when it ends. */
	bool bKeepWhenFinished = false;
	/** Where the last session's file went when it ended. */
	FString LastSessionPath;
};
