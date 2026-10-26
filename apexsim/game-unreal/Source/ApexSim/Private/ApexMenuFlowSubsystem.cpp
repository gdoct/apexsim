#include "ApexMenuFlowSubsystem.h"

#include "ApexBootSettings.h"
#include "ApexDriverKey.h"
#include "ApexNetSubsystem.h"
#include "ApexProfileSave.h"
#include "ApexSettingsSubsystem.h"
#include "ApexSim.h"
#include "Algo/AnyOf.h"
#include "Engine/DataTable.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "Track/ApexTrackContentSubsystem.h"
#include "UI/ApexCreateSessionModel.h"

namespace
{
	const TCHAR* TrackCatalogPath = TEXT("/Game/Data/DT_TrackCatalog.DT_TrackCatalog");

	UApexMenuFlowSubsystem* FlowOf(const UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UApexMenuFlowSubsystem>() : nullptr;
	}

	/** A profile by name (any case) or 1-based place in the list. */
	const FApexProfileEntry* FindProfile(const UApexMenuFlowSubsystem& Flow, const FString& Wanted)
	{
		const TArray<FApexProfileEntry>& Profiles = Flow.GetProfiles();
		if (const FApexProfileEntry* ByName = Profiles.FindByPredicate([&Wanted](const FApexProfileEntry& E) { return E.Name.Equals(Wanted, ESearchCase::IgnoreCase); }))
		{
			return ByName;
		}
		const int32 Place = Wanted.IsNumeric() ? FCString::Atoi(*Wanted) - 1 : INDEX_NONE;
		return Profiles.IsValidIndex(Place) ? &Profiles[Place] : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs ProfileListCommand(TEXT("apexsim.profile.List"),
		TEXT("Log the driver profiles on this machine and the one in use."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (const UApexMenuFlowSubsystem* Flow = FlowOf(World))
			{
				int32 Place = 1;
				for (const FApexProfileEntry& Entry : Flow->GetProfiles())
				{
					UE_LOG(LogApexSim, Display, TEXT("%d. %s%s  picture %d  %s  (%s / %s)"), Place++, *Entry.Name,
						Entry.Id == Flow->GetProfileId() ? TEXT(" [driving]") : TEXT(""), Entry.Portrait,
						*ApexProfiles::WireId(Entry.Id), *Entry.ProfileSlot, *Entry.SettingsSlot);
				}
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ProfileNewCommand(TEXT("apexsim.profile.New"),
		TEXT("apexsim.profile.New <name> [picture]: make a driver profile and drive as it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexMenuFlowSubsystem* Flow = FlowOf(World); Flow && Args.Num() >= 1)
			{
				Flow->CreateProfile(Args[0], Args.Num() >= 2 ? FCString::Atoi(*Args[1]) : 0);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ProfileUseCommand(TEXT("apexsim.profile.Use"),
		TEXT("apexsim.profile.Use <name or place>: drive as another profile."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexMenuFlowSubsystem* Flow = FlowOf(World);
			const FApexProfileEntry* Entry = Flow && Args.Num() >= 1 ? FindProfile(*Flow, Args[0]) : nullptr;
			if (Entry)
			{
				Flow->SwitchProfile(Entry->Id);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ProfileDeleteCommand(TEXT("apexsim.profile.Delete"),
		TEXT("apexsim.profile.Delete <name or place>: delete a profile that is not in use, with its saves."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UApexMenuFlowSubsystem* Flow = FlowOf(World);
			const FApexProfileEntry* Entry = Flow && Args.Num() >= 1 ? FindProfile(*Flow, Args[0]) : nullptr;
			if (Entry)
			{
				Flow->DeleteProfile(Entry->Id);
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs ProfilePictureCommand(TEXT("apexsim.profile.Picture"),
		TEXT("apexsim.profile.Picture <n>: the profile in use's picture, 1-based (0 none)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UApexMenuFlowSubsystem* Flow = FlowOf(World); Flow && Args.Num() >= 1)
			{
				Flow->SetPortrait(FCString::Atoi(*Args[0]));
			}
		}));
}

void UApexMenuFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// settings.yml is the authority on the server address, so it has to be read
	// before LoadProfile() decides which one to connect to.
	Collection.InitializeDependency<UApexBootSettingsSubsystem>();
	// Tracks found on disk at runtime join the catalog through it.
	Collection.InitializeDependency<UApexTrackContentSubsystem>();
	// The profile's id goes out with the first Authenticate, and its
	// settings slot is the settings subsystem's.
	Collection.InitializeDependency<UApexNetSubsystem>();
	Collection.InitializeDependency<UApexSettingsSubsystem>();

	// LoadObject rather than a constructor finder: the tables are authored in
	// the editor after this C++ is first compiled, so they legitimately do not
	// exist on the first run and a missing table must not be fatal.
	TrackCatalog = LoadObject<UDataTable>(nullptr, TrackCatalogPath);

	if (!TrackCatalog)
	{
		UE_LOG(LogApexSim, Warning, TEXT("No track catalog at %s — tracks will render without previews"), TrackCatalogPath);
	}

	LoadProfile();
}

void UApexMenuFlowSubsystem::SetPendingCar(const FString& CarId)
{
	if (PendingCarId.Equals(CarId, ESearchCase::IgnoreCase))
	{
		return;
	}
	PendingCarId = CarId;
	SaveProfile();
	OnPendingCarChanged.Broadcast(PendingCarId);
}

void UApexMenuFlowSubsystem::SetPendingTrack(const FString& TrackId)
{
	if (PendingTrackId.Equals(TrackId, ESearchCase::IgnoreCase))
	{
		return;
	}
	PendingTrackId = TrackId;
	SaveProfile();
	OnPendingTrackChanged.Broadcast(PendingTrackId);
}

void UApexMenuFlowSubsystem::LoadProfile()
{
	ProfileIndex = ApexProfiles::LoadIndex();
	const FApexProfileEntry* Entry = ApexProfiles::Chosen(*ProfileIndex, FCommandLine::Get());
	check(Entry);
	LoadProfileEntry(*Entry);
	PushIdentityToNet(false);
}

void UApexMenuFlowSubsystem::LoadProfileEntry(const FApexProfileEntry& Entry)
{
	ProfileSlot = Entry.ProfileSlot;
	Profile = nullptr;
	if (UGameplayStatics::DoesSaveGameExist(ProfileSlot, 0))
	{
		Profile = Cast<UApexProfileSave>(UGameplayStatics::LoadGameFromSlot(ProfileSlot, 0));
	}

	if (!Profile)
	{
		// First run, or a slot written by an incompatible build. Either way the
		// defaults above stand and the next SaveProfile writes a fresh one.
		Profile = Cast<UApexProfileSave>(UGameplayStatics::CreateSaveGameObject(UApexProfileSave::StaticClass()));
		Profile->DriverName = Entry.Name;
		Profile->DriverId = Entry.Id;
		Profile->Portrait = Entry.Portrait;
		PlayerName = Entry.Name;
		UE_LOG(LogApexSim, Log, TEXT("No profile in slot '%s' — starting from defaults"), *ProfileSlot);
		AdoptBootSettings();
		return;
	}
	// The index's id is the profile's: a save from before profiles has none.
	Profile->DriverId = Entry.Id;

	PendingCarId = Profile->LastCarId;
	PendingTrackId = Profile->LastTrackId;
	PlayerName = Profile->DriverName;
	ServerHost = Profile->ServerHost;
	ServerPort = Profile->ServerPort;
	CreateMaxPlayers = Profile->MaxPlayers;
	CreateAiCount = Profile->AiCount;
	CreateLapLimit = Profile->LapLimit;
	bCreateTimedRace = Profile->bTimedRace;
	CreateRaceMinutes = FMath::Clamp(Profile->RaceMinutes, ApexRaceLength::MinSeconds / 60, ApexRaceLength::MaxSeconds / 60);
	CreateQualifyingMinutes = ApexQualifyingLength::Clamp(Profile->QualifyingMinutes * 60) / 60;
	bCreateWeekend = Profile->bWeekend;
	CreateWeekendPracticeMinutes = ApexQualifyingLength::Clamp(Profile->WeekendPracticeMinutes * 60) / 60;
	CreateWeekendRounds = FMath::Clamp(Profile->WeekendRounds, 1, FApexWeekendFormat::MaxRounds);
	CreateWeekendRoundMinutes = FMath::Max(1, ApexQualifyingLength::Clamp(Profile->WeekendRoundMinutes * 60) / 60);
	CreateWeekendPracticeWeather = Profile->WeekendPracticeWeather;
	CreateWeekendPracticeHour = Profile->WeekendPracticeHour;
	CreateWeekendQualifyingWeather = Profile->WeekendQualifyingWeather;
	CreateWeekendQualifyingHour = Profile->WeekendQualifyingHour;
	CreateStartingMode = Profile->StartingMode;
	CreateSessionKind = Profile->SessionKind;
	CreateAllowedAssists = Profile->AllowedAssists;
	CreateConditions = Profile->Conditions.Clamped();
	CreateDamage = Profile->Damage;
	bCreateQualifyingCutoff = Profile->bQualifyingCutoff;
	CreateMandatoryStops = ApexMandatoryStops::Clamp(Profile->MandatoryStops);
	bCreateCompoundChange = Profile->bCompoundChange;
	CreateRaceRules = Profile->RaceRules.Normalised();
	CreateAiSkill = ApexAiSkill::Clamp(Profile->AiSkill);
	CreateClassAi.Reset();
	for (const FString& Line : Profile->ClassAi)
	{
		FString Class;
		FString Count;
		if (Line.Split(TEXT("="), &Class, &Count) && !Class.TrimStartAndEnd().IsEmpty())
		{
			CreateClassAi.Add(Class.TrimStartAndEnd(), FMath::Clamp(FCString::Atoi(*Count), 0, 255));
		}
	}
	CreateQualifyingFormat = Profile->QualifyingFormat == EApexQualifyingFormat::Shootout ? EApexQualifyingFormat::Shootout : EApexQualifyingFormat::Open;
	CreateQualifyingLapAllowance = ApexQualifyingLapAllowance::Clamp(Profile->QualifyingLapAllowance);
	CreateWeekendPracticeDamage = FMath::Clamp(Profile->WeekendPracticeDamage, -1, static_cast<int32>(EApexDamageLevel::VisualOnly));
	CreateWeekendQualifyingDamage = FMath::Clamp(Profile->WeekendQualifyingDamage, -1, static_cast<int32>(EApexDamageLevel::VisualOnly));
	CreateWeekendPracticeAssists = ApexCreateSession::PhaseAssistOfChip(ApexCreateSession::PhaseAssistChip(Profile->WeekendPracticeAssists));
	CreateWeekendQualifyingAssists = ApexCreateSession::PhaseAssistOfChip(ApexCreateSession::PhaseAssistChip(Profile->WeekendQualifyingAssists));

	// A profile written before demo lap was locked would otherwise start a mode
	// the server turns into a dead end for the player who asked for it; one
	// from before Sandbox left the mode list, a session of standing cars.
	if (CreateStartingMode == EApexGameMode::DemoLap || CreateStartingMode == EApexGameMode::Sandbox)
	{
		CreateStartingMode = EApexGameMode::FreePractice;
	}

	AdoptBootSettings();

	UE_LOG(LogApexSim, Log, TEXT("Profile loaded from '%s': driver '%s' (%s), last track '%s', last car '%s', %d best lap(s)"),
		*ProfileSlot, *PlayerName, *ApexProfiles::WireId(Profile->DriverId), *PendingTrackId, *PendingCarId, Profile->BestLapSeconds.Num());
}

UApexNetSubsystem* UApexMenuFlowSubsystem::GetNet() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexNetSubsystem>() : nullptr;
}

void UApexMenuFlowSubsystem::PushIdentityToNet(bool bReconnect)
{
	UApexNetSubsystem* Net = GetNet();
	if (!Net || !Profile)
	{
		return;
	}
	// A profile from before keys gets its key the first time it is used.
	if (Profile->DriverKeySeed.Num() != ApexDriverKey::KeyBytes && ApexDriverKey::Generate(Profile->DriverKeySeed))
	{
		SaveProfile();
	}
	Net->SetDriverId(ApexProfiles::WireId(Profile->DriverId));
	Net->SetDriverKey(Profile->DriverKeySeed);
	// The server reads the name and the id once per connection: a client
	// that is talking to one says hello again as the driver now in use.
	if (bReconnect && Net->GetConnectionState() != EApexConnectionState::Disconnected)
	{
		if (Net->IsInSession())
		{
			Net->LeaveSession();
		}
		Net->Connect(ServerHost, ServerPort, PlayerName, AuthToken);
	}
}

FApexProfileEntry* UApexMenuFlowSubsystem::CurrentEntry()
{
	if (!ProfileIndex || !Profile)
	{
		return nullptr;
	}
	return ProfileIndex->Profiles.FindByPredicate([this](const FApexProfileEntry& E) { return E.Id == Profile->DriverId; });
}

const TArray<FApexProfileEntry>& UApexMenuFlowSubsystem::GetProfiles() const
{
	static const TArray<FApexProfileEntry> None;
	return ProfileIndex ? ProfileIndex->Profiles : None;
}

const FApexDriverStats& UApexMenuFlowSubsystem::GetStats() const
{
	static const FApexDriverStats None;
	return Profile ? Profile->Stats : None;
}

void UApexMenuFlowSubsystem::SetPortrait(int32 Portrait)
{
	if (!Profile || Profile->Portrait == Portrait)
	{
		return;
	}
	Profile->Portrait = FMath::Max(0, Portrait);
	SaveProfile();
	OnProfileChanged.Broadcast();
}

void UApexMenuFlowSubsystem::SetDriverName(const FString& Name)
{
	const FString Clean = ApexProfiles::CleanName(Name);
	if (Clean == PlayerName)
	{
		return;
	}
	PlayerName = Clean;
	SaveProfile();
	PushIdentityToNet(true);
	OnProfileChanged.Broadcast();
}

FGuid UApexMenuFlowSubsystem::CreateProfile(const FString& Name, int32 Portrait)
{
	if (!ProfileIndex || ProfileIndex->Profiles.Num() >= ApexProfiles::MaxProfiles)
	{
		return FGuid();
	}
	SaveProfile();
	const FApexProfileEntry Entry = ApexProfiles::MakeEntry(Name, Portrait);
	// A new driver starts with no history but on the same server.
	UApexProfileSave* Fresh = Cast<UApexProfileSave>(UGameplayStatics::CreateSaveGameObject(UApexProfileSave::StaticClass()));
	Fresh->DriverName = Entry.Name;
	Fresh->DriverId = Entry.Id;
	Fresh->Portrait = Entry.Portrait;
	ApexDriverKey::Generate(Fresh->DriverKeySeed);
	Fresh->ServerHost = ServerHost;
	Fresh->ServerPort = ServerPort;
	if (!UGameplayStatics::SaveGameToSlot(Fresh, Entry.ProfileSlot, 0))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Profiles: could not write the new profile's slot '%s'"), *Entry.ProfileSlot);
		return FGuid();
	}
	ProfileIndex->Profiles.Add(Entry);
	ApexProfiles::SaveIndex(ProfileIndex);
	UE_LOG(LogApexSim, Log, TEXT("Profiles: made '%s' (%s)"), *Entry.Name, *ApexProfiles::WireId(Entry.Id));
	SwitchProfile(Entry.Id);
	return Entry.Id;
}

bool UApexMenuFlowSubsystem::SwitchProfile(const FGuid& Id)
{
	if (!ProfileIndex)
	{
		return false;
	}
	const FApexProfileEntry* Entry = ProfileIndex->Profiles.FindByPredicate([&Id](const FApexProfileEntry& E) { return E.Id == Id; });
	if (!Entry)
	{
		return false;
	}
	if (Profile && Profile->DriverId == Id)
	{
		return true;
	}
	SaveProfile();
	const FApexProfileEntry Chosen = *Entry;
	LoadProfileEntry(Chosen);
	ProfileIndex->Current = Chosen.Id;
	ApexProfiles::SaveIndex(ProfileIndex);
	if (UApexSettingsSubsystem* Settings = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexSettingsSubsystem>() : nullptr)
	{
		Settings->UseSlot(Chosen.SettingsSlot);
	}
	PushIdentityToNet(true);
	SeasonRoundRunning = INDEX_NONE;
	UE_LOG(LogApexSim, Log, TEXT("Profiles: now driving as '%s'"), *PlayerName);
	OnPendingCarChanged.Broadcast(PendingCarId);
	OnPendingTrackChanged.Broadcast(PendingTrackId);
	OnProfileChanged.Broadcast();
	return true;
}

bool UApexMenuFlowSubsystem::DeleteProfile(const FGuid& Id)
{
	if (!ProfileIndex || (Profile && Profile->DriverId == Id))
	{
		return false;
	}
	const int32 Index = ProfileIndex->Profiles.IndexOfByPredicate([&Id](const FApexProfileEntry& E) { return E.Id == Id; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	const FApexProfileEntry Gone = ProfileIndex->Profiles[Index];
	ProfileIndex->Profiles.RemoveAt(Index);
	ApexProfiles::SaveIndex(ProfileIndex);
	UGameplayStatics::DeleteGameInSlot(Gone.ProfileSlot, 0);
	UGameplayStatics::DeleteGameInSlot(Gone.SettingsSlot, 0);
	UE_LOG(LogApexSim, Log, TEXT("Profiles: deleted '%s'"), *Gone.Name);
	OnProfileChanged.Broadcast();
	return true;
}

bool UApexMenuFlowSubsystem::ConsumeProfilePrompt()
{
	if (bProfilePromptConsumed)
	{
		return false;
	}
	bProfilePromptConsumed = true;
	if (GetProfiles().Num() < 2 || ApexProfiles::CommandLineChoosesProfile(FCommandLine::Get()))
	{
		return false;
	}
	// An unattended run drives the game with -Apex flags of its own; the
	// ones that only set the machine up, or watch, leave the picker to the
	// player (a verify run shoots it with nothing else).
	static const TCHAR* const Observing[] = {
		TEXT("ApexScreens="), TEXT("ApexUpscaler="), TEXT("ApexContent"), TEXT("ApexNoSplashHold"), TEXT("ApexSettingsDir="),
		TEXT("ApexScreenshotAfter="), TEXT("ApexExecAfter="),
	};
	TArray<FString> Tokens, Switches;
	FCommandLine::Parse(FCommandLine::Get(), Tokens, Switches);
	for (const FString& Switch : Switches)
	{
		if (Switch.StartsWith(TEXT("Apex")) && !Algo::AnyOf(Observing, [&Switch](const TCHAR* Prefix) { return Switch.StartsWith(Prefix); }))
		{
			return false;
		}
	}
	return true;
}

const FApexSeason& UApexMenuFlowSubsystem::GetSeason() const
{
	static const FApexSeason None;
	return Profile ? Profile->Season : None;
}

const TArray<FApexSeasonSummary>& UApexMenuFlowSubsystem::GetPastSeasons() const
{
	static const TArray<FApexSeasonSummary> None;
	return Profile ? Profile->PastSeasons : None;
}

void UApexMenuFlowSubsystem::StartSeason(const FApexSeason& Season)
{
	if (!Profile || Season.Rounds.Num() == 0)
	{
		return;
	}
	EndSeason();
	Profile->Season = Season;
	SaveProfile();
	UE_LOG(LogApexSim, Log, TEXT("Season '%s': %d round(s), %s, car %s, %d AI, %d laps"),
		*Season.Name, Season.Rounds.Num(), *Season.CarClass, *Season.CarId, Season.AiCount, Season.Laps);
}

void UApexMenuFlowSubsystem::EndSeason()
{
	SeasonRoundRunning = INDEX_NONE;
	if (!Profile || Profile->Season.Rounds.Num() == 0)
	{
		return;
	}
	// A season ended before any round was raced leaves nothing to remember.
	if (Profile->Season.RoundsDone() > 0)
	{
		Profile->PastSeasons.Add(ApexSeason::Summarise(Profile->Season));
	}
	Profile->Season = FApexSeason();
	SaveProfile();
}

const TArray<FApexSchoolMedal>& UApexMenuFlowSubsystem::GetSchoolMedals() const
{
	static const TArray<FApexSchoolMedal> None;
	return Profile ? Profile->SchoolMedals : None;
}

bool UApexMenuFlowSubsystem::FileSchoolResult(const FString& Key, const ApexSchool::FTest& Test, const ApexSchool::FResult& Result)
{
	if (!Profile)
	{
		return false;
	}
	const bool bBest = ApexSchool::FileResult(Profile->SchoolMedals, Key, Test, Result);
	SaveProfile();
	return bBest;
}

bool UApexMenuFlowSubsystem::BeginSeasonRound()
{
	const int32 Round = Profile ? Profile->Season.NextRound() : INDEX_NONE;
	if (Round == INDEX_NONE)
	{
		return false;
	}
	SeasonRoundRunning = Round;
	SetPendingCar(Profile->Season.CarId);
	SetPendingTrack(Profile->Season.Rounds[Round].TrackId);
	return true;
}

int32 UApexMenuFlowSubsystem::FileSeasonRace(const FString& TrackId, const TArray<FApexSeasonEntry>& Results)
{
	const int32 Round = SeasonRoundRunning;
	if (!Profile || Round == INDEX_NONE || !Profile->Season.Rounds.IsValidIndex(Round)
		|| !Profile->Season.Rounds[Round].TrackId.Equals(TrackId, ESearchCase::IgnoreCase))
	{
		return INDEX_NONE;
	}
	if (!ApexSeason::FileRound(Profile->Season, Round, Results))
	{
		return INDEX_NONE;
	}
	SeasonRoundRunning = INDEX_NONE;
	SaveProfile();
	UE_LOG(LogApexSim, Log, TEXT("Season '%s': round %d of %d filed"), *Profile->Season.Name, Round + 1, Profile->Season.Rounds.Num());
	return Round;
}

void UApexMenuFlowSubsystem::FileSessionStats(const FApexSessionStats& Session)
{
	if (!Profile)
	{
		return;
	}
	ApexProfiles::FileSession(Profile->Stats, Session);
	SaveProfile();
	UE_LOG(LogApexSim, Log, TEXT("Profile stats: %d lap(s), %.1f km, %d race(s), %d win(s), %d incident(s) in all"),
		Profile->Stats.Laps, Profile->Stats.DistanceM / 1000.0, Profile->Stats.Races, Profile->Stats.Wins, Profile->Stats.Incidents);
}

FString UApexMenuFlowSubsystem::PendingCarClass() const
{
	FApexCarCatalogRow Row;
	return HasPendingCar() && GetCarCatalogRow(PendingCarId, Row) ? Row.CarClass.TrimStartAndEnd() : FString();
}

int32 UApexMenuFlowSubsystem::OtherClassAiCount() const
{
	const FString Own = PendingCarClass();
	int32 Others = 0;
	for (const TPair<FString, int32>& Pair : CreateClassAi)
	{
		if (Pair.Value > 0 && !Pair.Key.Equals(Own, ESearchCase::IgnoreCase))
		{
			Others += Pair.Value;
		}
	}
	return FMath::Min(Others, FMath::Max(0, CreateAiCount));
}

TArray<FApexClassEntry> UApexMenuFlowSubsystem::ClassesToSend() const
{
	TArray<FApexClassEntry> Out;
	if (CreateStartingMode == EApexGameMode::Hotlap || CreateAiCount <= 0)
	{
		return Out;
	}
	const FString Own = PendingCarClass();
	// By name, so the same choices make the same bytes.
	TArray<FString> Keys;
	CreateClassAi.GetKeys(Keys);
	Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
	int32 Others = 0;
	for (const FString& Key : Keys)
	{
		const int32 Count = FMath::Min(CreateClassAi[Key], CreateAiCount - Others);
		if (Count <= 0 || Key.Equals(Own, ESearchCase::IgnoreCase))
		{
			continue;
		}
		FApexClassEntry Entry;
		Entry.Class = Key;
		Entry.AiCount = Count;
		Others += Count;
		Out.Add(Entry);
	}
	if (Out.IsEmpty())
	{
		return Out;
	}
	if (!Own.IsEmpty())
	{
		// The player's own class first, with what is left of the field;
		// the server ranks the list by pace anyway.
		FApexClassEntry Mine;
		Mine.Class = Own;
		Mine.AiCount = CreateAiCount - Others;
		Out.Insert(Mine, 0);
	}
	return Out;
}

TArray<FString> UApexMenuFlowSubsystem::GridOrderToSend(int32 GridCeiling) const
{
	if (CreateStartingMode != EApexGameMode::Race || CreateGridOrder.IsEmpty()
		|| !CreateGridTrackId.Equals(GetPendingTrackId(), ESearchCase::IgnoreCase))
	{
		return TArray<FString>();
	}
	const ApexCreateSession::FGrid Grid = ApexCreateSession::Grid(
		CreateSessionKind != EApexSessionKind::Practice, CreateMaxPlayers, CreateAiCount, GridCeiling);
	if (ApexCreateSession::IsDefaultOrder(CreateGridOrder, Grid.Ai, Grid.Field))
	{
		return TArray<FString>();
	}
	return ApexCreateSession::Normalise(CreateGridOrder, Grid.Ai, Grid.Field);
}

FApexWeekendFormat UApexMenuFlowSubsystem::WeekendToSend() const
{
	FApexWeekendFormat Format;
	if (!IsCreatingWeekend())
	{
		return Format;
	}
	Format.PracticeSeconds = CreateWeekendPracticeMinutes * 60;
	for (int32 Round = 0; Round < FMath::Clamp(CreateWeekendRounds, 1, FApexWeekendFormat::MaxRounds); ++Round)
	{
		Format.QualifyingSeconds.Add(CreateWeekendRoundMinutes * 60);
	}
	// Each day's own sky, when it picks one; practice only when there is any.
	Format.bHasPracticeConditions = CreateWeekendPracticeMinutes > 0
		&& ApexCreateSession::DaySky(CreateConditions, CreateWeekendPracticeWeather, CreateWeekendPracticeHour,
			ApexCreateSession::EWeekendDay::Practice, Format.PracticeConditions);
	Format.bHasQualifyingConditions = ApexCreateSession::DaySky(CreateConditions, CreateWeekendQualifyingWeather,
		CreateWeekendQualifyingHour, ApexCreateSession::EWeekendDay::Qualifying, Format.QualifyingConditions);
	// Each phase's own damage, when it picks one; practice only when there is any.
	Format.bHasPracticeDamage = CreateWeekendPracticeMinutes > 0 && CreateWeekendPracticeDamage >= 0;
	Format.PracticeDamage = static_cast<EApexDamageLevel>(FMath::Max(CreateWeekendPracticeDamage, 0));
	Format.bHasQualifyingDamage = CreateWeekendQualifyingDamage >= 0;
	Format.QualifyingDamage = static_cast<EApexDamageLevel>(FMath::Max(CreateWeekendQualifyingDamage, 0));
	// Each phase's own aids, as a preset; practice's only with practice.
	Format.bHasPracticeAssists = CreateWeekendPracticeMinutes > 0 && CreateWeekendPracticeAssists >= 0;
	Format.PracticeAssists = ApexCreateSession::PhaseAssists(CreateWeekendPracticeAssists);
	Format.bHasQualifyingAssists = CreateWeekendQualifyingAssists >= 0;
	Format.QualifyingAssists = ApexCreateSession::PhaseAssists(CreateWeekendQualifyingAssists);
	return Format.Clamped();
}

FString UApexMenuFlowSubsystem::DescribeWeekend() const
{
	if (!IsCreatingWeekend())
	{
		return FString();
	}
	TArray<FString> Parts;
	if (CreateWeekendPracticeMinutes > 0)
	{
		Parts.Add(FString::Printf(TEXT("Practice %s"), *ApexRaceLength::Describe(CreateWeekendPracticeMinutes * 60)));
	}
	Parts.Add(ApexCreateSession::WeekendRoundsLabel(CreateWeekendRounds, CreateWeekendRoundMinutes,
		CreateQualifyingFormat == EApexQualifyingFormat::Shootout));
	const int32 RaceSeconds = EffectiveRaceSeconds();
	Parts.Add(FString::Printf(TEXT("Race %s"), *(RaceSeconds > 0 ? ApexRaceLength::Describe(RaceSeconds) : FString::Printf(TEXT("%d laps"), CreateLapLimit))));
	return FString::Join(Parts, TEXT(" · "));
}

FString UApexMenuFlowSubsystem::DescribeRaceLength() const
{
	if (CreateStartingMode == EApexGameMode::Qualification)
	{
		const int32 Qualifying = EffectiveQualifyingSeconds();
		return Qualifying > 0 ? ApexRaceLength::Describe(Qualifying) : FString(TEXT("Open"));
	}
	if (CreateStartingMode != EApexGameMode::Race)
	{
		return FString();
	}
	const int32 Seconds = EffectiveRaceSeconds();
	return Seconds > 0 ? ApexRaceLength::Describe(Seconds) : FString::Printf(TEXT("%d LAPS"), CreateLapLimit);
}

void UApexMenuFlowSubsystem::SaveProfile()
{
	if (!Profile)
	{
		return;
	}

	Profile->LastCarId = PendingCarId;
	Profile->LastTrackId = PendingTrackId;
	Profile->DriverName = PlayerName;
	Profile->ServerHost = ServerHost;
	Profile->ServerPort = ServerPort;
	Profile->MaxPlayers = CreateMaxPlayers;
	Profile->AiCount = CreateAiCount;
	Profile->LapLimit = CreateLapLimit;
	Profile->bTimedRace = bCreateTimedRace;
	Profile->RaceMinutes = CreateRaceMinutes;
	Profile->QualifyingMinutes = CreateQualifyingMinutes;
	Profile->bWeekend = bCreateWeekend;
	Profile->WeekendPracticeMinutes = CreateWeekendPracticeMinutes;
	Profile->WeekendRounds = CreateWeekendRounds;
	Profile->WeekendRoundMinutes = CreateWeekendRoundMinutes;
	Profile->WeekendPracticeWeather = CreateWeekendPracticeWeather;
	Profile->WeekendPracticeHour = CreateWeekendPracticeHour;
	Profile->WeekendQualifyingWeather = CreateWeekendQualifyingWeather;
	Profile->WeekendQualifyingHour = CreateWeekendQualifyingHour;
	Profile->StartingMode = CreateStartingMode;
	Profile->SessionKind = CreateSessionKind;
	Profile->AllowedAssists = CreateAllowedAssists;
	Profile->Conditions = CreateConditions;
	Profile->Damage = CreateDamage;
	Profile->bQualifyingCutoff = bCreateQualifyingCutoff;
	Profile->MandatoryStops = CreateMandatoryStops;
	Profile->bCompoundChange = bCreateCompoundChange;
	Profile->RaceRules = CreateRaceRules;
	Profile->AiSkill = CreateAiSkill;
	Profile->ClassAi.Reset();
	for (const TPair<FString, int32>& Pair : CreateClassAi)
	{
		if (Pair.Value > 0)
		{
			Profile->ClassAi.Add(FString::Printf(TEXT("%s=%d"), *Pair.Key, Pair.Value));
		}
	}
	Profile->QualifyingFormat = CreateQualifyingFormat;
	Profile->QualifyingLapAllowance = CreateQualifyingLapAllowance;
	Profile->WeekendPracticeDamage = CreateWeekendPracticeDamage;
	Profile->WeekendQualifyingDamage = CreateWeekendQualifyingDamage;
	Profile->WeekendPracticeAssists = CreateWeekendPracticeAssists;
	Profile->WeekendQualifyingAssists = CreateWeekendQualifyingAssists;

	if (!UGameplayStatics::SaveGameToSlot(Profile, ProfileSlot, 0))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Could not write the profile slot '%s'"), *ProfileSlot);
	}
	// The list shows each profile's name and picture without loading it.
	if (FApexProfileEntry* Entry = CurrentEntry())
	{
		if (Entry->Name != Profile->DriverName || Entry->Portrait != Profile->Portrait)
		{
			Entry->Name = Profile->DriverName;
			Entry->Portrait = Profile->Portrait;
			ApexProfiles::SaveIndex(ProfileIndex);
		}
	}

	// Same moment, both stores: a server picked in the connect dialog has to
	// reach settings.yml, or the next launch would silently go back to the old
	// one. A no-op when the address has not moved, which is most calls.
	if (UApexBootSettingsSubsystem* Boot = GetBoot())
	{
		Boot->SetServer(ServerHost, ServerPort);
	}
}

UApexBootSettingsSubsystem* UApexMenuFlowSubsystem::GetBoot() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UApexBootSettingsSubsystem>() : nullptr;
}

void UApexMenuFlowSubsystem::AdoptBootSettings()
{
	UApexBootSettingsSubsystem* Boot = GetBoot();
	if (!Boot)
	{
		return;
	}

	if (Boot->DidFileExist())
	{
		// The file the player can edit wins: pointing the game at a different
		// server has to be possible without first managing to connect to one.
		ServerHost = Boot->Get().ServerHost;
		ServerPort = Boot->Get().ServerPort;
	}
	else
	{
		// The file was created this run, so seed it from the profile: an
		// existing install keeps the server it was last pointed at.
		Boot->SetServer(ServerHost, ServerPort);
	}
}

bool UApexMenuFlowSubsystem::GetBestLapSeconds(const FString& TrackId, float& OutSeconds) const
{
	if (!Profile || TrackId.IsEmpty())
	{
		return false;
	}

	if (const float* Stored = Profile->BestLapSeconds.Find(TrackId))
	{
		OutSeconds = *Stored;
		return true;
	}
	return false;
}

bool UApexMenuFlowSubsystem::RecordBestLap(const FString& TrackId, float Seconds)
{
	if (!Profile || TrackId.IsEmpty() || Seconds <= 0.0f)
	{
		return false;
	}

	const float* Stored = Profile->BestLapSeconds.Find(TrackId);
	if (Stored && *Stored <= Seconds)
	{
		return false;
	}

	Profile->BestLapSeconds.Add(TrackId, Seconds);
	SaveProfile();
	UE_LOG(LogApexSim, Log, TEXT("New personal best on %s: %s"), *TrackId, *FormatLapTime(Seconds));
	return true;
}

FString UApexMenuFlowSubsystem::FormatLapTime(float Seconds)
{
	if (Seconds <= 0.0f)
	{
		return FString();
	}

	const int32 Minutes = FMath::FloorToInt(Seconds / 60.0f);
	const float Remainder = Seconds - Minutes * 60.0f;
	return FString::Printf(TEXT("%d:%06.3f"), Minutes, Remainder);
}

FString UApexMenuFlowSubsystem::GetGameModeName(EApexGameMode Mode)
{
	switch (Mode)
	{
	case EApexGameMode::Lobby:         return TEXT("Lobby");
	case EApexGameMode::Sandbox:       return TEXT("Sandbox");
	case EApexGameMode::Countdown:     return TEXT("Countdown");
	case EApexGameMode::Grid:          return TEXT("Grid");
	case EApexGameMode::DemoLap:       return TEXT("Demo lap");
	case EApexGameMode::FreePractice:  return TEXT("Free practice");
	case EApexGameMode::Replay:        return TEXT("Replay");
	case EApexGameMode::Qualification: return TEXT("Qualifying");
	case EApexGameMode::Practice:      return TEXT("Practice");
	case EApexGameMode::Race:          return TEXT("Race");
	case EApexGameMode::Hotlap:        return TEXT("Hotlap");
	default:                           return TEXT("Unknown");
	}
}

bool UApexMenuFlowSubsystem::ConsumeAutoConnect()
{
	if (!bAutoConnectOnStartup || bAutoConnectConsumed)
	{
		return false;
	}
	bAutoConnectConsumed = true;
	return true;
}

const FApexCarCatalogRow* UApexMenuFlowSubsystem::FindCarRow(const FString& CarId) const
{
	// The car's own folder describes it (its car.toml is what the car on
	// screen is built from and what the checksum is of); the table is a
	// fallback for a car with no folder on this machine. Both are the car
	// content subsystem's.
	const UApexCarContentSubsystem* Content = UApexCarContentSubsystem::Get();
	return Content ? Content->FindRow(CarId) : nullptr;
}

const FApexTrackCatalogRow* UApexMenuFlowSubsystem::FindTrackRow(const FString& TrackId) const
{
	if (TrackId.IsEmpty())
	{
		return nullptr;
	}
	// The track's own export describes it: it is what the circuit on screen
	// is built from, so its checksum is the one to compare with the server's.
	// The table is a fallback for a track with no export on this machine.
	if (const UApexTrackContentSubsystem* Content = GetTrackContent())
	{
		if (const FApexTrackCatalogRow* Runtime = Content->FindRuntimeRow(TrackId))
		{
			return Runtime;
		}
	}
	if (TrackCatalog)
	{
		for (const TPair<FName, uint8*>& Pair : TrackCatalog->GetRowMap())
		{
			if (Pair.Key.ToString().Equals(TrackId, ESearchCase::IgnoreCase))
			{
				return reinterpret_cast<const FApexTrackCatalogRow*>(Pair.Value);
			}
		}
	}
	return nullptr;
}

UApexTrackContentSubsystem* UApexMenuFlowSubsystem::GetTrackContent() const
{
	return GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexTrackContentSubsystem>() : nullptr;
}

bool UApexMenuFlowSubsystem::GetCarCatalogRow(const FString& CarId, FApexCarCatalogRow& OutRow) const
{
	if (const FApexCarCatalogRow* Row = FindCarRow(CarId))
	{
		OutRow = *Row;
		return true;
	}
	return false;
}

bool UApexMenuFlowSubsystem::GetTrackCatalogRow(const FString& TrackId, FApexTrackCatalogRow& OutRow) const
{
	if (const FApexTrackCatalogRow* Row = FindTrackRow(TrackId))
	{
		OutRow = *Row;
		return true;
	}
	return false;
}

void UApexMenuFlowSubsystem::ReportUnmatchedCatalogIds(const FApexLobbyState& LobbyState)
{
	if (bReportedUnmatchedIds)
	{
		return;
	}
	bReportedUnmatchedIds = true;

	TArray<FString> MissingCars;
	for (const FApexCarConfigSummary& Car : LobbyState.CarConfigs)
	{
		if (!FindCarRow(Car.Id))
		{
			MissingCars.Add(FString::Printf(TEXT("%s (%s)"), *Car.Id, *Car.Name));
		}
	}

	TArray<FString> MissingTracks;
	for (const FApexTrackConfigSummary& Track : LobbyState.TrackConfigs)
	{
		if (!FindTrackRow(Track.Id))
		{
			MissingTracks.Add(FString::Printf(TEXT("%s (%s)"), *Track.Id, *Track.Name));
		}
	}

	if (MissingCars.Num() > 0)
	{
		UE_LOG(LogApexSim, Warning, TEXT("%d car(s) have no folder on this machine and no DT_CarCatalog row, and will show placeholder art: %s"),
			MissingCars.Num(), *FString::Join(MissingCars, TEXT(", ")));
	}
	if (MissingTracks.Num() > 0)
	{
		UE_LOG(LogApexSim, Warning, TEXT("%d track(s) have no export on this machine and no DT_TrackCatalog row, and will show placeholder art: %s"),
			MissingTracks.Num(), *FString::Join(MissingTracks, TEXT(", ")));
	}
	// A row that matched but was baked from another version of the file is
	// worse than a missing one: the art is there and wrong. Say so once here;
	// the race director repeats it, with a toast, for the content actually used.
	int32 Stale = 0;
	for (const FApexCarConfigSummary& Car : LobbyState.CarConfigs)
	{
		if (const FApexCarCatalogRow* Row = FindCarRow(Car.Id))
		{
			if (ApexContent::Compare(Row->SourceCrc, Car.ContentCrc) == EApexContentMatch::Mismatch)
			{
				++Stale;
				UE_LOG(LogApexSim, Warning, TEXT("%s"), *ApexContent::DescribeMismatch(TEXT("Car"), Car.Name, Row->SourceCrc, Car.ContentCrc));
			}
		}
	}
	for (const FApexTrackConfigSummary& Track : LobbyState.TrackConfigs)
	{
		if (const FApexTrackCatalogRow* Row = FindTrackRow(Track.Id))
		{
			if (ApexContent::Compare(Row->SourceCrc, Track.ContentCrc) == EApexContentMatch::Mismatch)
			{
				++Stale;
				UE_LOG(LogApexSim, Warning, TEXT("%s"), *ApexContent::DescribeMismatch(TEXT("Track"), Track.Name, Row->SourceCrc, Track.ContentCrc));
			}
		}
	}
	if (Stale > 0)
	{
		UE_LOG(LogApexSim, Warning, TEXT("%d catalog row(s) were imported from a different version of the file than the server runs"), Stale);
	}

	if (MissingCars.Num() == 0 && MissingTracks.Num() == 0)
	{
		UE_LOG(LogApexSim, Log, TEXT("Catalog join complete: %d car(s) and %d track(s) all matched"),
			LobbyState.CarConfigs.Num(), LobbyState.TrackConfigs.Num());
	}
}

// --- Content checksums ---------------------------------------------------------

const FApexLobbyState* UApexMenuFlowSubsystem::CachedLobbyState() const
{
	const UApexNetSubsystem* Net = GetGameInstance() ? GetGameInstance()->GetSubsystem<UApexNetSubsystem>() : nullptr;
	return Net ? &Net->GetCachedLobbyState() : nullptr;
}

EApexContentMatch UApexMenuFlowSubsystem::VerifyContent(
	const TCHAR* Kind, const FString& Id, const FString& Name, int64 LocalCrc, int64 ServerCrc, bool bNotify)
{
	const EApexContentMatch Verdict = ApexContent::Compare(LocalCrc, ServerCrc);
	switch (Verdict)
	{
	case EApexContentMatch::Match:
		UE_LOG(LogApexSim, Log, TEXT("%s \"%s\" matches the server's file (%08X)"), Kind, *Name, static_cast<uint32>(LocalCrc & 0xFFFFFFFF));
		break;
	case EApexContentMatch::Mismatch:
	{
		const FString Message = ApexContent::DescribeMismatch(Kind, Name, LocalCrc, ServerCrc);
		UE_LOG(LogApexSim, Warning, TEXT("%s"), *Message);
		if (bNotify)
		{
			OnContentMismatch.Broadcast(Message);
		}
		break;
	}
	case EApexContentMatch::Unknown:
		if (!ReportedUnknownContent.Contains(Id))
		{
			ReportedUnknownContent.Add(Id);
			UE_LOG(LogApexSim, Log, TEXT("%s \"%s\" cannot be checked against the server: %s"), Kind, *Name,
				ServerCrc == 0 ? TEXT("the server sent no checksum") : TEXT("the catalog row has no checksum (re-run the import)"));
		}
		break;
	}
	return Verdict;
}

EApexContentMatch UApexMenuFlowSubsystem::VerifyTrackContent(const FString& TrackId, bool bNotify)
{
	const FApexTrackCatalogRow* Row = FindTrackRow(TrackId);
	const FApexLobbyState* Lobby = CachedLobbyState();
	const FApexTrackConfigSummary* Server = Lobby
		? Lobby->TrackConfigs.FindByPredicate([&TrackId](const FApexTrackConfigSummary& T) { return T.Id.Equals(TrackId, ESearchCase::IgnoreCase); })
		: nullptr;
	const FString Name = Server ? Server->Name : (Row ? Row->DisplayName : TrackId);
	return VerifyContent(TEXT("Track"), TrackId, Name, Row ? Row->SourceCrc : 0, Server ? Server->ContentCrc : 0, bNotify);
}

EApexContentMatch UApexMenuFlowSubsystem::VerifyCarContent(const FString& CarId, bool bNotify)
{
	const FApexCarCatalogRow* Row = FindCarRow(CarId);
	const FApexLobbyState* Lobby = CachedLobbyState();
	const FApexCarConfigSummary* Server = Lobby
		? Lobby->CarConfigs.FindByPredicate([&CarId](const FApexCarConfigSummary& C) { return C.Id.Equals(CarId, ESearchCase::IgnoreCase); })
		: nullptr;
	const FString Name = Server ? Server->Name : (Row ? Row->DisplayName : CarId);
	return VerifyContent(TEXT("Car"), CarId, Name, Row ? Row->SourceCrc : 0, Server ? Server->ContentCrc : 0, bNotify);
}

FString UApexMenuFlowSubsystem::FindTrackIdByStem(const FString& Stem) const
{
	if (Stem.IsEmpty())
	{
		return FString();
	}
	if (const UApexTrackContentSubsystem* Content = GetTrackContent())
	{
		const FString Id = Content->FindRuntimeTrackIdByStem(Stem);
		if (!Id.IsEmpty())
		{
			return Id;
		}
	}
	if (TrackCatalog)
	{
		for (const TPair<FName, uint8*>& Pair : TrackCatalog->GetRowMap())
		{
			const FApexTrackCatalogRow* Row = reinterpret_cast<const FApexTrackCatalogRow*>(Pair.Value);
			if (Row && Row->YamlBaseName.Equals(Stem, ESearchCase::IgnoreCase))
			{
				return Pair.Key.ToString();
			}
		}
	}
	return FString();
}
