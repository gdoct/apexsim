#include "ApexDemoModeSubsystem.h"

#include "ApexMenuFlowSubsystem.h"
#include "ApexNetSubsystem.h"
#include "ApexSim.h"
#include "Catalog/ApexCatalogRows.h"
#include "Engine/GameInstance.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
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
		TEXT("Cars in the menu's demo race (the circuit's grid may hold fewer)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDemoLaps(
		TEXT("apexsim.demo.Laps"),
		3,
		TEXT("Laps in each demo race before the next one starts."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDemoRandomSky(
		TEXT("apexsim.demo.RandomSky"),
		1,
		TEXT("1 gives each demo race a random weather and time of day; 0 races under the default sunny 13:00."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarDemoMaxMinutes(
		TEXT("apexsim.demo.MaxMinutes"),
		12.0f,
		TEXT("A demo race still running after this long is restarted (a car stuck off the road never finishes)."),
		ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs DemoRestartCommand(
		TEXT("apexsim.demo.Restart"),
		TEXT("Start the menu's demo race over."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
			{
				const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
				if (UApexNetSubsystem* Net = GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr)
				{
					Net->LeaveDemoSession();
				}
			}));

	/** Seconds a finished demo race lingers on its cool-down laps before the next. */
	constexpr float FinishedLingerSeconds = 6.0f;
}

void UApexDemoModeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexMenuFlowSubsystem>();

	if (UApexNetSubsystem* Net = GetNet())
	{
		DemoChangedHandle = Net->OnDemoSessionChanged.AddUObject(this, &UApexDemoModeSubsystem::HandleDemoSessionChanged);
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

AApexRaceDirector* UApexDemoModeSubsystem::GetDirector() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? AApexRaceDirector::Find(GameInstance->GetWorld()) : nullptr;
}

bool UApexDemoModeSubsystem::IsDemoAllowed(const UApexNetSubsystem& Net) const
{
	if (IsDemoDisabled())
	{
		return false;
	}
	const AApexRaceDirector* Director = GetDirector();
	return Director
		&& Net.IsAuthenticated()
		&& !Net.IsInSession()
		&& !Net.IsSessionRequestPending()
		&& Net.GetCachedLobbyState().TrackConfigs.Num() > 0
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
	switch (Net->GetConnectionState())
	{
	case EApexConnectionState::Connecting:
	case EApexConnectionState::Authenticating:
		return true;
	case EApexConnectionState::Authenticated:
		// A failed create backs off for seconds; a startup should not wait it out.
		return !bWarnedNoTracks && (Failures == 0 || Net->IsInDemoSession() || Net->IsDemoSessionRequested());
	default:
		// Disconnected (nobody asked to connect), Failed, or Reconnecting after
		// the server could not be reached.
		return false;
	}
}

bool UApexDemoModeSubsystem::IsDemoDisabled()
{
	static const bool bCommandLineOff = FParse::Param(FCommandLine::Get(), TEXT("ApexNoDemo"))
		// An unattended run is there to look at something else.
		|| FParse::Param(FCommandLine::Get(), TEXT("ApexAutoRace"))
		// A replay clip plays offline, with no server to race a demo on.
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

bool UApexDemoModeSubsystem::Tick(float DeltaSeconds)
{
	UApexNetSubsystem* Net = GetNet();
	if (!Net)
	{
		return true;
	}
	Cooldown = FMath::Max(0.0f, Cooldown - DeltaSeconds);

	const bool bDemoOnServer = Net->IsInDemoSession() || Net->IsDemoSessionRequested();
	if (!IsDemoAllowed(*Net))
	{
		if (bDemoOnServer && !Net->IsSessionRequestPending())
		{
			// Turned off, or disconnected mid-demo: a session request already
			// left it on its own.
			Net->LeaveDemoSession();
		}
		bRestarting = false;
		return true;
	}

	if (!bDemoOnServer)
	{
		if (Cooldown <= 0.0f && ChooseTrack(*Net))
		{
			UApexMenuFlowSubsystem* Flow = GetFlow();
			// The field races in the class of the car the player picked.
			if (Flow && Flow->HasPendingCar())
			{
				Net->SelectCar(Flow->GetPendingCarId());
			}
			FApexSessionConditions Conditions;
			if (CVarDemoRandomSky.GetValueOnGameThread() != 0)
			{
				FRandomStream Random(static_cast<int32>(FPlatformTime::Cycles()));
				Conditions = RollConditions(Random);
			}
			UE_LOG(LogApexSim, Log, TEXT("Demo mode: starting an AI race on %s, %s"), *TrackStem, *Conditions.Describe());
			Net->CreateDemoSession(TrackId, CVarDemoAiCount.GetValueOnGameThread(), CVarDemoLaps.GetValueOnGameThread(), Conditions);
			// Until the answer: a request that fails backs off further each time.
			Cooldown = FMath::Min(5.0f * FMath::Pow(2.0f, static_cast<float>(Failures)), 120.0f);
			++Failures;
		}
		return true;
	}

	if (!Net->IsInDemoSession() || !bViewBegun)
	{
		return true;
	}
	RunningFor += DeltaSeconds;
	FinishedFor = Net->GetDemoSessionState() == EApexSessionState::Finished ? FinishedFor + DeltaSeconds : 0.0f;

	AApexRaceDirector* Director = GetDirector();
	if (bRestarting)
	{
		if (!Director || Director->GetDemoBackdropOpacity() <= 0.0f)
		{
			Net->LeaveDemoSession();
		}
		return true;
	}

	const UApexMenuFlowSubsystem* Flow = GetFlow();
	if (FinishedFor > FinishedLingerSeconds)
	{
		Restart(TEXT("the race is over"));
	}
	else if (RunningFor > CVarDemoMaxMinutes.GetValueOnGameThread() * 60.0f)
	{
		Restart(TEXT("it has run long enough"));
	}
	else if (Flow && Flow->HasPendingTrack() && !Flow->GetPendingTrackId().Equals(TrackId, ESearchCase::IgnoreCase))
	{
		// The player picked another circuit; show that one, if it has a level.
		FApexTrackCatalogRow Row;
		const UApexTrackContentSubsystem* Content = GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>();
		if (Flow->GetTrackCatalogRow(Flow->GetPendingTrackId(), Row) && !Row.YamlBaseName.IsEmpty() && Content
			&& Content->HasTrack(Row.YamlBaseName))
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
