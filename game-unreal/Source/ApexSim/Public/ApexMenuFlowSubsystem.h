#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Catalog/ApexCatalogRows.h"
#include "Catalog/ApexContentCrc.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ApexMenuFlowSubsystem.generated.h"

class UApexBootSettingsSubsystem;
class UApexProfileSave;
class UDataTable;

/** The screens the shell can show, in the order of UApexRootWidget's switcher (append only). */
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
	/** Saved and recent replays (UApexReplaysWidget). */
	Replays        = 9,
	/** A circuit's fastest laps on the server (UApexLeaderboardWidget). */
	Leaderboard    = 10,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPendingCarChanged, const FString&, CarId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnPendingTrackChanged, const FString&, TrackId);
/** A track or car about to be used does not match the server's file; Message is player-facing. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FApexOnContentMismatch, const FString&, Message);

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

	/** Raised by VerifyTrackContent / VerifyCarContent with bNotify; the root widget shows it as a toast. */
	UPROPERTY(BlueprintAssignable, Category = "ApexSim|Catalog")
	FApexOnContentMismatch OnContentMismatch;

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

	/**
	 * A race runs for a time rather than a number of laps: CreateRaceMinutes
	 * from the green light, then the leader's lap is the last. Both lengths
	 * are kept, so switching back finds the other where it was left.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bCreateTimedRace = false;

	/** A timed race's length, minutes (ApexRaceLength::LadderMinutes). */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateRaceMinutes = 60;

	/**
	 * Qualifying's length, minutes (ApexQualifyingLength::ChoiceMinutes); 0
	 * is open qualifying, which runs until the host starts the race. A timed
	 * one ends with the chequered flag and its results screen.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateQualifyingMinutes = 15;

	/**
	 * The race is a race weekend: practice (CreateWeekendPracticeMinutes, 0
	 * for none), CreateWeekendRounds knockout qualifying rounds of
	 * CreateWeekendRoundMinutes each, then the race over the race's own
	 * length. CreateStartingMode stays Race; the host moves it on.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bCreateWeekend = false;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendPracticeMinutes = 20;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendRounds = 3;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendRoundMinutes = 10;

	/**
	 * Practice's and the qualifying day's sky in a weekend: the weather
	 * (EApexWeather) and the hour the day starts at (minutes); -1 keeps the
	 * race day's. A day with either set is sent as its own conditions
	 * (ApexCreateSession::DaySky).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendPracticeWeather = -1;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendPracticeHour = -1;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendQualifyingWeather = -1;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendQualifyingHour = -1;

	bool IsCreatingWeekend() const { return bCreateWeekend && CreateStartingMode == EApexGameMode::Race; }

	/**
	 * How qualifying runs: Open, or a one-lap shoot-out (one car at a time;
	 * in a weekend, the final round). Only a session that qualifies sends it
	 * (QualifyingFormatToSend).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexQualifyingFormat CreateQualifyingFormat = EApexQualifyingFormat::Open;

	/** The most timed laps a car may end in qualifying (each round of a weekend); 0 for no limit. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateQualifyingLapAllowance = 0;

	/** A weekend's practice and qualifying rounds' own damage (EApexDamageLevel); -1 runs them under the race's. */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendPracticeDamage = -1;
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateWeekendQualifyingDamage = -1;

	/** Whether the session being made qualifies: a qualifying session or a weekend. */
	bool CreatesQualifying() const { return CreateStartingMode == EApexGameMode::Qualification || IsCreatingWeekend(); }

	/** What CreateSession carries for the format: Open for a session that does not qualify. */
	EApexQualifyingFormat QualifyingFormatToSend() const
	{
		return CreatesQualifying() ? CreateQualifyingFormat : EApexQualifyingFormat::Open;
	}
	/** The allowance CreateSession carries: none for a session that does not qualify, or for a plain shoot-out's one lap. */
	int32 QualifyingLapAllowanceToSend() const
	{
		const bool bPlainShootout = !IsCreatingWeekend() && CreateQualifyingFormat == EApexQualifyingFormat::Shootout;
		return CreatesQualifying() && !bPlainShootout ? ApexQualifyingLapAllowance::Clamp(CreateQualifyingLapAllowance) : 0;
	}

	/**
	 * The 107% rule: a car slower than 107% of pole in qualifying (Q1 of a
	 * weekend), or with no time, does not start the race. Only a session
	 * that qualifies sends it (QualifyingCutoffToSend).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bCreateQualifyingCutoff = false;

	/** What CreateSession carries: 107 for a qualifying session or a weekend with the rule on, else 0. */
	int32 QualifyingCutoffToSend() const
	{
		return bCreateQualifyingCutoff && (CreateStartingMode == EApexGameMode::Qualification || IsCreatingWeekend())
			? ApexQualifyingCutoff::Rule107 : ApexQualifyingCutoff::None;
	}
	/**
	 * The race's pit rule (ApexMandatoryStops): the stops every car owes and
	 * whether it must run a second dry compound. A session with a race in it
	 * (a race, a weekend, or qualifying before one) sends it (PitRuleApplies).
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateMandatoryStops = 0;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	bool bCreateCompoundChange = false;

	bool PitRuleApplies() const
	{
		return CreateStartingMode == EApexGameMode::Race || CreateStartingMode == EApexGameMode::Qualification;
	}
	/** What CreateSession carries for the rule: nothing in a session without a race. */
	int32 MandatoryStopsToSend() const { return PitRuleApplies() ? ApexMandatoryStops::Clamp(CreateMandatoryStops) : 0; }
	bool CompoundChangeToSend() const { return PitRuleApplies() && bCreateCompoundChange; }

	/** The weekend CreateSession carries: no rounds unless a weekend is being made. */
	FApexWeekendFormat WeekendToSend() const;
	/** "Practice 20 min · Q1-Q3 10 min each · Race 12 laps"; empty for no weekend. */
	FString DescribeWeekend() const;

	/**
	 * What a created session actually gets. A hotlap has no field and no
	 * distance: whatever the sliders hold, it is made with no AI and no lap
	 * limit, and the sliders keep their values for the next race. Only a
	 * race is timed; a timed race has no lap limit.
	 */
	int32 EffectiveAiCount() const { return CreateStartingMode == EApexGameMode::Hotlap ? 0 : CreateAiCount; }
	int32 EffectiveLapLimit() const
	{
		return CreateStartingMode == EApexGameMode::Hotlap || EffectiveRaceSeconds() > 0 ? 0 : CreateLapLimit;
	}
	int32 EffectiveRaceSeconds() const
	{
		return CreateStartingMode == EApexGameMode::Race && bCreateTimedRace
			? ApexRaceLength::Clamp(CreateRaceMinutes * 60) : 0;
	}
	/** Qualifying's length as the server gets it: 0 for open qualifying, and for any other mode. */
	int32 EffectiveQualifyingSeconds() const
	{
		return CreateStartingMode == EApexGameMode::Qualification
			? ApexQualifyingLength::Clamp(CreateQualifyingMinutes * 60) : 0;
	}
	/** "5 LAPS", "2 h", "15 min", "Open": the race's (or qualifying's) length the way the menus say it; empty for other modes. */
	FString DescribeRaceLength() const;

	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexSessionKind CreateSessionKind = EApexSessionKind::Multiplayer;

	/**
	 * Which driving aids the session allows its drivers. The server forces a
	 * disallowed aid off for everyone in the session, whatever their settings.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	FApexAllowedAssists CreateAllowedAssists;

	/**
	 * The weather and time of day the session runs under. The server bakes
	 * the weather's grip into the session's track and echoes both to whoever
	 * joins; the race director lights the circuit from them.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	FApexSessionConditions CreateConditions;

	/**
	 * How much damage every car takes in the session, AI included: a rule of
	 * the session the server applies to all of them, not a driver's choice.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	EApexDamageLevel CreateDamage = EApexDamageLevel::Full;

	/**
	 * The AI field's level (ApexAiSkill): the skill the server spreads the
	 * field round, or -1 for its mixed field of every level, novice to ace.
	 * A rule of the session, like the damage.
	 */
	UPROPERTY(BlueprintReadWrite, Category = "ApexSim|Menu")
	int32 CreateAiSkill = -1;

	/**
	 * The race's start order as the create screen edits it: driver references
	 * (`ApexCreateSession::HostRef`, `AiRef`, or a name) first to last, empty
	 * for the order cars are seated in. Not kept on the profile: it belongs to
	 * one track (CreateGridTrackId), and the AI and the cars it names change.
	 */
	TArray<FString> CreateGridOrder;

	/** The stored qualifying result CreateGridOrder was loaded from; empty for a hand-made order. */
	FString CreateGridResultId;

	/** The track CreateGridOrder was made for. */
	FString CreateGridTrackId;

	/**
	 * The start order a session created now sends: empty unless it is a race
	 * with an order made for the pending track that is not just the seating
	 * order, and otherwise the order made complete for the field.
	 * `GridCeiling` is the most cars a grid holds.
	 */
	TArray<FString> GridOrderToSend(int32 GridCeiling) const;

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

	/**
	 * Compares the catalog row's SourceCrc with the ContentCrc the server sent
	 * for the same id in its last LobbyState (ApexContentCrc.h). A mismatch is
	 * logged as a warning, and with bNotify also raised on OnContentMismatch so
	 * the player sees it; Unknown (no row, no server checksum) is logged once.
	 * Called by the race director as the level streams and the car spawns.
	 */
	EApexContentMatch VerifyTrackContent(const FString& TrackId, bool bNotify);
	EApexContentMatch VerifyCarContent(const FString& CarId, bool bNotify);

	/** Row id of the track whose YAML base name is Stem ("Monza"), or empty. The demo session names its track that way. */
	FString FindTrackIdByStem(const FString& Stem) const;

private:
	EApexContentMatch VerifyContent(const TCHAR* Kind, const FString& Id, const FString& Name, int64 LocalCrc, int64 ServerCrc, bool bNotify);
	const FApexLobbyState* CachedLobbyState() const;

	const FApexCarCatalogRow* FindCarRow(const FString& CarId) const;
	/** The track export's row (UApexTrackContentSubsystem), else the table's. */
	const FApexTrackCatalogRow* FindTrackRow(const FString& TrackId) const;
	class UApexTrackContentSubsystem* GetTrackContent() const;

	/** Reads the profile slot into the fields above; creates one if absent. */
	void LoadProfile();

	UApexBootSettingsSubsystem* GetBoot() const;

	/**
	 * Reconciles the server address with settings.yml: an existing file wins
	 * over the profile, a file created this run is seeded from it.
	 */
	void AdoptBootSettings();

	UPROPERTY(Transient)
	TObjectPtr<UApexProfileSave> Profile;

	/**
	 * The track table, a fallback for a track with no export here; null when
	 * missing. The cars' rows, table included, come from
	 * `UApexCarContentSubsystem`.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UDataTable> TrackCatalog;

	FString PendingCarId;
	FString PendingTrackId;

	bool bAutoConnectConsumed = false;
	bool bReportedUnmatchedIds = false;
	/** Ids whose Unknown verdict has been logged, so a missing checksum is said once. */
	TSet<FString> ReportedUnknownContent;
};
