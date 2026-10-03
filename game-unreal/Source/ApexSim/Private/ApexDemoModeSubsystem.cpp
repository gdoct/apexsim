#include "ApexDemoModeSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "ApexSpectatorSubsystem.h"
#include "ApexSpectatorStream.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Race/ApexRaceDirector.h"
#include "Track/ApexTrackContentSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> CVarDemoEnabled(
		TEXT("apexsim.demo.Enabled"),
		1,
		TEXT("1 plays an AI race behind the menu; 0 leaves the menu on its plain background."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDemoAiCount(
		TEXT("apexsim.demo.AiCount"),
		10,
		TEXT("Cars in a demo session (the circuit's grid may hold fewer); a showcase has its own field."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDemoLaps(
		TEXT("apexsim.demo.Laps"),
		3,
		TEXT("Laps in each demo session before the next one starts."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDemoRandomSky(
		TEXT("apexsim.demo.RandomSky"),
		1,
		TEXT("1 gives each demo session a random weather and time of day; 0 races under the default sunny 13:00. A showcase keeps the sky it was rendered in."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarDemoMaxMinutes(
		TEXT("apexsim.demo.MaxMinutes"),
		12.0f,
		TEXT("A backdrop still running after this long moves on to the next (a car stuck off the road never finishes)."),
		ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs DemoRestartCommand(
		TEXT("apexsim.demo.Restart"),
		TEXT("Start the menu's backdrop race over."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
				if (UApexSpectatorSubsystem* Spectator = GameInstance ? GameInstance->GetSubsystem<UApexSpectatorSubsystem>() : nullptr)
				{
					Spectator->RequestNext();
				}
			}));

	/** Seconds a finished backdrop race lingers on its cool-down laps before the next. */
	constexpr float FinishedLingerSeconds = 6.0f;
	/** How long a connection under way, or a channel list asked for, is given before a local file plays instead. */
	constexpr float ServerGraceSeconds = 3.0f;

	/** `Zandvoort.gt3.day.apxs` -> `Zandvoort`. */
	FString StemOfShowcaseFile(const FString& Path)
	{
		FString Name = FPaths::GetCleanFilename(Path);
		int32 Dot = INDEX_NONE;
		if (Name.FindChar(TEXT('.'), Dot))
		{
			Name.LeftInline(Dot);
		}
		return Name;
	}
}

void UApexDemoModeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexMenuFlowSubsystem>();
	Collection.InitializeDependency<UApexSpectatorSubsystem>();

	if (UApexNetSubsystem* Net = GetNet())
	{
		DemoChangedHandle = Net->OnDemoSessionChanged.AddUObject(this, &UApexDemoModeSubsystem::HandleDemoSessionChanged);
		ShowcasesHandle = Net->OnShowcases.AddUObject(this, &UApexDemoModeSubsystem::HandleShowcases);
	}
	FParse::Value(FCommandLine::Get(), TEXT("-ApexShowcase="), ForcedShowcase);
	bNoShowcase = FParse::Param(FCommandLine::Get(), TEXT("ApexNoShowcase"));
	// Known from the start, so the splash hold can count on a file before
	// the server has answered (or refused).
	if (!IsDemoDisabled() && !bNoShowcase)
	{
		ScanLocalFiles();
	}
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UApexDemoModeSubsystem::Tick));
}

void UApexDemoModeSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	if (UApexNetSubsystem* Net = GetNet())
	{
		Net->OnDemoSessionChanged.Remove(DemoChangedHandle);
		Net->OnShowcases.Remove(ShowcasesHandle);
	}
	Super::Deinitialize();
}

UApexNetSubsystem* UApexDemoModeSubsystem::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

UApexMenuFlowSubsystem* UApexDemoModeSubsystem::GetFlow() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
}

UApexSpectatorSubsystem* UApexDemoModeSubsystem::GetSpectator() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexSpectatorSubsystem>() : nullptr;
}

AApexRaceDirector* UApexDemoModeSubsystem::GetDirector() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? AApexRaceDirector::Find(GameInstance->GetWorld()) : nullptr;
}

bool UApexDemoModeSubsystem::IsDemoAllowed(const UApexNetSubsystem& Net) const
{
	// A replay the player asked for plays with the backdrop turned off too.
	if (IsDemoDisabled() && Source != EApexBackdropSource::Replay)
	{
		return false;
	}
	const AApexRaceDirector* Director = GetDirector();
	return Director
		&& !Net.IsInSession()
		&& !Net.IsSessionRequestPending()
		// The player's own race has the director.
		&& (!Director->IsRaceViewActive() || Director->IsDemoViewActive());
}

bool UApexDemoModeSubsystem::IsDemoExpected() const
{
	if (IsDemoDisabled())
	{
		return false;
	}
	const UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return false;
	}
	if (Source != EApexBackdropSource::None)
	{
		return true;
	}
	// A local file needs no server: as long as one is on disk and nothing
	// has failed, a backdrop is coming.
	if (!bNoShowcase && bFilesScanned && LocalFiles.Num() > 0 && Failures == 0)
	{
		return true;
	}
	switch (Net->GetConnectionState())
	{
	case EApexConnectionState::Connecting:
	case EApexConnectionState::Authenticating:
		return true;
	case EApexConnectionState::Authenticated:
		// A failed request backs off for seconds; a startup should not wait it out.
		return !bWarnedNoTracks && (Failures == 0 || Net->IsInDemoSession() || Net->IsDemoSessionRequested());
	default:
		// Disconnected (nobody asked to connect), Failed, or Reconnecting after
		// the server could not be reached: a local file, if any, was tried above.
		return false;
	}
}

bool UApexDemoModeSubsystem::IsDemoDisabled()
{
	static const bool bCommandLineOff = FParse::Param(FCommandLine::Get(), TEXT("ApexNoDemo"))
		// An unattended run is there to look at something else.
		|| FParse::Param(FCommandLine::Get(), TEXT("ApexAutoRace"))
		// A replay plays offline, with no backdrop under it.
		|| FCString::Strifind(FCommandLine::Get(), TEXT("-ApexReplay=")) != nullptr;
	return bCommandLineOff || CVarDemoEnabled.GetValueOnGameThread() == 0;
}

FApexSessionConditions UApexDemoModeSubsystem::RollConditions(FRandomStream& Random)
{
	FApexSessionConditions Conditions;

	struct FWeatherWeight
	{
		EApexWeather Weather;
		int32 Weight;
	};
	static constexpr FWeatherWeight Weathers[] = {
		{ EApexWeather::Sunny, 38 },
		{ EApexWeather::Cloudy, 30 },
		{ EApexWeather::Overcast, 22 },
		{ EApexWeather::LightRain, 6 },
		{ EApexWeather::HeavyRain, 4 },
	};
	int32 Roll = Random.RandRange(0, 99);
	for (const FWeatherWeight& Entry : Weathers)
	{
		Conditions.Weather = Entry.Weather;
		if (Roll < Entry.Weight)
		{
			break;
		}
		Roll -= Entry.Weight;
	}

	// [first, last] quarter hour, counted from midnight.
	auto QuarterIn = [&Random](int32 FirstQuarter, int32 LastQuarter)
	{
		return Random.RandRange(FirstQuarter, LastQuarter) * 15;
	};
	const int32 ClockRoll = Random.RandRange(0, 99);
	if (ClockRoll < 70)
	{
		Conditions.TimeOfDayMinutes = QuarterIn(8 * 4, 18 * 4 - 1);
	}
	else if (ClockRoll < 90)
	{
		// Low sun: dawn (8 quarters) or dusk (12), each quarter as likely.
		const int32 Quarter = Random.RandRange(0, 19);
		Conditions.TimeOfDayMinutes = Quarter < 8 ? (6 * 4 + Quarter) * 15 : (18 * 4 + Quarter - 8) * 15;
	}
	else
	{
		// 21:00 through 05:45, across midnight.
		Conditions.TimeOfDayMinutes = QuarterIn(21 * 4, 30 * 4 - 1);
	}
	return Conditions.Clamped();
}

bool UApexDemoModeSubsystem::ChooseTrack(const UApexNetSubsystem& Net)
{
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	if (!Flow)
	{
		return false;
	}

	struct FCandidate
	{
		const FApexTrackConfigSummary* Track;
		FString Stem;
	};
	TArray<FCandidate> Candidates;
	for (const FApexTrackConfigSummary& Track : Net.GetCachedLobbyState().TrackConfigs)
	{
		FApexTrackCatalogRow Row;
		if (!Flow->GetTrackCatalogRow(Track.Id, Row) || Row.YamlBaseName.IsEmpty())
		{
			continue;
		}
		// A track with neither a cooked level nor a runtime export would be
		// cars racing through the void.
		if (Content && Content->HasTrack(Row.YamlBaseName))
		{
			Candidates.Add({ &Track, Row.YamlBaseName });
		}
	}
	if (Candidates.Num() == 0)
	{
		if (!bWarnedNoTracks)
		{
			bWarnedNoTracks = true;
			UE_LOG(LogApexSim, Warning, TEXT("Demo mode: no lobby track has a catalog row and a cooked level or runtime export; the menu stays on its plain background"));
		}
		return false;
	}

	const FCandidate* Chosen = Candidates.FindByPredicate([Flow](const FCandidate& C) {
		return Flow->HasPendingTrack() && C.Track->Id.Equals(Flow->GetPendingTrackId(), ESearchCase::IgnoreCase);
	});
	if (!Chosen)
	{
		// No track of the player's to show: go round the others.
		int32 Pick = FMath::RandRange(0, Candidates.Num() - 1);
		if (Candidates.Num() > 1 && Candidates[Pick].Track->Id == LastTrackId)
		{
			Pick = (Pick + 1) % Candidates.Num();
		}
		Chosen = &Candidates[Pick];
	}

	TrackId = Chosen->Track->Id;
	TrackStem = Chosen->Stem;
	Centerline = Chosen->Track->Centerline;
	return true;
}

void UApexDemoModeSubsystem::ScanLocalFiles()
{
	if (bFilesScanned)
	{
		return;
	}
	bFilesScanned = true;
	LocalFiles.Reset();
	TArray<FString> Files = UApexSpectatorSubsystem::FindShowcaseFiles();
	if (!ForcedShowcase.IsEmpty() && IFileManager::Get().FileExists(*ForcedShowcase))
	{
		Files.Reset();
		Files.Add(FPaths::ConvertRelativePathToFull(ForcedShowcase));
	}
	for (const FString& Path : Files)
	{
		FApexStreamHeader Header;
		FApexStreamRoster Roster;
		FString Error;
		if (!FApexStreamFile::ReadPreamble(Path, Header, Roster, Error))
		{
			UE_LOG(LogApexSim, Warning, TEXT("Demo mode: %s is not a showcase file: %s"), *Path, *Error);
			continue;
		}
		FFileCandidate& Candidate = LocalFiles.AddDefaulted_GetRef();
		Candidate.Path = Path;
		Candidate.TrackStem = Header.Track.Stem.IsEmpty() ? StemOfShowcaseFile(Path) : Header.Track.Stem;
		Candidate.TrackId = Header.Track.TrackId;
		Candidate.TrackCrc = Header.Track.SourceCrc;
		for (const FApexStreamRosterEntry& Entry : Roster.Entries)
		{
			Candidate.Cars.AddUnique(TPair<FString, int64>(Entry.CarConfigId, static_cast<int64>(Entry.ContentCrc)));
		}
	}
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: %d local showcase file(s)"), LocalFiles.Num());
}

UApexDemoModeSubsystem::FContentCrcs UApexDemoModeSubsystem::GatherContentCrcs(const UApexNetSubsystem& Net) const
{
	FContentCrcs Content;
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexTrackContentSubsystem* Tracks = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	if (!Flow)
	{
		return Content;
	}
	// Every track with an export on this machine, by id: the catalog is keyed
	// by it whether the server lists the track or not.
	for (const FFileCandidate& File : LocalFiles)
	{
		FApexTrackCatalogRow Row;
		if (Flow->GetTrackCatalogRow(File.TrackId, Row) && !Row.YamlBaseName.IsEmpty() && Tracks && Tracks->HasTrack(Row.YamlBaseName))
		{
			Content.Tracks.Add(File.TrackId, TPair<FString, int64>(Row.YamlBaseName, Row.SourceCrc));
		}
		for (const TPair<FString, int64>& Car : File.Cars)
		{
			FApexCarCatalogRow CarRow;
			if (Flow->GetCarCatalogRow(Car.Key, CarRow))
			{
				Content.Cars.Add(Car.Key, CarRow.SourceCrc);
			}
		}
	}
	return Content;
}

int32 UApexDemoModeSubsystem::ChooseFile(const TArray<FFileCandidate>& Files, const FContentCrcs& Content,
	const FString& PendingTrackId, const FString& Avoid, TArray<FString>* OutWhySkipped)
{
	TArray<int32> Playable;
	for (int32 i = 0; i < Files.Num(); ++i)
	{
		const FFileCandidate& File = Files[i];
		const TPair<FString, int64>* Track = Content.Tracks.Find(File.TrackId);
		FString Why;
		if (!Track)
		{
			Why = TEXT("its track has no export here");
		}
		else if (File.TrackCrc != 0 && Track->Value != 0 && File.TrackCrc != Track->Value)
		{
			Why = TEXT("its track has changed since it was rendered");
		}
		for (const TPair<FString, int64>& Car : File.Cars)
		{
			if (!Why.IsEmpty())
			{
				break;
			}
			const int64* Crc = Content.Cars.Find(Car.Key);
			if (!Crc)
			{
				Why = FString::Printf(TEXT("car %s is not here"), *Car.Key);
			}
			else if (Car.Value != 0 && *Crc != 0 && Car.Value != *Crc)
			{
				Why = FString::Printf(TEXT("car %s has changed since it was rendered"), *Car.Key);
			}
		}
		if (!Why.IsEmpty())
		{
			if (OutWhySkipped)
			{
				OutWhySkipped->Add(FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(File.Path), *Why));
			}
			continue;
		}
		Playable.Add(i);
	}
	if (Playable.Num() == 0)
	{
		return INDEX_NONE;
	}
	// The player's own track first, then anything but the one just played.
	for (int32 i : Playable)
	{
		if (!PendingTrackId.IsEmpty() && Files[i].TrackId.Equals(PendingTrackId, ESearchCase::IgnoreCase) && Files[i].Path != Avoid)
		{
			return i;
		}
	}
	for (int32 i : Playable)
	{
		if (!PendingTrackId.IsEmpty() && Files[i].TrackId.Equals(PendingTrackId, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	for (int32 i : Playable)
	{
		if (Files[i].Path != Avoid)
		{
			return i;
		}
	}
	return Playable[0];
}

bool UApexDemoModeSubsystem::StartShowcase(UApexNetSubsystem& Net)
{
	UApexSpectatorSubsystem* Spectator = GetSpectator();
	if (!Spectator || bNoShowcase || !Net.IsAuthenticated() || !Net.IsShowcaseAvailable())
	{
		return false;
	}
	if (!bShowcasesKnown)
	{
		if (!bShowcasesAsked)
		{
			bShowcasesAsked = true;
			SecondsSinceAsked = 0.0f;
			Net.ListShowcases();
		}
		// The answer comes on the next tick or so; try again then.
		return false;
	}
	const TArray<FApexShowcaseSummary>& Channels = Net.GetShowcases();
	if (Channels.Num() == 0)
	{
		return false;
	}
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	const FString Pending = Flow && Flow->HasPendingTrack() ? Flow->GetPendingTrackId() : FString();

	// A channel this client can draw: its track has a level here.
	auto StemOf = [&](const FApexShowcaseSummary& Channel, FString& OutStem)
	{
		FApexTrackCatalogRow Row;
		if (Flow && Flow->GetTrackCatalogRow(Channel.TrackId, Row) && !Row.YamlBaseName.IsEmpty()
			&& Content && Content->HasTrack(Row.YamlBaseName))
		{
			OutStem = Row.YamlBaseName;
			return true;
		}
		return false;
	};
	const FApexShowcaseSummary* Chosen = nullptr;
	FString Stem;
	if (!ForcedShowcase.IsEmpty())
	{
		Chosen = Channels.FindByPredicate([this](const FApexShowcaseSummary& C) { return C.Id.Equals(ForcedShowcase, ESearchCase::IgnoreCase); });
		if (Chosen && !StemOf(*Chosen, Stem))
		{
			Chosen = nullptr;
		}
	}
	if (!Chosen && !Pending.IsEmpty())
	{
		for (const FApexShowcaseSummary& Channel : Channels)
		{
			if (Channel.TrackId.Equals(Pending, ESearchCase::IgnoreCase) && Channel.Id != LastShowcaseId && StemOf(Channel, Stem))
			{
				Chosen = &Channel;
				break;
			}
		}
	}
	if (!Chosen)
	{
		for (const FApexShowcaseSummary& Channel : Channels)
		{
			if ((Channel.Id != LastShowcaseId || Channels.Num() == 1) && StemOf(Channel, Stem))
			{
				Chosen = &Channel;
				break;
			}
		}
	}
	if (!Chosen)
	{
		return false;
	}
	TrackId = Chosen->TrackId;
	TrackStem = Stem;
	Centerline.Reset();
	FApexTrackConfigSummary Track;
	if (Net.FindTrackById(TrackId, Track))
	{
		Centerline = Track.Centerline;
	}
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: watching showcase %s on %s (%s)"), *Chosen->Id, *TrackStem, *Chosen->Conditions.Describe());
	if (!Spectator->WatchShowcase(Chosen->Id))
	{
		return false;
	}
	Source = EApexBackdropSource::Showcase;
	LastShowcaseId = Chosen->Id;
	return true;
}

bool UApexDemoModeSubsystem::StartLocalFile(UApexNetSubsystem& Net)
{
	UApexSpectatorSubsystem* Spectator = GetSpectator();
	if (!Spectator || bNoShowcase)
	{
		return false;
	}
	ScanLocalFiles();
	if (LocalFiles.Num() == 0)
	{
		return false;
	}
	const UApexMenuFlowSubsystem* Flow = GetFlow();
	const FString Pending = Flow && Flow->HasPendingTrack() ? Flow->GetPendingTrackId() : FString();
	TArray<FString> Skipped;
	const int32 Pick = ChooseFile(LocalFiles, GatherContentCrcs(Net), Pending, LastFile, &Skipped);
	for (const FString& Why : Skipped)
	{
		UE_LOG(LogApexSim, Log, TEXT("Demo mode: skipping %s"), *Why);
	}
	if (Pick == INDEX_NONE)
	{
		return false;
	}
	const FFileCandidate& File = LocalFiles[Pick];
	TrackId = File.TrackId;
	TrackStem = File.TrackStem;
	// The cameras' path comes with the stream; the lobby's is a fallback.
	Centerline.Reset();
	FApexTrackConfigSummary Track;
	if (Net.FindTrackById(TrackId, Track))
	{
		Centerline = Track.Centerline;
	}
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: playing %s"), *File.Path);
	if (!Spectator->PlayFile(File.Path))
	{
		return false;
	}
	Source = EApexBackdropSource::LocalFile;
	LastFile = File.Path;
	return true;
}

bool UApexDemoModeSubsystem::StartDemoSession(UApexNetSubsystem& Net)
{
	if (!Net.IsAuthenticated() || Net.GetCachedLobbyState().TrackConfigs.Num() == 0 || !ChooseTrack(Net))
	{
		return false;
	}
	UApexMenuFlowSubsystem* Flow = GetFlow();
	// The field races in the class of the car the player picked.
	if (Flow && Flow->HasPendingCar())
	{
		Net.SelectCar(Flow->GetPendingCarId());
	}
	FApexSessionConditions Conditions;
	if (CVarDemoRandomSky.GetValueOnGameThread() != 0)
	{
		FRandomStream Random(static_cast<int32>(FPlatformTime::Cycles()));
		Conditions = RollConditions(Random);
	}
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: starting an AI race on %s, %s"), *TrackStem, *Conditions.Describe());
	Net.CreateDemoSession(TrackId, CVarDemoAiCount.GetValueOnGameThread(), CVarDemoLaps.GetValueOnGameThread(), Conditions);
	Source = EApexBackdropSource::DemoSession;
	return true;
}

bool UApexDemoModeSubsystem::StartBackdrop(UApexNetSubsystem& Net)
{
	if (StartShowcase(Net))
	{
		return true;
	}
	// The server's showcase comes first when there is a server: give a
	// connection under way, and a channel list asked for, a moment before
	// falling back to a file. A startup offline is not held up: the connect
	// fails in under a second and the file plays.
	const EApexConnectionState State = Net.GetConnectionState();
	const bool bConnecting = State == EApexConnectionState::Connecting || State == EApexConnectionState::Authenticating
		|| (State == EApexConnectionState::Authenticated && Net.GetCachedLobbyState().TrackConfigs.Num() == 0);
	const bool bListPending = Net.IsAuthenticated() && Net.IsShowcaseAvailable() && bShowcasesAsked && !bShowcasesKnown;
	if (!bNoShowcase && ((bConnecting && SecondsSinceInit < ServerGraceSeconds) || (bListPending && SecondsSinceAsked < ServerGraceSeconds)))
	{
		return false;
	}
	return StartLocalFile(Net) || StartDemoSession(Net);
}

bool UApexDemoModeSubsystem::PlayReplay(const FString& Path, FString& OutError)
{
	UApexNetSubsystem* Net = GetNet();
	UApexSpectatorSubsystem* Spectator = GetSpectator();
	if (!Net || !Spectator)
	{
		OutError = TEXT("the game is not ready");
		return false;
	}
	const AApexRaceDirector* Director = GetDirector();
	if (!Director || Net->IsInSession() || Net->IsSessionRequestPending()
		|| (Director->IsRaceViewActive() && !Director->IsDemoViewActive()))
	{
		OutError = TEXT("leave the session first");
		return false;
	}
	FApexStreamHeader Header;
	FApexStreamRoster Roster;
	if (!FApexStreamFile::ReadPreamble(Path, Header, Roster, OutError))
	{
		return false;
	}
	const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
	if (!Content || !Content->HasTrack(Header.Track.Stem))
	{
		OutError = FString::Printf(TEXT("%s is not installed on this machine"), *Header.Track.DisplayName);
		return false;
	}
	Teardown(*Net);
	TrackId = Header.Track.TrackId;
	TrackStem = Header.Track.Stem;
	Centerline.Reset();
	FApexTrackConfigSummary Track;
	if (Net->FindTrackById(TrackId, Track))
	{
		Centerline = Track.Centerline;
	}
	if (!Spectator->PlayFile(Path, /*bInLoop*/ false))
	{
		OutError = TEXT("the file is gone");
		return false;
	}
	Source = EApexBackdropSource::Replay;
	Cooldown = 0.0f;
	Failures = 0;
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: playing the replay %s"), *Path);
	return true;
}

void UApexDemoModeSubsystem::StopReplay()
{
	if (Source != EApexBackdropSource::Replay)
	{
		return;
	}
	if (UApexNetSubsystem* Net = GetNet())
	{
		Teardown(*Net);
	}
	// The ordinary backdrop comes back on the next tick or so.
	Cooldown = 0.5f;
}

void UApexDemoModeSubsystem::Teardown(UApexNetSubsystem& Net)
{
	if (UApexSpectatorSubsystem* Spectator = GetSpectator())
	{
		Spectator->Stop();
	}
	Net.LeaveDemoSession();
	Source = EApexBackdropSource::None;
}

bool UApexDemoModeSubsystem::Tick(float DeltaSeconds)
{
	UApexNetSubsystem* Net = GetNet();
	UApexSpectatorSubsystem* Spectator = GetSpectator();
	if (!Net)
	{
		return true;
	}
	Cooldown = FMath::Max(0.0f, Cooldown - DeltaSeconds);
	SecondsSinceInit += DeltaSeconds;
	SecondsSinceAsked += DeltaSeconds;
	if (Net->GetConnectionState() != EApexConnectionState::Authenticated)
	{
		// The channels are the server's: ask again on the next connection.
		bShowcasesAsked = false;
		bShowcasesKnown = false;
	}

	const bool bStreaming = Spectator && Spectator->GetSource() != EApexSpectatorSource::None;
	const bool bRunning = bStreaming || Net->IsInDemoSession() || Net->IsDemoSessionRequested();
	if (!IsDemoAllowed(*Net))
	{
		if (bRunning && !Net->IsSessionRequestPending())
		{
			// Turned off, or disconnected mid-demo: a session request already
			// left it on its own.
			Teardown(*Net);
		}
		bRestarting = false;
		return true;
	}

	if (!bRunning)
	{
		Source = EApexBackdropSource::None;
		if (Cooldown <= 0.0f)
		{
			if (StartBackdrop(*Net))
			{
				// Until the answer: a request that fails backs off further each time.
				Cooldown = FMath::Min(5.0f * FMath::Pow(2.0f, static_cast<float>(Failures)), 120.0f);
				++Failures;
			}
			else
			{
				// Waiting on the server, or nothing to play yet: look again soon.
				Cooldown = 0.25f;
			}
		}
		return true;
	}

	if (bStreaming && Spectator->HasFailed())
	{
		UE_LOG(LogApexSim, Warning, TEXT("Demo mode: the backdrop could not be played (%s); trying another"), *Spectator->GetFailure());
		Teardown(*Net);
		Cooldown = FMath::Max(Cooldown, 1.0f);
		return true;
	}

	if (!Net->IsInDemoSession() || !bViewBegun)
	{
		return true;
	}
	if (Source == EApexBackdropSource::Replay)
	{
		// The player's own choice: it runs until they leave it, and the
		// backdrop's reasons to move on do not apply.
		if (Spectator)
		{
			Spectator->TakeNextRequested();
		}
		return true;
	}
	RunningFor += DeltaSeconds;
	FinishedFor = Net->GetDemoSessionState() == EApexSessionState::Finished ? FinishedFor + DeltaSeconds : 0.0f;

	AApexRaceDirector* Director = GetDirector();
	if (bRestarting)
	{
		if (!Director || Director->GetDemoBackdropOpacity() <= 0.0f)
		{
			Teardown(*Net);
		}
		return true;
	}

	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (Spectator && Spectator->TakeNextRequested())
	{
		Restart(TEXT("the next one was asked for"));
	}
	else if (FinishedFor > FinishedLingerSeconds)
	{
		Restart(TEXT("the race is over"));
	}
	else if (RunningFor > CVarDemoMaxMinutes.GetValueOnGameThread() * 60.0f)
	{
		Restart(TEXT("it has run long enough"));
	}
	else if (Flow && Flow->HasPendingTrack() && !Flow->GetPendingTrackId().Equals(TrackId, ESearchCase::IgnoreCase))
	{
		// The player picked another circuit; show that one, if it has a level
		// and something to play on it.
		FApexTrackCatalogRow Row;
		const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
		const bool bHasLevel = Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) && !Row.YamlBaseName.IsEmpty() && Content
			&& Content->HasTrack(Row.YamlBaseName);
		bool bHasRace = Source == EApexBackdropSource::DemoSession;
		if (Source == EApexBackdropSource::Showcase)
		{
			bHasRace = Net->GetShowcases().ContainsByPredicate([Flow](const FApexShowcaseSummary& C) {
				return C.TrackId.Equals(Flow->GetPendingTrackId(), ESearchCase::IgnoreCase);
			});
		}
		else if (Source == EApexBackdropSource::LocalFile)
		{
			bHasRace = LocalFiles.ContainsByPredicate([Flow](const FFileCandidate& F) {
				return F.TrackId.Equals(Flow->GetPendingTrackId(), ESearchCase::IgnoreCase);
			});
		}
		if (bHasLevel && bHasRace)
		{
			Restart(TEXT("the player chose another track"));
		}
	}
	return true;
}

void UApexDemoModeSubsystem::Restart(const TCHAR* Why)
{
	UE_LOG(LogApexSim, Log, TEXT("Demo mode: starting over because %s"), Why);
	bRestarting = true;
	if (AApexRaceDirector* Director = GetDirector())
	{
		Director->FadeOutDemo();
	}
}

void UApexDemoModeSubsystem::HandleShowcases(const TArray<FApexShowcaseSummary>& Showcases)
{
	bShowcasesKnown = true;
	// A channel list in hand: no need to wait out the back-off from asking.
	if (Source == EApexBackdropSource::None && Showcases.Num() > 0)
	{
		Cooldown = 0.0f;
	}
}

void UApexDemoModeSubsystem::HandleDemoSessionChanged(bool bJoined)
{
	AApexRaceDirector* Director = GetDirector();
	if (bJoined)
	{
		Failures = 0;
		RunningFor = 0.0f;
		FinishedFor = 0.0f;
		bRestarting = false;
		LastTrackId = TrackId;
		// A stream brings its own path for the cameras; it beats the lobby's.
		if (const UApexSpectatorSubsystem* Spectator = GetSpectator())
		{
			if (Spectator->GetSource() != EApexSpectatorSource::None && Spectator->GetCenterline().Num() > 0)
			{
				Centerline = Spectator->GetCenterline();
			}
			if (Spectator->GetSource() != EApexSpectatorSource::None && TrackStem.IsEmpty() && Spectator->HasHeader())
			{
				TrackStem = Spectator->GetHeader().Track.Stem;
			}
		}
		if (Director && !TrackStem.IsEmpty())
		{
			Director->BeginDemoView(TrackStem, Centerline);
			bViewBegun = Director->IsDemoViewActive();
		}
		return;
	}

	if (bViewBegun && Director)
	{
		Director->EndDemoView();
	}
	bViewBegun = false;
	bRestarting = false;
	// A moment before the next, so a player backing out of a screen that
	// took the connection does not start one per frame.
	Cooldown = FMath::Max(Cooldown, 1.0f);
}
