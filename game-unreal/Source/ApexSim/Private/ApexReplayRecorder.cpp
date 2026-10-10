#include "ApexReplayRecorder.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	FAutoConsoleCommandWithWorld ReplaySaveCommand(
		TEXT("apexsim.replay.Save"),
		TEXT("Save the session being recorded as a replay (Saved/Replays)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
			UApexReplayRecorder* Recorder = GameInstance ? GameInstance->GetSubsystem<UApexReplayRecorder>() : nullptr;
			FString Path;
			FString Error;
			if (Recorder && Recorder->SaveReplay(Path, Error))
			{
				UE_LOG(LogApexSim, Display, TEXT("apexsim.replay.Save: %s"), *Path);
			}
			else
			{
				UE_LOG(LogApexSim, Warning, TEXT("apexsim.replay.Save: %s"), Recorder ? *Error : TEXT("no recorder"));
			}
		}));

	FAutoConsoleCommandWithWorld ReplayListCommand(
		TEXT("apexsim.replay.List"),
		TEXT("List the replays on disk."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld*)
		{
			for (const FApexReplayInfo& Info : UApexReplayRecorder::ListReplays())
			{
				UE_LOG(LogApexSim, Display, TEXT("%s  %s  %.0f s  %d car(s)  %s"), Info.bSaved ? TEXT("saved ") : TEXT("recent"),
					*Info.Header.Track.DisplayName, Info.Header.DurationSeconds(), Info.Cars, *Info.Path);
			}
		}));

	/** Every other frame of a 60 Hz broadcast: 30 Hz, the showcases' rate. */
	constexpr int32 FrameStride = 2;

	/** `tracks/default/Zandvoort/Zandvoort.yaml` -> `Zandvoort`. */
	FString StemOfTrackFile(const FString& TrackFile)
	{
		return FPaths::GetBaseFilename(TrackFile);
	}

	/** The usual server rates, so an estimate off by an arrival's jitter lands on the real one. */
	int32 SnapTickRate(double Estimate)
	{
		static const int32 Rates[] = {30, 60, 120, 240, 420, 480};
		for (const int32 Rate : Rates)
		{
			if (FMath::Abs(Estimate - Rate) <= Rate * 0.12)
			{
				return Rate;
			}
		}
		return Estimate > 1.0 ? FMath::RoundToInt(Estimate) : 420;
	}

	const TCHAR* ModeWord(EApexGameMode Mode)
	{
		switch (Mode)
		{
		case EApexGameMode::Hotlap: return TEXT("Hotlap");
		case EApexGameMode::FreePractice: return TEXT("Practice");
		case EApexGameMode::Qualification: return TEXT("Qualifying");
		case EApexGameMode::Race: return TEXT("Race");
		default: return TEXT("Session");
		}
	}
}

void UApexReplayRecorder::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSessionJoined.AddDynamic(this, &UApexReplayRecorder::HandleSessionJoined);
		Net->OnSessionLeft.AddDynamic(this, &UApexReplayRecorder::HandleSessionLeft);
		Net->OnTelemetry.AddDynamic(this, &UApexReplayRecorder::HandleTelemetry);
		Net->OnSessionRosterUpdated.AddDynamic(this, &UApexReplayRecorder::HandleRosterUpdated);
		Net->OnLapTiming.AddDynamic(this, &UApexReplayRecorder::HandleLapTiming);
		Net->OnSessionStateChanged.AddDynamic(this, &UApexReplayRecorder::HandleSessionStateChanged);
		Net->OnDisconnected.AddDynamic(this, &UApexReplayRecorder::HandleDisconnected);
		RoadStateHandle = Net->OnRoadState.AddUObject(this, &UApexReplayRecorder::HandleRoadState);
	}
}

void UApexReplayRecorder::Deinitialize()
{
	// A game closed mid-session keeps its session among the recent ones.
	FinishAndKeepRecent();
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSessionJoined.RemoveDynamic(this, &UApexReplayRecorder::HandleSessionJoined);
		Net->OnSessionLeft.RemoveDynamic(this, &UApexReplayRecorder::HandleSessionLeft);
		Net->OnTelemetry.RemoveDynamic(this, &UApexReplayRecorder::HandleTelemetry);
		Net->OnSessionRosterUpdated.RemoveDynamic(this, &UApexReplayRecorder::HandleRosterUpdated);
		Net->OnLapTiming.RemoveDynamic(this, &UApexReplayRecorder::HandleLapTiming);
		Net->OnSessionStateChanged.RemoveDynamic(this, &UApexReplayRecorder::HandleSessionStateChanged);
		Net->OnDisconnected.RemoveDynamic(this, &UApexReplayRecorder::HandleDisconnected);
		Net->OnRoadState.Remove(RoadStateHandle);
	}
	Super::Deinitialize();
}

UApexNetSubsystem* UApexReplayRecorder::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

bool UApexReplayRecorder::IsRecordableSession() const
{
	const UApexNetSubsystem* Net = GetNet();
	return Net && Net->IsInSession() && !Net->IsInDemoSession();
}

int32 UApexReplayRecorder::EstimateTickRate() const
{
	// The wire does not say the server's rate: its ticks against the clock do.
	const double Seconds = LastSeconds - FirstSeconds;
	return Seconds > 1.0 ? SnapTickRate((LastSeenServerTick - FirstServerTick) / Seconds) : 420;
}

double UApexReplayRecorder::GetRecordedSeconds() const
{
	return FirstTick >= 0 && LastTick > FirstTick ? static_cast<double>(LastTick - FirstTick) / EstimateTickRate() : 0.0;
}

// --- Recording ----------------------------------------------------------------------

void UApexReplayRecorder::Begin()
{
	Writer.Reset();
	bRecording = true;
	bKeepWhenFinished = false;
	LastSessionPath.Reset();
	bHaveFirstRoster = false;
	RosterRevision = 0;
	bRosterChanged = false;
	TickOffset = 0;
	FirstTick = -1;
	LastTick = -1;
	LastServerTick = -1;
	RaceStartTick = -1;
	FirstServerTick = -1;
	LastSeenServerTick = -1;
	FramesSeen = 0;
	FramesWritten = 0;
	LastState = EApexSessionState::Lobby;
	GameMode = EApexGameMode::Lobby;
	Finished.Reset();
	bStoppedForSize = false;
	PendingRoads.Reset();
	if (const UApexNetSubsystem* Net = GetNet())
	{
		SessionId = Net->GetCurrentSessionId();
		LatestRoster = Net->GetSessionRoster();
	}
}

void UApexReplayRecorder::HandleSessionJoined(const FString& InSessionId, int32 GridPosition)
{
	// A session joined straight from another keeps the last one first.
	FinishAndKeepRecent();
	Begin();
}

void UApexReplayRecorder::HandleSessionLeft()
{
	FinishAndKeepRecent();
}

void UApexReplayRecorder::HandleDisconnected(const FString& Reason)
{
	FinishAndKeepRecent();
}

void UApexReplayRecorder::HandleSessionStateChanged(EApexSessionState NewState)
{
	// A finished race is complete: on record as soon as it is, in case the
	// player quits from the results.
	if (NewState == EApexSessionState::Finished && bRecording)
	{
		FinishAndKeepRecent();
	}
}

void UApexReplayRecorder::HandleRosterUpdated(const FApexSessionRoster& Roster)
{
	if (!bRecording || !IsRecordableSession())
	{
		return;
	}
	LatestRoster = Roster;
	bRosterChanged = bHaveFirstRoster;
}

void UApexReplayRecorder::HandleTelemetry(const FApexTelemetryFrame& Frame)
{
	if (!bRecording || !IsRecordableSession() || Frame.Cars.Num() == 0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (FirstServerTick < 0)
	{
		FirstServerTick = Frame.ServerTick;
		FirstSeconds = Now;
	}
	LastSeenServerTick = Frame.ServerTick;
	LastSeconds = Now;

	// A hotlap's garage is the player tuning a parked car: cut it, so the
	// replay is the running.
	const bool bAllParked = !Frame.Cars.ContainsByPredicate([](const FApexCarTelemetry& Car) { return !Car.bInGarage; });
	if (bAllParked)
	{
		if (LastServerTick >= 0)
		{
			TickOffset += Frame.ServerTick - LastServerTick;
		}
		LastServerTick = Frame.ServerTick;
		return;
	}
	if (LastServerTick >= 0 && Frame.ServerTick < LastServerTick)
	{
		// A tick that runs back is a new session's clock; nothing to cut.
		LastServerTick = Frame.ServerTick;
		return;
	}
	LastServerTick = Frame.ServerTick;

	if (++FramesSeen % FrameStride != 0 && LastTick >= 0)
	{
		return;
	}
	if (Writer.StoredBytes() > MaxBytes)
	{
		if (!bStoppedForSize)
		{
			bStoppedForSize = true;
			UE_LOG(LogApexSim, Warning, TEXT("Replay: the recording has reached %lld MB; the rest of the session is not recorded"),
				MaxBytes / (1024 * 1024));
		}
		return;
	}

	const int64 Tick = Frame.ServerTick - TickOffset;
	if (FirstTick < 0)
	{
		FirstTick = Tick;
	}
	LastTick = Tick;
	if (Frame.GameMode != EApexGameMode::Lobby && Frame.GameMode != EApexGameMode::Countdown)
	{
		GameMode = Frame.GameMode;
	}

	if (!bHaveFirstRoster)
	{
		LatestRoster = GetNet()->GetSessionRoster();
		FirstRoster = MakeRoster(LatestRoster, 0);
		bHaveFirstRoster = true;
	}
	else if (bRosterChanged)
	{
		bRosterChanged = false;
		Writer.Add(Tick, ApexSpectator::EncodeRoster(MakeRoster(LatestRoster, ++RosterRevision)));
	}

	if (Frame.SessionState != LastState)
	{
		if (Frame.SessionState == EApexSessionState::Racing && RaceStartTick < 0)
		{
			RaceStartTick = Tick;
		}
		FApexStreamEvent State;
		State.Tick = Tick;
		State.Kind = ApexSpectator::EventSessionState;
		State.State = Frame.SessionState;
		Writer.Add(Tick, ApexSpectator::EncodeEvent(State));
		LastState = Frame.SessionState;
	}

	FApexStreamFrame Out;
	Out.Tick = Tick;
	Out.RosterRevision = RosterRevision;
	Out.State = Frame.SessionState;
	Out.CountdownMs = Frame.SessionState == EApexSessionState::Countdown ? Frame.CountdownMs : -1;
	Out.Rows.Reserve(Frame.Cars.Num() * ApexSpectator::RowSize);
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		FApexStreamCarRow::FromTelemetry(Car).Write(Out.Rows);
		if (Car.FinishPosition > 0 && !Finished.Contains(Car.CarIndex))
		{
			Finished.Add(Car.CarIndex);
			FApexStreamEvent Finish;
			Finish.Tick = Tick;
			Finish.Kind = ApexSpectator::EventFinish;
			Finish.CarIndex = Car.CarIndex;
			Finish.Value = Car.FinishPosition;
			Writer.Add(Tick, ApexSpectator::EncodeEvent(Finish));
		}
	}
	Writer.Add(Tick, ApexSpectator::EncodeFrame(Out));
	++FramesWritten;
	// The join's road, filed under the first frame it can be.
	for (const FApexRoadState& Road : PendingRoads)
	{
		WriteRoad(Tick, Road);
	}
	PendingRoads.Reset();
}

void UApexReplayRecorder::HandleRoadState(const FApexRoadState& Road)
{
	if (!bRecording || !IsRecordableSession())
	{
		return;
	}
	if (LastTick < 0)
	{
		// A lap's burst is a handful of messages; more than this is not a burst.
		if (PendingRoads.Num() < 64)
		{
			PendingRoads.Add(Road);
		}
		return;
	}
	WriteRoad(LastTick, Road);
}

void UApexReplayRecorder::WriteRoad(int64 Tick, const FApexRoadState& Road)
{
	FApexStreamRoad Record;
	Record.Tick = Tick;
	Record.Road = Road;
	// A stream's road is its own, not a live session's.
	Record.Road.SessionId.Reset();
	Writer.Add(Tick, ApexSpectator::EncodeRoad(Record));
}

void UApexReplayRecorder::HandleLapTiming(const FApexLapTiming& Timing)
{
	if (!bRecording || !IsRecordableSession() || LastTick < 0)
	{
		return;
	}
	FApexStreamEvent Event;
	Event.Tick = LastTick;
	Event.Kind = ApexSpectator::EventLapTiming;
	Event.LapTiming = Timing;
	Writer.Add(LastTick, ApexSpectator::EncodeEvent(Event));
}

FApexStreamRoster UApexReplayRecorder::MakeRoster(const FApexSessionRoster& Roster, int32 Revision) const
{
	const UApexNetSubsystem* Net = GetNet();
	FApexStreamRoster Out;
	Out.Revision = Revision;
	for (const FApexRosterEntry& Entry : Roster.Entries)
	{
		FApexStreamRosterEntry& Row = Out.Entries.AddDefaulted_GetRef();
		Row.CarIndex = Entry.CarIndex;
		Row.CarConfigId = Entry.CarConfigId;
		Row.Livery = Entry.Livery;
		Row.Name = Entry.PlayerName;
		Row.bIsAi = Entry.bIsAi;
		if (Net)
		{
			if (const FApexCarConfigSummary* Car = Net->GetCachedLobbyState().CarConfigs.FindByPredicate(
					[&Entry](const FApexCarConfigSummary& C) { return C.Id.Equals(Entry.CarConfigId, ESearchCase::IgnoreCase); }))
			{
				Row.ContentCrc = static_cast<uint32>(Car->ContentCrc);
			}
		}
	}
	return Out;
}

// --- Files ----------------------------------------------------------------------------

bool UApexReplayRecorder::BuildFile(TArray<uint8>& OutBytes, FApexStreamHeader& OutHeader, FString& OutError) const
{
	if (!bHaveFirstRoster || FramesWritten == 0 || GetRecordedSeconds() < MinSeconds)
	{
		OutError = TEXT("nothing worth a replay recorded yet");
		return false;
	}
	const UApexNetSubsystem* Net = GetNet();
	const UGameInstance* GameInstance = GetGameInstance();
	const UApexMenuFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;

	FApexStreamHeader& H = OutHeader;
	H = FApexStreamHeader();
	H.Version = ApexSpectator::FormatVersion;
	H.StreamId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	H.TickRate = EstimateTickRate();
	H.FrameRate = FMath::Max(1, FMath::RoundToInt(FramesWritten / FMath::Max(1.0, static_cast<double>(LastTick - FirstTick) / H.TickRate)));
	H.RowSize = ApexSpectator::RowSize;
	H.Conditions = Net ? Net->GetSessionConditions() : FApexSessionConditions();
	H.GameMode = GameMode == EApexGameMode::Lobby ? EApexGameMode::Race : GameMode;
	H.RaceStartTick = RaceStartTick;
	H.StartTick = FirstTick;
	H.EndTick = LastTick;

	FApexSessionSummary Session;
	if (Net && Net->FindSessionById(SessionId, Session))
	{
		H.Track.TrackId = Session.TrackId;
		H.Track.Stem = StemOfTrackFile(Session.TrackFile);
		H.Track.DisplayName = Session.TrackName;
		H.SessionKind = Session.SessionKind;
		H.LapLimit = Session.LapLimit;
	}
	// A timed race has no distance; a stream header has no field for its
	// clock yet, so a replay of one counts laps without a limit.
	const bool bTimed = (Net && Net->GetSessionRaceSeconds() > 0) || (Flow && Flow->EffectiveRaceSeconds() > 0);
	if (H.LapLimit <= 0 && Flow && H.GameMode == EApexGameMode::Race && !bTimed)
	{
		H.LapLimit = Flow->CreateLapLimit;
	}
	if (H.GameMode == EApexGameMode::Hotlap)
	{
		H.LapLimit = 0;
	}
	FApexStreamPath Path;
	bool bPath = false;
	if (Net)
	{
		FApexTrackConfigSummary Track;
		if (!H.Track.TrackId.IsEmpty() && Net->FindTrackById(H.Track.TrackId, Track))
		{
			H.Track.SourceCrc = static_cast<uint32>(Track.ContentCrc);
			if (H.Track.DisplayName.IsEmpty())
			{
				H.Track.DisplayName = Track.Name;
			}
			if (Track.Centerline.Num() > 2)
			{
				Path.Points = Track.Centerline;
				double Length = 0.0;
				for (int32 i = 1; i < Path.Points.Num(); ++i)
				{
					Length += FVector2D::Distance(Path.Points[i - 1], Path.Points[i]);
				}
				Path.SpacingM = static_cast<float>(Length / (Path.Points.Num() - 1));
				bPath = true;
			}
		}
		H.Track.LengthM = Net->GetTrackSectors().TrackLengthM;
	}
	if (H.Track.Stem.IsEmpty())
	{
		OutError = TEXT("the session's circuit is not known");
		return false;
	}

	TArray<FApexStreamEvent> Preamble;
	if (Net && Net->GetTrackSectors().IsValid())
	{
		FApexStreamEvent& Sectors = Preamble.AddDefaulted_GetRef();
		Sectors.Tick = FirstTick;
		Sectors.Kind = ApexSpectator::EventTrackSectors;
		Sectors.Sectors = Net->GetTrackSectors();
	}
	OutBytes = Writer.ToBytes(H, FirstRoster, bPath ? &Path : nullptr, Preamble);
	return true;
}

bool UApexReplayRecorder::SaveReplay(FString& OutPath, FString& OutError)
{
	TArray<uint8> Bytes;
	FApexStreamHeader Header;
	if (!BuildFile(Bytes, Header, OutError))
	{
		return false;
	}
	const FString Base = MakeReplayName(FDateTime::Now(), Header.Track.DisplayName, Header.GameMode);
	OutPath = FPaths::Combine(ReplayDirectory(), Base + TEXT(".apxs"));
	for (int32 Suffix = 2; IFileManager::Get().FileExists(*OutPath); ++Suffix)
	{
		OutPath = FPaths::Combine(ReplayDirectory(), FString::Printf(TEXT("%s (%d).apxs"), *Base, Suffix));
	}
	if (!FFileHelper::SaveArrayToFile(Bytes, *OutPath))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
		return false;
	}
	UE_LOG(LogApexSim, Log, TEXT("Replay saved: %s (%.0f s, %lld KB)"), *OutPath, GetRecordedSeconds(), Bytes.Num() / 1024ll);
	return true;
}

void UApexReplayRecorder::FinishAndKeepRecent()
{
	if (!bRecording)
	{
		return;
	}
	bRecording = false;
	TArray<uint8> Bytes;
	FApexStreamHeader Header;
	FString Error;
	if (BuildFile(Bytes, Header, Error))
	{
		const FString Dir = bKeepWhenFinished ? ReplayDirectory() : RecentDirectory();
		const FString Base = MakeReplayName(FDateTime::Now(), Header.Track.DisplayName, Header.GameMode);
		FString Path = FPaths::Combine(Dir, Base + TEXT(".apxs"));
		for (int32 Suffix = 2; IFileManager::Get().FileExists(*Path); ++Suffix)
		{
			Path = FPaths::Combine(Dir, FString::Printf(TEXT("%s (%d).apxs"), *Base, Suffix));
		}
		if (FFileHelper::SaveArrayToFile(Bytes, *Path))
		{
			LastSessionPath = Path;
			if (bKeepWhenFinished)
			{
				UE_LOG(LogApexSim, Log, TEXT("Replay of the session saved: %s"), *Path);
			}
			else
			{
				UE_LOG(LogApexSim, Log, TEXT("Replay of the session kept among the recent ones: %s"), *Path);
				PruneRecent();
			}
		}
	}
	bKeepWhenFinished = false;
	Writer.Reset();
}

bool UApexReplayRecorder::CanKeepThisSession() const
{
	return bRecording || (!LastSessionPath.IsEmpty() && IFileManager::Get().FileExists(*LastSessionPath));
}

bool UApexReplayRecorder::IsThisSessionKept() const
{
	return bRecording ? bKeepWhenFinished
		: !LastSessionPath.IsEmpty() && FPaths::IsSamePath(FPaths::GetPath(LastSessionPath), ReplayDirectory());
}

bool UApexReplayRecorder::KeepThisSession(FString& OutPath, bool& bOutWhenFinished, FString& OutError)
{
	bOutWhenFinished = false;
	if (bRecording)
	{
		bKeepWhenFinished = true;
		bOutWhenFinished = true;
		return true;
	}
	if (LastSessionPath.IsEmpty() || !IFileManager::Get().FileExists(*LastSessionPath))
	{
		OutError = TEXT("nothing of this session was recorded");
		return false;
	}
	if (!KeepReplay(LastSessionPath, OutPath))
	{
		OutError = FString::Printf(TEXT("could not move %s"), *LastSessionPath);
		return false;
	}
	LastSessionPath = OutPath;
	UE_LOG(LogApexSim, Log, TEXT("Replay of the session saved: %s"), *OutPath);
	return true;
}

FString UApexReplayRecorder::ReplayDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Replays"));
}

FString UApexReplayRecorder::RecentDirectory()
{
	return FPaths::Combine(ReplayDirectory(), TEXT("Recent"));
}

FString UApexReplayRecorder::MakeReplayName(const FDateTime& When, const FString& Track, EApexGameMode Mode)
{
	FString Clean = Track.IsEmpty() ? FString(TEXT("Circuit")) : Track;
	// What a file system refuses, or a shell trips on.
	for (const TCHAR Bad : FString(TEXT("\\/:*?\"<>|")))
	{
		Clean.ReplaceCharInline(Bad, TEXT('-'));
	}
	return FString::Printf(TEXT("%s %s %s"), *When.ToString(TEXT("%Y-%m-%d %H.%M")), *Clean.TrimStartAndEnd(), ModeWord(Mode));
}

TArray<FApexReplayInfo> UApexReplayRecorder::ListReplays()
{
	TArray<FApexReplayInfo> Out;
	for (const bool bSaved : {true, false})
	{
		const FString Dir = bSaved ? ReplayDirectory() : RecentDirectory();
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.apxs")), true, false);
		TArray<FApexReplayInfo> Group;
		for (const FString& File : Files)
		{
			FApexReplayInfo Info;
			Info.Path = FPaths::Combine(Dir, File);
			Info.bSaved = bSaved;
			FApexStreamRoster Roster;
			FString Error;
			if (!FApexStreamFile::ReadPreamble(Info.Path, Info.Header, Roster, Error))
			{
				UE_LOG(LogApexSim, Warning, TEXT("Replay %s skipped: %s"), *Info.Path, *Error);
				continue;
			}
			Info.Cars = Roster.Entries.Num();
			if (const FApexStreamRosterEntry* Human = Roster.Entries.FindByPredicate([](const FApexStreamRosterEntry& E) { return !E.bIsAi; }))
			{
				Info.Driver = Human->Name;
			}
			// The file system keeps UTC; the list (and the names) are local time.
			Info.When = IFileManager::Get().GetTimeStamp(*Info.Path) + (FDateTime::Now() - FDateTime::UtcNow());
			Info.Bytes = IFileManager::Get().FileSize(*Info.Path);
			Group.Add(MoveTemp(Info));
		}
		Group.Sort([](const FApexReplayInfo& A, const FApexReplayInfo& B) { return A.When > B.When; });
		Out.Append(MoveTemp(Group));
	}
	return Out;
}

bool UApexReplayRecorder::KeepReplay(const FString& Path, FString& OutNewPath)
{
	OutNewPath = FPaths::Combine(ReplayDirectory(), FPaths::GetCleanFilename(Path));
	if (FPaths::IsSamePath(FPaths::GetPath(Path), ReplayDirectory()))
	{
		return true;
	}
	return IFileManager::Get().Move(*OutNewPath, *Path, /*bReplace*/ false);
}

bool UApexReplayRecorder::DeleteReplay(const FString& Path)
{
	return IFileManager::Get().Delete(*Path);
}

void UApexReplayRecorder::PruneRecent()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(RecentDirectory(), TEXT("*.apxs")), true, false);
	if (Files.Num() <= MaxRecent)
	{
		return;
	}
	TArray<TPair<FDateTime, FString>> Dated;
	for (const FString& File : Files)
	{
		const FString Path = FPaths::Combine(RecentDirectory(), File);
		Dated.Emplace(IFileManager::Get().GetTimeStamp(*Path), Path);
	}
	Dated.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key > B.Key; });
	for (int32 i = MaxRecent; i < Dated.Num(); ++i)
	{
		IFileManager::Get().Delete(*Dated[i].Value);
	}
}
