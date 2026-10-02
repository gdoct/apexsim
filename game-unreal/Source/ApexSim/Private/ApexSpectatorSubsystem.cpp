#include "ApexSpectatorSubsystem.h"

#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"

namespace
{
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

bool UApexSpectatorSubsystem::PlayFile(const FString& Path)
{
	Stop();
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

void UApexSpectatorSubsystem::Stop()
{
	if (Source == EApexSpectatorSource::Net)
	{
		if (UApexNetSubsystem* Net = GetNet())
		{
			Net->LeaveSpectate();
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
	SourceName = ShowcaseId;
	UE_LOG(LogApexSim, Log, TEXT("Spectator: watching showcase %s"), *ShowcaseId);
}

void UApexSpectatorSubsystem::HandleSpectatorRecords(TArrayView<const uint8> Run)
{
	if (Source != EApexSpectatorSource::Net || bFailed)
	{
		return;
	}
	FString Error;
	if (!Player.ApplyFramed(Run, Error))
	{
		UE_LOG(LogApexSim, Verbose, TEXT("Spectator: a record did not decode: %s"), *Error);
	}
	Drain();
}

bool UApexSpectatorSubsystem::Tick(float DeltaSeconds)
{
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
		Clock += static_cast<double>(DeltaSeconds) * Loaded->File.GetHeader().TickRate;
		while (Cursor < Loaded->Records.Num() && static_cast<double>(RecordTicks[Cursor]) <= Clock)
		{
			Player.Apply(Loaded->Records[Cursor], Error);
			++Cursor;
		}
		if (Cursor >= Loaded->Records.Num() && Clock > static_cast<double>(Loaded->File.GetHeader().EndTick) + Loaded->File.GetHeader().TickRate)
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
		const FApexStreamHeader& Header = Player.GetHeader();
		if (StreamId.IsEmpty())
		{
			StreamId = Header.StreamId;
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
		default:
			break;
		}
	}
	for (const FApexTelemetryFrame& Frame : Player.TakeFrames())
	{
		Net->FeedBackdropTelemetry(Frame);
	}
}

FString UApexSpectatorSubsystem::DescribeState() const
{
	const TCHAR* SourceText = Source == EApexSpectatorSource::File ? TEXT("file") : Source == EApexSpectatorSource::Net ? TEXT("showcase") : TEXT("none");
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
	return Line;
}
