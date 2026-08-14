#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Catalog/ApexCatalogRows.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexMenuFlowSubsystem.generated.h"

class UApexProfileSave;
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
	/** Appended, not inserted: the switcher is indexed by this enum. */
	SessionResults = 8,
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

	/**
	 * The mode a session is counted into once it starts.
	 *
	 * Not part of CreateSession — the server always creates a session in Lobby —
	 * but the user chooses it at the same time, and StartCountdown needs it.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexGameMode CreateStartingMode = EApexGameMode::FreePractice;

	/**
	 * Set when a session is created from a one-click start, so the shell counts
	 * it in rather than parking the player in the lobby. Consumed on join.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bAutoStartOnJoin = false;

	/**
	 * The mode that start counts into. Usually CreateStartingMode, but a demo
	 * lap is a one-off that must not overwrite the saved session setup.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexGameMode AutoStartMode = EApexGameMode::FreePractice;

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

	// --- Local profile --------------------------------------------------------
	//
	// The server has no account model, so the selections and lap times the design
	// treats as the player's own live in a save slot on this machine.

	/** Writes the current selections and connection defaults to the profile slot. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Profile")
	void SaveProfile();

	UFUNCTION(BlueprintPure, Category = "ApexSim|Profile")
	bool GetBestLapSeconds(const FString& TrackId, float& OutSeconds) const;

	/** Keeps the faster of the stored and given times, and saves if it improved. */
	UFUNCTION(BlueprintCallable, Category = "ApexSim|Profile")
	bool RecordBestLap(const FString& TrackId, float Seconds);

	/** "1:29.884". Returns an empty string for a non-positive time. */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Profile")
	static FString FormatLapTime(float Seconds);

	/** Player-facing name for a game mode: "Free practice", "Demo lap", "Race". */
	UFUNCTION(BlueprintPure, Category = "ApexSim|Menu")
	static FString GetGameModeName(EApexGameMode Mode);

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

	/** Reads the profile slot into the fields above; creates one if absent. */
	void LoadProfile();

	UPROPERTY(Transient)
	TObjectPtr<UApexProfileSave> Profile;

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
