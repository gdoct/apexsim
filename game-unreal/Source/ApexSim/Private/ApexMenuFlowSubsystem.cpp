#include "ApexMenuFlowSubsystem.h"

#include "ApexBootSettings.h"
#include "ApexNetSubsystem.h"
#include "ApexProfileSave.h"
#include "ApexSim.h"
#include "Engine/DataTable.h"
#include "Kismet/GameplayStatics.h"
#include "Track/ApexTrackContentSubsystem.h"

namespace
{
	const TCHAR* CarCatalogPath = TEXT("/Game/Data/DT_CarCatalog.DT_CarCatalog");
	const TCHAR* TrackCatalogPath = TEXT("/Game/Data/DT_TrackCatalog.DT_TrackCatalog");
}

void UApexMenuFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// settings.yml is the authority on the server address, so it has to be read
	// before LoadProfile() decides which one to connect to.
	Collection.InitializeDependency<UApexBootSettingsSubsystem>();
	// Tracks found on disk at runtime join the catalog through it.
	Collection.InitializeDependency<UApexTrackContentSubsystem>();

	// LoadObject rather than a constructor finder: the tables are authored in
	// the editor after this C++ is first compiled, so they legitimately do not
	// exist on the first run and a missing table must not be fatal.
	CarCatalog = LoadObject<UDataTable>(nullptr, CarCatalogPath);
	TrackCatalog = LoadObject<UDataTable>(nullptr, TrackCatalogPath);

	if (!CarCatalog)
	{
		UE_LOG(LogApexSim, Warning, TEXT("No car catalog at %s — cars will render without local metadata"), CarCatalogPath);
	}
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
	if (UGameplayStatics::DoesSaveGameExist(UApexProfileSave::SlotName, 0))
	{
		Profile = Cast<UApexProfileSave>(UGameplayStatics::LoadGameFromSlot(UApexProfileSave::SlotName, 0));
	}

	if (!Profile)
	{
		// First run, or a slot written by an incompatible build. Either way the
		// defaults above stand and the next SaveProfile writes a fresh one.
		Profile = Cast<UApexProfileSave>(UGameplayStatics::CreateSaveGameObject(UApexProfileSave::StaticClass()));
		UE_LOG(LogApexSim, Log, TEXT("No profile in slot '%s' — starting from defaults"), UApexProfileSave::SlotName);
		AdoptBootSettings();
		return;
	}

	PendingCarId = Profile->LastCarId;
	PendingTrackId = Profile->LastTrackId;
	PlayerName = Profile->DriverName;
	ServerHost = Profile->ServerHost;
	ServerPort = Profile->ServerPort;
	CreateMaxPlayers = Profile->MaxPlayers;
	CreateAiCount = Profile->AiCount;
	CreateLapLimit = Profile->LapLimit;
	CreateStartingMode = Profile->StartingMode;
	CreateSessionKind = Profile->SessionKind;
	CreateAllowedAssists = Profile->AllowedAssists;
	CreateConditions = Profile->Conditions.Clamped();

	// A profile written before demo lap was locked would otherwise start a mode
	// the server turns into a dead end for the player who asked for it.
	if (CreateStartingMode == EApexGameMode::DemoLap)
	{
		CreateStartingMode = EApexGameMode::FreePractice;
	}

	AdoptBootSettings();

	UE_LOG(LogApexSim, Log, TEXT("Profile loaded: driver '%s', last track '%s', last car '%s', %d best lap(s)"),
		*PlayerName, *PendingTrackId, *PendingCarId, Profile->BestLapSeconds.Num());
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
	Profile->StartingMode = CreateStartingMode;
	Profile->SessionKind = CreateSessionKind;
	Profile->AllowedAssists = CreateAllowedAssists;
	Profile->Conditions = CreateConditions;

	if (!UGameplayStatics::SaveGameToSlot(Profile, UApexProfileSave::SlotName, 0))
	{
		UE_LOG(LogApexSim, Warning, TEXT("Could not write the profile slot '%s'"), UApexProfileSave::SlotName);
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
	case EApexGameMode::DemoLap:       return TEXT("Demo lap");
	case EApexGameMode::FreePractice:  return TEXT("Free practice");
	case EApexGameMode::Replay:        return TEXT("Replay");
	case EApexGameMode::Qualification: return TEXT("Qualifying");
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
	if (!CarCatalog || CarId.IsEmpty())
	{
		return nullptr;
	}

	// Row names should already match the server's lowercase-hyphenated UUIDs,
	// but a hand-edited row must not silently break the join.
	for (const TPair<FName, uint8*>& Pair : CarCatalog->GetRowMap())
	{
		if (Pair.Key.ToString().Equals(CarId, ESearchCase::IgnoreCase))
		{
			return reinterpret_cast<const FApexCarCatalogRow*>(Pair.Value);
		}
	}
	return nullptr;
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
		UE_LOG(LogApexSim, Warning, TEXT("%d car(s) have no DT_CarCatalog row and will show placeholder art: %s"),
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
