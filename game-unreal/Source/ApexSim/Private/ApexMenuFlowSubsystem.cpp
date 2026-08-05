#include "ApexMenuFlowSubsystem.h"

#include "ApexSim.h"
#include "Engine/DataTable.h"

namespace
{
	const TCHAR* CarCatalogPath = TEXT("/Game/Data/DT_CarCatalog.DT_CarCatalog");
	const TCHAR* TrackCatalogPath = TEXT("/Game/Data/DT_TrackCatalog.DT_TrackCatalog");
}

void UApexMenuFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

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
}

void UApexMenuFlowSubsystem::SetPendingCar(const FString& CarId)
{
	if (PendingCarId.Equals(CarId, ESearchCase::IgnoreCase))
	{
		return;
	}
	PendingCarId = CarId;
	OnPendingCarChanged.Broadcast(PendingCarId);
}

void UApexMenuFlowSubsystem::SetPendingTrack(const FString& TrackId)
{
	if (PendingTrackId.Equals(TrackId, ESearchCase::IgnoreCase))
	{
		return;
	}
	PendingTrackId = TrackId;
	OnPendingTrackChanged.Broadcast(PendingTrackId);
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
	if (!TrackCatalog || TrackId.IsEmpty())
	{
		return nullptr;
	}
	for (const TPair<FName, uint8*>& Pair : TrackCatalog->GetRowMap())
	{
		if (Pair.Key.ToString().Equals(TrackId, ESearchCase::IgnoreCase))
		{
			return reinterpret_cast<const FApexTrackCatalogRow*>(Pair.Value);
		}
	}
	return nullptr;
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
		UE_LOG(LogApexSim, Warning, TEXT("%d track(s) have no DT_TrackCatalog row and will show placeholder art: %s"),
			MissingTracks.Num(), *FString::Join(MissingTracks, TEXT(", ")));
	}
	if (MissingCars.Num() == 0 && MissingTracks.Num() == 0)
	{
		UE_LOG(LogApexSim, Log, TEXT("Catalog join complete: %d car(s) and %d track(s) all matched"),
			LobbyState.CarConfigs.Num(), LobbyState.TrackConfigs.Num());
	}
}
