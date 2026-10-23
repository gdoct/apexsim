#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "GameFramework/SaveGame.h"

#include "ApexProfileSave.generated.h"

/**
 * What a driver has done in this game, summed over every session they drove
 * (filed by UApexSessionRecorder when a session ends; ApexProfiles::FileSession).
 */
USTRUCT()
struct APEXSIM_API FApexDriverStats
{
	GENERATED_BODY()

	/** Whole laps driven, out- and in-laps not counted. */
	UPROPERTY()
	int32 Laps = 0;

	UPROPERTY()
	double DistanceM = 0.0;

	/** Sessions driven in, of every kind. */
	UPROPERTY()
	int32 Sessions = 0;

	/** Races taken to the flag; Wins and Podiums of those. */
	UPROPERTY()
	int32 Races = 0;

	UPROPERTY()
	int32 Wins = 0;

	UPROPERTY()
	int32 Podiums = 0;

	/** Penalties race control gave and laps lost to track limits. */
	UPROPERTY()
	int32 Incidents = 0;

	/** `<track id>|<car id>` -> the best lap in seconds: the PBs per track and car. */
	UPROPERTY()
	TMap<FString, float> CarBests;
};

/**
 * One driver profile: what the client remembers between runs for one person.
 *
 * The server keeps no per-player state beyond lap records — it has no
 * account model, and a `LobbyState` describes only what is available right
 * now — so anything the design calls "yours" (the setup you left off with,
 * your best lap on a track, your stats) has to be stored locally or not exist
 * at all. Several people can share an install: each has a profile of their
 * own, listed in UApexProfileIndexSave (ApexProfiles.h), and its own settings
 * slot beside it.
 *
 * Written through UApexMenuFlowSubsystem; nothing else should touch the slot.
 */
UCLASS()
class APEXSIM_API UApexProfileSave : public USaveGame
{
	GENERATED_BODY()

public:
	/** The first profile's slot, and the only one before profiles existed. */
	static constexpr const TCHAR* SlotName = TEXT("ApexProfile");

	UPROPERTY()
	FString DriverName = TEXT("Player");

	/** Made once when the profile is created; sent with Authenticate so a server knows this driver across sessions. */
	UPROPERTY()
	FGuid DriverId;

	/** The driver picture, 1-based into the portraits (UApexPortraitSubsystem); 0 for none. */
	UPROPERTY()
	int32 Portrait = 0;

	UPROPERTY()
	FApexDriverStats Stats;

	UPROPERTY()
	FString ServerHost = TEXT("127.0.0.1");

	UPROPERTY()
	int32 ServerPort = 9000;

	/** Content UUIDs, matched against the catalog and the server's lobby state. */
	UPROPERTY()
	FString LastCarId;

	UPROPERTY()
	FString LastTrackId;

	UPROPERTY()
	int32 MaxPlayers = 8;

	UPROPERTY()
	int32 AiCount = 0;

	UPROPERTY()
	int32 LapLimit = 5;

	/** A race this player creates runs for RaceMinutes instead of LapLimit laps. */
	UPROPERTY()
	bool bTimedRace = false;

	UPROPERTY()
	int32 RaceMinutes = 60;

	/** Qualifying this player creates runs this long, minutes; 0 is open qualifying. */
	UPROPERTY()
	int32 QualifyingMinutes = 15;

	/** A race this player creates is a race weekend: practice, knockout qualifying, the race. */
	UPROPERTY()
	bool bWeekend = false;

	UPROPERTY()
	int32 WeekendPracticeMinutes = 20;

	UPROPERTY()
	int32 WeekendRounds = 3;

	UPROPERTY()
	int32 WeekendRoundMinutes = 10;

	/** A weekend's practice and qualifying day skies: weather (EApexWeather) and starting hour in minutes; -1 is the race day's. */
	UPROPERTY()
	int32 WeekendPracticeWeather = -1;
	UPROPERTY()
	int32 WeekendPracticeHour = -1;
	UPROPERTY()
	int32 WeekendQualifyingWeather = -1;
	UPROPERTY()
	int32 WeekendQualifyingHour = -1;

	UPROPERTY()
	EApexGameMode StartingMode = EApexGameMode::FreePractice;

	UPROPERTY()
	EApexSessionKind SessionKind = EApexSessionKind::Multiplayer;

	/** Which driving aids a session this player creates allows. */
	UPROPERTY()
	FApexAllowedAssists AllowedAssists;

	/** The weather and clock a session this player creates runs under. */
	UPROPERTY()
	FApexSessionConditions Conditions;

	/** How much damage every car takes in a session this player creates. */
	UPROPERTY()
	EApexDamageLevel Damage = EApexDamageLevel::Full;

	/** The 107% rule on a session this player creates with qualifying. */
	UPROPERTY()
	bool bQualifyingCutoff = false;

	/** The race's pit rule on a session this player creates: mandatory stops (ApexMandatoryStops) and a second compound. */
	UPROPERTY()
	int32 MandatoryStops = 0;

	UPROPERTY()
	bool bCompoundChange = false;

	/** The race-control rules on a session this player creates (start, penalties, safety car, reverse grid). */
	UPROPERTY()
	FApexRaceRules RaceRules;

	/** The AI field's level in a session this player creates (ApexAiSkill; -1 mixed). */
	UPROPERTY()
	int32 AiSkill = -1;

	/** Qualifying's format on a session this player creates (a weekend's final round). */
	UPROPERTY()
	EApexQualifyingFormat QualifyingFormat = EApexQualifyingFormat::Open;

	/** The qualifying lap allowance (ApexQualifyingLapAllowance; 0 none). */
	UPROPERTY()
	int32 QualifyingLapAllowance = 0;

	/** A weekend's practice and qualifying rounds' own damage (EApexDamageLevel; -1 the race's). */
	UPROPERTY()
	int32 WeekendPracticeDamage = -1;

	UPROPERTY()
	int32 WeekendQualifyingDamage = -1;

	/** Track UUID -> best lap in seconds. Populated by the results screen. */
	UPROPERTY()
	TMap<FString, float> BestLapSeconds;
};
