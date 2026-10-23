#pragma once

#include "CoreMinimal.h"

#include "ApexSeason.generated.h"

/** How a season scores a race (ApexSeason::PointsTable). */
UENUM()
enum class EApexPointsPreset : uint8
{
	/** 25, 18, 15, 12, 10, 8, 6, 4, 2, 1 */
	Modern  = 0,
	/** 10, 6, 4, 3, 2, 1 */
	Classic = 1,
	/** 50 down by fives to 25, then by ones: every finisher of a long field scores. */
	Deep    = 2,
};

/** One car's place in a round of a season. */
USTRUCT()
struct APEXSIM_API FApexSeasonEntry
{
	GENERATED_BODY()

	/** The driver's name as the race had it; the player's own line is bYou whatever their name. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	bool bYou = false;

	UPROPERTY()
	bool bIsAi = false;

	UPROPERTY()
	FString CarId;

	/** Classified position; 0 for a car that did not finish (no points). */
	UPROPERTY()
	int32 Position = 0;
};

/** A round of a season: its circuit and, once raced, the classification. */
USTRUCT()
struct APEXSIM_API FApexSeasonRound
{
	GENERATED_BODY()

	UPROPERTY()
	FString TrackId;

	UPROPERTY()
	bool bDone = false;

	UPROPERTY()
	TArray<FApexSeasonEntry> Results;
};

/**
 * An offline season against the AI, saved in the driver's profile: a
 * calendar of races in one car, a points preset, dropped rounds. The AI
 * field is the same size every round, so its drivers (named by seat on the
 * server) carry their points from race to race.
 */
USTRUCT()
struct APEXSIM_API FApexSeason
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	/** The class (catalog CarClass) and the player's car in it, fixed for the season. */
	UPROPERTY()
	FString CarClass;

	UPROPERTY()
	FString CarId;

	UPROPERTY()
	TArray<FApexSeasonRound> Rounds;

	UPROPERTY()
	EApexPointsPreset Points = EApexPointsPreset::Modern;

	/** Each driver's worst this many rounds do not count. */
	UPROPERTY()
	int32 DropRounds = 0;

	UPROPERTY()
	int32 AiCount = 11;

	UPROPERTY()
	int32 Laps = 8;

	/** The AI field's level (ApexAiSkill); -1 the mixed field. */
	UPROPERTY()
	int32 AiSkill = -1;

	bool IsActive() const { return Rounds.Num() > 0 && NextRound() != INDEX_NONE; }
	bool IsFinished() const { return Rounds.Num() > 0 && NextRound() == INDEX_NONE; }
	/** The first round not yet raced; INDEX_NONE once all are. */
	int32 NextRound() const { return Rounds.IndexOfByPredicate([](const FApexSeasonRound& R) { return !R.bDone; }); }
	int32 RoundsDone() const { return Rounds.FilterByPredicate([](const FApexSeasonRound& R) { return R.bDone; }).Num(); }
};

/** A season once over, as the profile remembers it. */
USTRUCT()
struct APEXSIM_API FApexSeasonSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString CarClass;

	UPROPERTY()
	FString Champion;

	/** The player's place in the drivers' standings; 0 when they scored nothing. */
	UPROPERTY()
	int32 YourPosition = 0;

	UPROPERTY()
	int32 YourPoints = 0;

	UPROPERTY()
	int32 Rounds = 0;
};

namespace ApexSeason
{
	/** Points for P1, P2... under a preset; past the end, none. */
	APEXSIM_API const TArray<int32>& PointsTable(EApexPointsPreset Preset);
	APEXSIM_API int32 PointsFor(EApexPointsPreset Preset, int32 Position);
	APEXSIM_API FString PresetName(EApexPointsPreset Preset);

	/** One line of a standings table. */
	struct APEXSIM_API FStanding
	{
		/** A driver's name, or a car's id in the cars' table. */
		FString Key;
		bool bYou = false;
		bool bIsAi = false;
		int32 Points = 0;
		int32 Wins = 0;
		/** Each round's points; -1 for a round not raced or not entered. */
		TArray<int32> RoundPoints;
		/** Which rounds' points the drop rule left out. */
		TArray<bool> Dropped;
		/** Finishes by place (index 0 = wins), for the tie-break. */
		TArray<int32> Places;
	};

	/**
	 * The drivers' standings, leader first: points after drops, then wins,
	 * then seconds, thirds and so on, then name. The player is one driver
	 * whatever name they raced under.
	 */
	APEXSIM_API TArray<FStanding> DriverStandings(const FApexSeason& Season);

	/** The cars' standings: every result of each car summed, no drops. */
	APEXSIM_API TArray<FStanding> CarStandings(const FApexSeason& Season);

	/**
	 * File a race as round `Round`: its classification, best first (each
	 * line's Position, 0 for not classified). False when the round is not
	 * the next one or there is nothing to file.
	 */
	APEXSIM_API bool FileRound(FApexSeason& Season, int32 Round, const TArray<FApexSeasonEntry>& Results);

	/**
	 * `Count` circuits of `TrackIds` (sorted by the caller), spread evenly
	 * over the list so a short season is not just its first names.
	 */
	APEXSIM_API TArray<FString> SpreadCalendar(const TArray<FString>& TrackIds, int32 Count);

	/** The season as the profile keeps it once over. */
	APEXSIM_API FApexSeasonSummary Summarise(const FApexSeason& Season);
}
