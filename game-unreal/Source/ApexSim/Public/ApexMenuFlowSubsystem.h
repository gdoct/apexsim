#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Catalog/ApexCatalogRows.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexMenuFlowSubsystem.generated.h"

class UDataTable;

/** The screens the shell can show. Order matches the WidgetSwitcher in WBP_Root. */
UENUM(BlueprintType)
enum class EApexScreen : uint8
{
	MainMenu       = 0,
	ConnectDialog  = 1,
	SessionBrowser = 2,
	SessionCreate  = 3,
	CarSelect      = 4,
	TrackSelect    = 5,
	SessionLobby   = 6,
	Loading        = 7,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPendingCarChanged, const FString&, CarId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPendingTrackChanged, const FString&, TrackId);

/**
 * Client-side menu state that the protocol has no message for, plus the
 * UUID -> local asset joins.
 *
 * Kept out of ApexSimNet so that module stays a pure protocol implementation
 * and survives into the racing client unchanged.
 *
 * Note there is no SelectTrack message in the protocol: a track choice only
 * exists as an argument to CreateSession, so PendingTrackId lives here rather
 * than being pushed to the server when the user picks it.
 */
UCLASS()
class APEXSIM_API UApexMenuFlowSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// --- Pending selections ---------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Menu")
	FApexOnPendingCarChanged OnPendingCarChanged;

	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Menu")
	FApexOnPendingTrackChanged OnPendingTrackChanged;

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Menu")
	void SetPendingCar(const FString& CarId);

	UFUNCTION(BlueprintCallable, Category = "ApexSim|Menu")
	void SetPendingTrack(const FString& TrackId);

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	const FString& GetPendingCarId() const { return PendingCarId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	const FString& GetPendingTrackId() const { return PendingTrackId; }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	bool HasPendingCar() const { return !PendingCarId.IsEmpty(); }

	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	bool HasPendingTrack() const { return !PendingTrackId.IsEmpty(); }

	// --- Session creation parameters ------------------------------------------

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateMaxPlayers = 8;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateAiCount = 0;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateLapLimit = 5;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexSessionKind CreateSessionKind = EApexSessionKind::Multiplayer;

	// --- Connection defaults --------------------------------------------------

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	FString ServerHost = TEXT("127.0.0.1");

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 ServerPort = 9000;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	FString PlayerName = TEXT("Player");

	/** With `[auth] mode = "dev"` the server accepts any token (transport.rs:247). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	FString AuthToken = TEXT("dev-token");

	/** Whether the main menu should connect on its own the first time it appears. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bAutoConnectOnStartup = true;

	/** Cleared once the auto-connect has been attempted, so returning to the menu doesn't retry. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Menu")
	bool ConsumeAutoConnect();

	// --- Catalog lookups ------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "ApexSim|Catalog")
	bool GetCarCatalogRow(const FString& CarId, FApexCarCatalogRow& OutRow) const;

	UFUNCTION(BlueprintPure, Category = "ApexSim|Catalog")
	bool GetTrackCatalogRow(const FString& TrackId, FApexTrackCatalogRow& OutRow) const;

	/**
	 * Logs, once, every ID present in the given lists but missing from the
	 * catalogs. Called after the first LobbyState so a stale catalog announces
	 * itself instead of silently showing placeholder art everywhere.
	 */
	void ReportUnmatchedCatalogIds(const FApexLobbyState& LobbyState);

private:
	const FApexCarCatalogRow* FindCarRow(const FString& CarId) const;
	const FApexTrackCatalogRow* FindTrackRow(const FString& TrackId) const;

	/** Assigned from the paths below in Initialize; null if the tables are missing. */
	UPROPERTY(Transient)
	TObjectPtr<UDataTable> CarCatalog;

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> TrackCatalog;

	FString PendingCarId;
	FString PendingTrackId;

	bool bAutoConnectConsumed = false;
	bool bReportedUnmatchedIds = false;
};
