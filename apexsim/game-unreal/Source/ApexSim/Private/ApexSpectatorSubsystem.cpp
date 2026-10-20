#include "ApexSpectatorSubsystem.h"

#include "ApexNetSubsystem.h"
#include "ApexProtocolCodec.h"
#include "ApexShowcaseRenderer.h"
#include "ApexSimJob.h"
#include "Misc/Crc.h"
#include "ApexSim.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"

namespace
{
	/**
	 * The most a live stream's timeshift keeps before its oldest frames go:
	 * hours of a hotlap, an hour or so of a full field.
	 */
	constexpr int64 LiveBufferCapBytes = 256ll * 1024 * 1024;

	FAutoConsoleCommandWithWorldAndArgs SpectateInfoCommand(
		TEXT("apexsim.spectate.Info"),
		TEXT("Print what the spectator player is doing: source, epoch, frames applied and dropped."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
				if (const UApexSpectatorSubsystem* Spectator = GameInstance ? GameInstance->GetSubsystem<UApexSpectatorSubsystem>() : nullptr)
				{
					UE_LOG(LogApexSim, Display, TEXT("%s"), *Spectator->DescribeState());
				}
			}));

	FAutoConsoleCommandWithWorldAndArgs SpectateNextCommand(
		TEXT("apexsim.spectate.Next"),
		TEXT("Move the menu backdrop on to the next showcase or file."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
				if (UApexSpectatorSubsystem* Spectator = GameInstance ? GameInstance->GetSubsystem<UApexSpectatorSubsystem>() : nullptr)
				{
					Spectator->RequestNext();
				}
			}));
}

void UApexSpectatorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	if (UApexNetSubsystem* Net = GetNet())
	{
		JoinedHandle = Net->OnSpectatorJoined.AddUObject(this, &UApexSpectatorSubsystem::HandleSpectatorJoined);
		RecordsHandle = Net->OnSpectatorRecords.AddUObject(this, &UApexSpectatorSubsystem::HandleSpectatorRecords);
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UApexSpectatorSubsystem::Tick));
}

void UApexSpectatorSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnSpectatorJoined.Remove(JoinedHandle);
		Net->OnSpectatorRecords.Remove(RecordsHandle);
	}
	Stop();
	Super::Deinitialize();
}

UApexNetSubsystem* UApexSpectatorSubsystem::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

TArray<FString> UApexSpectatorSubsystem::ShowcaseDirectories()
{
	TArray<FString> Dirs;
	FString Override;
	if (FParse::Value(FCommandLine::Get(), TEXT("-ApexShowcaseDir="), Override))
	{
		TArray<FString> Parts;
		Override.ParseIntoArray(Parts, TEXT("+"), true);
		for (const FString& Part : Parts)
		{
			Dirs.Add(FPaths::ConvertRelativePathToFull(Part));
		}
	}
	// Beside ApexSim.exe in a package (ProjectDir is <Release>/Game/ApexSim/),
	// the repo's build/showcase in the editor: the same rule as the tracks.
	const FString Default = FPlatformProperties::RequiresCookedData()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("Showcase"))
		: FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("build"), TEXT("showcase"));
	Dirs.AddUnique(FPaths::ConvertRelativePathToFull(Default));
	// The ones the game rendered itself (FApexShowcaseRenderer).
	Dirs.AddUnique(FApexShowcaseRenderer::CacheDirectory());
	return Dirs;
}

TArray<FString> UApexSpectatorSubsystem::FindShowcaseFiles()
{
	TArray<FString> Files;
	for (const FString& Dir : ShowcaseDirectories())
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*.apxs")), true, false);
		Names.Sort();
		for (const FString& Name : Names)
		{
			Files.AddUnique(Dir / Name);
		}
	}
	return Files;
}

bool UApexSpectatorSubsystem::PlayFile(const FString& Path, bool bInLoop)
{
	Stop();
	bLoop = bInLoop;
	if (!IFileManager::Get().FileExists(*Path))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Spectator: no such file %s"), *Path);
		return false;
	}
	Source = EApexSpectatorSource::File;
	SourceName = Path;
	bLoading = true;
	const FString FilePath = Path;
	// Read and inflate off the game thread: a few megabytes of zlib.
	Loading = Async(EAsyncExecution::ThreadPool, [FilePath]() -> TSharedPtr<FLoadedFile>
	{
		TSharedPtr<FLoadedFile> Out = MakeShared<FLoadedFile>();
		if (!Out->File.LoadFromFile(FilePath, Out->Error))
		{
			return Out;
		}
		if (!Out->File.ReadAll(Out->Records, Out->Error))
		{
			return Out;
		}
		Out->bOk = true;
		return Out;
	});
	UE_LOG(LogApexSim, Log, TEXT("Spectator: loading %s"), *Path);
	return true;
}

bool UApexSpectatorSubsystem::WatchShowcase(const FString& Id)
{
	Stop();
	UApexNetSubsystem* Net = GetNet();
	if (!Net || !Net->IsAuthenticated())
	{
		return false;
	}
	Source = EApexSpectatorSource::Net;
	SourceName = Id.IsEmpty() ? TEXT("(the server's first showcase)") : Id;
	bLoading = true;
	Net->SpectateShowcase(Id);
	return true;
}

bool UApexSpectatorSubsystem::WatchLive(const FString& SessionId)
{
	Stop();
	UApexNetSubsystem* Net = GetNet();
	if (!Net || !Net->IsAuthenticated())
	{
		return false;
	}
	Source = EApexSpectatorSource::Net;
	bLiveSource = true;
	SourceName = SessionId;
	bLoading = true;
	Net->SpectateSession(SessionId);
	return true;
}

bool UApexSpectatorSubsystem::CanSimulateHotlap(FString* OutWhy)
{
	static TAutoConsoleVariable<int32> CVarLocalWatch(
		TEXT("apexsim.watch.Local"),
		1,
		TEXT("1 simulates Watch hotlap in the game (the physics library) when it can; 0 asks the server for it."),
		ECVF_Default);
	FString Why, TracksDir, CarsDir;
	const bool bCan = CVarLocalWatch.GetValueOnGameThread() != 0 && ApexPhysics::IsAvailable(&Why)
		&& ApexSimContent::Find(TracksDir, CarsDir, Why);
	if (OutWhy)
	{
		*OutWhy = CVarLocalWatch.GetValueOnGameThread() == 0 ? FString(TEXT("apexsim.watch.Local is 0")) : Why;
	}
	return bCan;
}

FString UApexSpectatorSubsystem::WatchRecordsDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ApexSim"), TEXT("Watch")));
}

bool UApexSpectatorSubsystem::StartLocalHotlap(const FString& TrackConfigId, const FString& TrackName, const FString& TrackStem,
	const FApexSessionConditions& Conditions, const FApexCarSetup* Setup, const FString& CarId, FString& OutError)
{
	FString TracksDir, CarsDir;
	if (!ApexSimContent::Find(TracksDir, CarsDir, OutError))
	{
		return false;
	}
	const FString Yaml = ApexSimContent::TrackYaml(TracksDir, TrackStem);
	if (Yaml.IsEmpty())
	{
		OutError = FString::Printf(TEXT("no track YAML for %s"), *TrackStem);
		return false;
	}
	// Any car, the player's own among them: the cars folder holds both.
	const FString CarsRoot = FPaths::GetCleanFilename(CarsDir) == TEXT("default") ? FPaths::GetPath(CarsDir) : CarsDir;
	// The create a server would get (SpectateHotlapWatch's).
	ApexProtocol::FHotlapWatchOptions Options;
	Options.bStream = true;
	if (Setup)
	{
		Options.bHasSetup = true;
		Options.Setup = *Setup;
	}
	const TArray<uint8> Create = ApexProtocol::EncodeCreateSession(TrackConfigId, 1, 1, 3, EApexSessionKind::HotlapWatch,
		FApexAllowedAssists(), Conditions, EApexDamageLevel::Full, ApexAiSkill::Mixed, 0, TArray<FString>(), 0,
		FApexWeekendFormat(), &Options);
	// One watch of a circuit and car is one set of laps (the sky's own draws
	// included), as a seeded race is.
	const uint64 Seed = FCrc::StrCrc32(*(TrackStem + TEXT("/") + CarId));
	TUniquePtr<ApexPhysics::FWatch> Watch = ApexPhysics::FWatch::Start(Create, Yaml, CarsRoot, CarId, WatchRecordsDirectory(),
		/*RoadContact: the live server's road mesh*/ 1, /*the simulation's own rate*/ 0, Seed, OutError);
	if (!Watch)
	{
		return false;
	}
	TArray<uint8> Corners;
	Watch->Corners(Corners);
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->BeginLocalHotlapWatch(TrackConfigId, TrackName, TrackStem, Conditions, Corners);
	}
	LocalWatch = MoveTemp(Watch);
	LocalTickDebt = 0.0;
	Source = EApexSpectatorSource::Local;
	bLiveSource = true;
	SourceName = FString::Printf(TEXT("hotlap at %s, simulated here"), *TrackStem);
	bLoading = true;
	UE_LOG(LogApexSim, Log, TEXT("Spectator: watching a hotlap at %s in %s, simulated here"), *TrackStem, *CarId);
	return true;
}

void UApexSpectatorSubsystem::TickLocal(float DeltaSeconds)
{
	if (Source != EApexSpectatorSource::Local || !LocalWatch || bFailed)
	{
		return;
	}
	const double Rate = static_cast<double>(LiveTickRate());
	// A long frame (a hitch, a level load) catches up a quarter second at a
	// time rather than simulating the gap in one frame.
	LocalTickDebt = FMath::Min(LocalTickDebt + static_cast<double>(DeltaSeconds) * Rate, Rate * 0.25);
	// The first tick gives the catch-up (the header names the rate).
	const uint32 Ticks = Player.HasHeader() ? static_cast<uint32>(LocalTickDebt) : 1u;
	if (Ticks == 0)
	{
		return;
	}
	LocalTickDebt = FMath::Max(0.0, LocalTickDebt - Ticks);
	TArray<uint8> Run;
	if (!LocalWatch->Tick(Ticks) || !LocalWatch->Take(Run))
	{
		Fail(FString::Printf(TEXT("the hotlap stopped: %s"), *ApexPhysics::LastError()));
		return;
	}
	if (Run.Num() > 0)
	{
		ApplyLiveRun(Run);
	}
}

bool UApexSpectatorSubsystem::WatchHotlap(const FString& TrackConfigId, const FString& TrackName, const FString& TrackStem,
	const FApexSessionConditions& Conditions, const FApexCarSetup* Setup, const FString& CarId, FString& OutError)
{
	Stop();
	FString Why;
	if (CanSimulateHotlap(&Why))
	{
		if (StartLocalHotlap(TrackConfigId, TrackName, TrackStem, Conditions, Setup, CarId, OutError))
		{
			return true;
		}
		UE_LOG(LogApexSim, Warning, TEXT("Spectator: the hotlap cannot be simulated here (%s); asking the server"), *OutError);
	}
	UApexNetSubsystem* Net = GetNet();
	if (!Net || !Net->IsAuthenticated())
	{
		OutError = OutError.IsEmpty() ? FString::Printf(TEXT("not connected to a server (%s)"), *Why) : OutError;
		return false;
	}
	Source = EApexSpectatorSource::Net;
	bLiveSource = true;
	SourceName = FString::Printf(TEXT("hotlap at %s"), *TrackStem);
	bLoading = true;
	Net->SpectateHotlapWatch(TrackConfigId, TrackName, TrackStem, Conditions, Setup);
	OutError.Reset();
	return true;
}

void UApexSpectatorSubsystem::Stop()
{
	if (Source == EApexSpectatorSource::Net)
	{
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->LeaveSpectate();
		}
	}
	if (Source == EApexSpectatorSource::Local)
	{
		// Freed, the watch keeps its ladder level and ghost.
		LocalWatch.Reset();
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->EndLocalHotlapWatch();
		}
	}
	if (bFeeding)
	{
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->EndBackdropFeed();
		}
		bFeeding = false;
	}
	Source = EApexSpectatorSource::None;
	bLiveSource = false;
	SourceName.Reset();
	bLoading = false;
	bFailed = false;
	Failure.Reset();
	Player.Reset();
	Centerline.Reset();
	StreamId.Reset();
	Loading = TFuture<TSharedPtr<FLoadedFile>>();
	Loaded.Reset();
	RecordTicks.Reset();
	Clock = 0.0;
	Cursor = 0;
	Loops = 0;
	bLoop = true;
	bPaused = false;
	PlaybackRate = 1.0f;
	LiveRecords.Reset();
	LiveTicks.Reset();
	LiveBytes = 0;
	LiveCursor = 0;
	LiveClock = 0.0;
	LiveStartTick = -1;
	bAtLiveEdge = true;
}

void UApexSpectatorSubsystem::SetPlaybackRate(float Rate)
{
	PlaybackRate = FMath::Clamp(Rate, 0.25f, 4.0f);
	if (CanTimeshift() && PlaybackRate < 1.0f)
	{
		// Slower than it comes in: the stream falls behind its edge.
		if (bAtLiveEdge)
		{
			LiveClock = static_cast<double>(LiveEdgeTick());
		}
		bAtLiveEdge = false;
	}
}

void UApexSpectatorSubsystem::SetPaused(bool bInPaused)
{
	bPaused = bInPaused;
	if (bPaused && CanTimeshift())
	{
		if (bAtLiveEdge)
		{
			LiveClock = static_cast<double>(LiveEdgeTick());
		}
		bAtLiveEdge = false;
	}
}

int32 UApexSpectatorSubsystem::LiveTickRate() const
{
	return Player.HasHeader() && Player.GetHeader().TickRate > 0 ? Player.GetHeader().TickRate : 420;
}

double UApexSpectatorSubsystem::GetSecondsBehindLive() const
{
	if (!CanTimeshift() || bAtLiveEdge)
	{
		return 0.0;
	}
	return FMath::Max(0.0, (static_cast<double>(LiveEdgeTick()) - LiveClock) / LiveTickRate());
}

void UApexSpectatorSubsystem::KeepLive(TArrayView<const uint8> Run)
{
	// [u32 big-endian length][body], as the TCP message and a datagram carry them.
	int32 At = 0;
	int64 Last = LiveTicks.IsEmpty() ? 0 : LiveTicks.Last();
	while (At + 4 <= Run.Num())
	{
		const int32 Length = static_cast<int32>((static_cast<uint32>(Run[At]) << 24) | (static_cast<uint32>(Run[At + 1]) << 16)
			| (static_cast<uint32>(Run[At + 2]) << 8) | static_cast<uint32>(Run[At + 3]));
		At += 4;
		if (Length < 0 || At + Length > Run.Num())
		{
			break;
		}
		TArray<uint8> Body(Run.GetData() + At, Length);
		At += Length;
		int64 Tick = Last;
		ApexSpectator::RecordTick(Body, Tick);
		// A frame part late off UDP (an older tick) stays in arrival order:
		// the player drops what it has passed anyway.
		Last = FMath::Max(Last, Tick);
		if (LiveStartTick < 0 && ApexSpectator::RecordType(Body) == ApexSpectator::RecordFrame)
		{
			LiveStartTick = Tick;
		}
		LiveBytes += Body.Num();
		LiveTicks.Add(Last);
		LiveRecords.Add(MoveTemp(Body));
	}
	if (LiveBytes > LiveBufferCapBytes)
	{
		TrimLive();
	}
}

void UApexSpectatorSubsystem::TrimLive()
{
	// The oldest frames go, down to three quarters of the cap; everything
	// else (the header, the roster, the timing so far) stays, so a seek to
	// the start still knows the race.
	int64 ToFree = LiveBytes - LiveBufferCapBytes * 3 / 4;
	TArray<TArray<uint8>> Kept;
	TArray<int64> KeptTicks;
	Kept.Reserve(LiveRecords.Num());
	KeptTicks.Reserve(LiveRecords.Num());
	int32 DroppedBeforeCursor = 0;
	LiveStartTick = -1;
	for (int32 Index = 0; Index < LiveRecords.Num(); ++Index)
	{
		const bool bFrame = ApexSpectator::RecordType(LiveRecords[Index]) == ApexSpectator::RecordFrame;
		if (bFrame && ToFree > 0)
		{
			ToFree -= LiveRecords[Index].Num();
			LiveBytes -= LiveRecords[Index].Num();
			DroppedBeforeCursor += Index < LiveCursor ? 1 : 0;
			continue;
		}
		if (bFrame && LiveStartTick < 0)
		{
			LiveStartTick = LiveTicks[Index];
		}
		Kept.Add(MoveTemp(LiveRecords[Index]));
		KeptTicks.Add(LiveTicks[Index]);
	}
	LiveRecords = MoveTemp(Kept);
	LiveTicks = MoveTemp(KeptTicks);
	LiveCursor = FMath::Max(0, LiveCursor - DroppedBeforeCursor);
	LiveClock = FMath::Max(LiveClock, static_cast<double>(LiveStartTick));
}

void UApexSpectatorSubsystem::ApplyLiveUpTo(double Tick, bool bSkipFrames)
{
	FString Error;
	int32 LastFrameTickStart = INDEX_NONE;
	while (LiveCursor < LiveRecords.Num() && static_cast<double>(LiveTicks[LiveCursor]) <= Tick)
	{
		const TArray<uint8>& Body = LiveRecords[LiveCursor];
		if (bSkipFrames && ApexSpectator::RecordType(Body) == ApexSpectator::RecordFrame)
		{
			if (LastFrameTickStart == INDEX_NONE || LiveTicks[LastFrameTickStart] != LiveTicks[LiveCursor])
			{
				LastFrameTickStart = LiveCursor;
			}
		}
		else if (!Player.Apply(Body, Error))
		{
			UE_LOG(LogApexSim, Verbose, TEXT("Spectator: a record did not decode: %s"), *Error);
		}
		++LiveCursor;
	}
	for (int32 Index = LastFrameTickStart; Index != INDEX_NONE && Index < LiveCursor; ++Index)
	{
		if (LiveTicks[Index] == LiveTicks[LastFrameTickStart]
			&& ApexSpectator::RecordType(LiveRecords[Index]) == ApexSpectator::RecordFrame)
		{
			Player.Apply(LiveRecords[Index], Error);
		}
	}
}

double UApexSpectatorSubsystem::GetPlaybackSeconds() const
{
	if (CanTimeshift())
	{
		const double At = bAtLiveEdge ? static_cast<double>(LiveEdgeTick()) : LiveClock;
		return LiveStartTick < 0 ? 0.0 : FMath::Max(0.0, (At - static_cast<double>(LiveStartTick)) / LiveTickRate());
	}
	if (Source != EApexSpectatorSource::File || !Loaded || !Loaded->bOk)
	{
		return 0.0;
	}
	const FApexStreamHeader& H = Loaded->File.GetHeader();
	return H.TickRate > 0 ? FMath::Max(0.0, (Clock - static_cast<double>(H.StartTick)) / H.TickRate) : 0.0;
}

double UApexSpectatorSubsystem::GetDurationSeconds() const
{
	if (CanTimeshift())
	{
		return LiveStartTick < 0 ? 0.0 : FMath::Max(0.0, static_cast<double>(LiveEdgeTick() - LiveStartTick) / LiveTickRate());
	}
	return Source == EApexSpectatorSource::File && Loaded && Loaded->bOk ? Loaded->File.GetHeader().DurationSeconds() : 0.0;
}

bool UApexSpectatorSubsystem::IsAtEnd() const
{
	return Source == EApexSpectatorSource::File && !bLoop && Loaded && Loaded->bOk && Cursor >= Loaded->Records.Num();
}

void UApexSpectatorSubsystem::SeekTo(double Seconds)
{
	if (CanTimeshift())
	{
		const double Start = static_cast<double>(FMath::Max<int64>(LiveStartTick, 0));
		const double Edge = static_cast<double>(LiveEdgeTick());
		const double From = bAtLiveEdge ? Edge : LiveClock;
		const double Target = FMath::Clamp(Start + FMath::Max(0.0, Seconds) * LiveTickRate(), Start, Edge);
		if (Target < From)
		{
			// Back: from the first record (the catch-up's header, which
			// starts the timing over) forward to the target.
			LiveCursor = 0;
		}
		ApplyLiveUpTo(Target, /*bSkipFrames*/ true);
		LiveClock = Target;
		// Past the edge is live again, unless paused there.
		bAtLiveEdge = Target >= Edge && !bPaused;
		if (bAtLiveEdge)
		{
			PlaybackRate = 1.0f;
		}
		Drain();
		return;
	}
	if (Source != EApexSpectatorSource::File || bLoading || bFailed || !Loaded || !Loaded->bOk)
	{
		return;
	}
	const FApexStreamHeader& H = Loaded->File.GetHeader();
	const double Target = FMath::Clamp(static_cast<double>(H.StartTick) + FMath::Max(0.0, Seconds) * H.TickRate,
		static_cast<double>(H.StartTick), static_cast<double>(H.EndTick));
	FString Error;
	if (Target < Clock)
	{
		// Back: the preamble again (the sectors in it start the timing over),
		// then forward from the top.
		Player.ApplyFramed(Loaded->File.GetPreambleBytes(), Error);
		Cursor = 0;
	}
	// Every record on the way but the frames, which only the last tick's
	// need: a minute skipped is a minute of lap timing, not of cars.
	int32 LastFrameTickStart = INDEX_NONE;
	while (Cursor < Loaded->Records.Num() && static_cast<double>(RecordTicks[Cursor]) <= Target)
	{
		const TArray<uint8>& Body = Loaded->Records[Cursor];
		if (ApexSpectator::RecordType(Body) == ApexSpectator::RecordFrame)
		{
			if (LastFrameTickStart == INDEX_NONE || RecordTicks[LastFrameTickStart] != RecordTicks[Cursor])
			{
				LastFrameTickStart = Cursor;
			}
		}
		else
		{
			Player.Apply(Body, Error);
		}
		++Cursor;
	}
	for (int32 Index = LastFrameTickStart; Index != INDEX_NONE && Index < Cursor; ++Index)
	{
		if (RecordTicks[Index] == RecordTicks[LastFrameTickStart]
			&& ApexSpectator::RecordType(Loaded->Records[Index]) == ApexSpectator::RecordFrame)
		{
			Player.Apply(Loaded->Records[Index], Error);
		}
	}
	Clock = Target;
	Drain();
}

void UApexSpectatorSubsystem::Fail(const FString& Why)
{
	UE_LOG(LogApexSim, Warning, TEXT("Spectator: %s"), *Why);
	bFailed = true;
	bLoading = false;
	Failure = Why;
	if (bFeeding)
	{
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->EndBackdropFeed();
		}
		bFeeding = false;
	}
}

void UApexSpectatorSubsystem::HandleSpectatorJoined(const FString& InStreamId, const FString& ShowcaseId)
{
	if (Source != EApexSpectatorSource::Net)
	{
		return;
	}
	if (InStreamId.IsEmpty())
	{
		Fail(TEXT("the server refused the showcase"));
		return;
	}
	StreamId = InStreamId;
	if (!bLiveSource)
	{
		SourceName = ShowcaseId;
	}
	UE_LOG(LogApexSim, Log, TEXT("Spectator: watching %s %s"), bLiveSource ? TEXT("session") : TEXT("showcase"), *SourceName);
}

void UApexSpectatorSubsystem::HandleSpectatorRecords(TArrayView<const uint8> Run)
{
	if (Source != EApexSpectatorSource::Net || bFailed)
	{
		return;
	}
	if (bLiveSource)
	{
		ApplyLiveRun(Run);
		return;
	}
	FString Error;
	if (!Player.ApplyFramed(Run, Error))
	{
		UE_LOG(LogApexSim, Verbose, TEXT("Spectator: a record did not decode: %s"), *Error);
	}
	Drain();
}

void UApexSpectatorSubsystem::ApplyLiveRun(TArrayView<const uint8> Run)
{
	// Kept for the timeshift; played at once at the live edge.
	KeepLive(Run);
	if (bAtLiveEdge)
	{
		ApplyLiveUpTo(static_cast<double>(LiveEdgeTick()), /*bSkipFrames*/ false);
		LiveClock = static_cast<double>(LiveEdgeTick());
	}
	Drain();
}

bool UApexSpectatorSubsystem::Tick(float DeltaSeconds)
{
	// A watch simulated here runs on the game's clock whatever is on screen
	// (paused or behind, the timeshift plays what it kept).
	TickLocal(DeltaSeconds);
	if (CanTimeshift() && !bAtLiveEdge && !bFailed)
	{
		// Behind the edge: the kept records on the play clock, until it
		// catches up (faster than real time, or a seek past the edge).
		if (!bPaused)
		{
			LiveClock += static_cast<double>(DeltaSeconds) * LiveTickRate() * PlaybackRate;
		}
		const double Edge = static_cast<double>(LiveEdgeTick());
		LiveClock = FMath::Min(LiveClock, Edge);
		ApplyLiveUpTo(LiveClock, /*bSkipFrames*/ false);
		if (!bPaused && PlaybackRate >= 1.0f && LiveCursor >= LiveRecords.Num() && LiveClock >= Edge)
		{
			bAtLiveEdge = true;
			PlaybackRate = 1.0f;
		}
		Drain();
		return true;
	}
	if (Source == EApexSpectatorSource::File && bLoading && Loading.IsValid() && Loading.IsReady())
	{
		Loaded = Loading.Get();
		Loading = TFuture<TSharedPtr<FLoadedFile>>();
		if (!Loaded || !Loaded->bOk)
		{
			Fail(FString::Printf(TEXT("cannot play %s: %s"), *SourceName, Loaded ? *Loaded->Error : TEXT("no result")));
			return true;
		}
		bLoading = false;
		RecordTicks.Reset(Loaded->Records.Num());
		int64 Last = Loaded->File.GetHeader().StartTick;
		for (const TArray<uint8>& Body : Loaded->Records)
		{
			int64 Tick = Last;
			ApexSpectator::RecordTick(Body, Tick);
			Last = Tick;
			RecordTicks.Add(Tick);
		}
		Cursor = 0;
		Clock = static_cast<double>(Loaded->File.GetHeader().StartTick);
		UE_LOG(LogApexSim, Log, TEXT("Spectator: %s in, %d record(s), %.0f s of %s"), *SourceName, Loaded->Records.Num(),
			Loaded->File.GetHeader().DurationSeconds(), *Loaded->File.GetHeader().Track.DisplayName);
	}

	if (Source == EApexSpectatorSource::File && !bLoading && !bFailed && Loaded)
	{
		FString Error;
		if (Cursor == 0 && !Player.HasHeader())
		{
			// The preamble (header, roster, path, sectors), straight from the
			// file's own bytes.
			if (!Player.ApplyFramed(Loaded->File.GetPreambleBytes(), Error))
			{
				Fail(FString::Printf(TEXT("the preamble of %s does not decode: %s"), *SourceName, *Error));
				return true;
			}
		}
		if (!bPaused)
		{
			Clock += static_cast<double>(DeltaSeconds) * Loaded->File.GetHeader().TickRate * PlaybackRate;
		}
		if (!bLoop)
		{
			// A replay holds on its last frame.
			Clock = FMath::Min(Clock, static_cast<double>(Loaded->File.GetHeader().EndTick));
		}
		while (Cursor < Loaded->Records.Num() && static_cast<double>(RecordTicks[Cursor]) <= Clock)
		{
			Player.Apply(Loaded->Records[Cursor], Error);
			++Cursor;
		}
		if (bLoop && Cursor >= Loaded->Records.Num() && Clock > static_cast<double>(Loaded->File.GetHeader().EndTick) + Loaded->File.GetHeader().TickRate)
		{
			// The end: start over. The header resets the player, and the cars
			// jump back to the grid, which the motion buffers take as a
			// restart.
			++Loops;
			Cursor = 0;
			Clock = static_cast<double>(Loaded->File.GetHeader().StartTick);
			Player.ApplyFramed(Loaded->File.GetPreambleBytes(), Error);
		}
		Drain();
	}
	return true;
}

void UApexSpectatorSubsystem::Drain()
{
	UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return;
	}
	// The path before the feed begins: the demo view takes the cameras'
	// centerline the moment the feed starts.
	if (Player.HasPath() && Centerline.Num() != Player.GetPath().Points.Num())
	{
		Centerline = Player.GetPath().Points;
	}
	if (Player.TakeHeaderChanged())
	{
		if (Source == EApexSpectatorSource::Net || Source == EApexSpectatorSource::Local)
		{
			bLoading = false;
		}
		const FApexStreamHeader& Header = Player.GetHeader();
		if (StreamId.IsEmpty())
		{
			StreamId = Header.StreamId;
			if (Source == EApexSpectatorSource::Local)
			{
				Net->SetLocalHotlapWatchId(StreamId);
			}
		}
		if (!bFeeding)
		{
			Net->BeginBackdropFeed(Header.Conditions);
			bFeeding = true;
		}
	}
	if (Player.TakeRosterChanged())
	{
		Net->FeedBackdropRoster(Player.GetRoster().ToSessionRoster(StreamId));
	}
	for (const FApexStreamEvent& Event : Player.TakeEvents())
	{
		switch (Event.Kind)
		{
		case ApexSpectator::EventTrackSectors:
		{
			FApexTrackSectors Sectors = Event.Sectors;
			Sectors.SessionId = StreamId;
			Net->FeedBackdropSectors(Sectors);
			break;
		}
		case ApexSpectator::EventLapTiming:
			Net->FeedBackdropLapTiming(Event.LapTiming);
			break;
		case ApexSpectator::EventPitService:
			Net->FeedBackdropPitService(Event.PitService);
			break;
		default:
			break;
		}
	}
	for (const FApexTelemetryFrame& Frame : Player.TakeFrames())
	{
		Net->FeedBackdropTelemetry(Frame);
	}
	for (const FApexStreamRoad& Road : Player.TakeRoads())
	{
		Net->FeedBackdropRoad(Road.Road);
	}
	for (const FApexStreamGhost& Ghost : Player.TakeGhosts())
	{
		Net->FeedBackdropGhost(Ghost.Lap);
	}
}

FString UApexSpectatorSubsystem::DescribeState() const
{
	const TCHAR* SourceText = Source == EApexSpectatorSource::File ? TEXT("file")
		: Source == EApexSpectatorSource::Net ? (bLiveSource ? TEXT("live session") : TEXT("showcase"))
		: Source == EApexSpectatorSource::Local ? TEXT("simulated here") : TEXT("none");
	FString Line = FString::Printf(TEXT("Spectator: source %s %s"), SourceText, *SourceName);
	if (bLoading)
	{
		Line += TEXT(" (loading)");
	}
	if (bFailed)
	{
		Line += FString::Printf(TEXT(" FAILED: %s"), *Failure);
	}
	if (Player.HasHeader())
	{
		const FApexStreamHeader& H = Player.GetHeader();
		Line += FString::Printf(TEXT("; %s %s epoch %u, %d Hz frames of a %d Hz clock, %.0f s"), *H.Track.DisplayName,
			*H.Conditions.Describe(), H.Epoch, H.FrameRate, H.TickRate, H.DurationSeconds());
	}
	Line += FString::Printf(TEXT("; frames applied %d, dropped %d, incomplete %d"), Player.GetAppliedFrames(),
		Player.GetDroppedFrames(), Player.GetIncompleteFrames());
	if (Source == EApexSpectatorSource::File && Loaded)
	{
		Line += FString::Printf(TEXT("; at tick %.0f of %lld, loop %d"), Clock, Loaded->File.GetHeader().EndTick, Loops);
	}
	if (CanTimeshift())
	{
		Line += FString::Printf(TEXT("; timeshift %d record(s), %.1f MB, "), LiveRecords.Num(), LiveBytes / (1024.0 * 1024.0));
		Line += bAtLiveEdge ? FString(TEXT("live"))
			: FString::Printf(TEXT("%.1f s behind%s at %.2fx"), GetSecondsBehindLive(), bPaused ? TEXT(", paused") : TEXT(""), PlaybackRate);
	}
	return Line;
}
