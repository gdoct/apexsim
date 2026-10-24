#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

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

/**
 * Success ballast: the drivers' standings' leaders carry weight into the
 * next round (ApexSeason::BallastTable), handed to the session as
 * CreateSession's ballast by driver.
 */
UENUM()
enum class EApexSuccessBallast : uint8
{
	None  = 0,
	/** 30, 20, 10 kg for the top three. */
	Light = 1,
	/** 75, 57, 48, 39, 30, 21, 15, 9, 3 kg for the top nine, a touring-car series' scale. */
	Heavy = 2,
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

	/** The car's best lap of the race, seconds (the server's, inside track limits); 0 for none or a round filed before it was kept. */
	UPROPERTY()
	float BestLapSeconds = 0.0f;
};

/**
 * A round of a season: its circuit, its own rules and, once raced, the
 * classification. Every rule defaults to the season's (-1 or 0), so a round
 * nobody edited, and one saved before rounds had rules, races the season's
 * format (ApexSeason::RoundSession).
 */
USTRUCT()
struct APEXSIM_API FApexSeasonRound
{
	GENERATED_BODY()

	UPROPERTY()
	FString TrackId;

	/** The round's weather (EApexWeather); -1 for the season's (the profile's conditions). */
	UPROPERTY()
	int32 Weather = -1;

	/** The race's start, hour of the day 0..23; -1 for the season's. */
	UPROPERTY()
	int32 StartHour = -1;

	/** Timed qualifying before the race, minutes (ApexQualifyingLength); 0 for the race alone, the season's format. */
	UPROPERTY()
	int32 QualifyingMinutes = 0;

	/** The race's laps; 0 for the season's. */
	UPROPERTY()
	int32 Laps = 0;

	/** A timed race's length, minutes (ApexRaceLength); 0 for a race over laps. */
	UPROPERTY()
	int32 RaceMinutes = 0;

	/** Any rule of the round's own, not the season's. */
	bool HasOwnRules() const { return Weather >= 0 || StartHour >= 0 || QualifyingMinutes > 0 || Laps > 0 || RaceMinutes > 0; }
	/** The rules back to the season's. */
	void ClearRules() { Weather = -1; StartHour = -1; QualifyingMinutes = 0; Laps = 0; RaceMinutes = 0; }

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

	/** Ballast by championship position into each round (none in round 1). */
	UPROPERTY()
	EApexSuccessBallast Ballast = EApexSuccessBallast::None;

	/** Points for a round's fastest lap (ApexSeason::FastestLapScorer); 0 for none, a season saved before the rule. */
	UPROPERTY()
	int32 FastestLapPoints = 0;

	/** The fastest lap scores only for a driver who finished in the points. */
	UPROPERTY()
	bool bFastestLapNeedsPoints = false;

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

	/** The ballast for the drivers' standings' P1, P2..., kg; empty for none. */
	APEXSIM_API const TArray<int32>& BallastTable(EApexSuccessBallast Preset);
	/** "Light: 30-20-10 kg"; empty for none. */
	APEXSIM_API FString BallastName(EApexSuccessBallast Preset);

	/** One line of a standings table. */
	struct APEXSIM_API FStanding
	{
		/** A driver's name, or a car's id in the cars' table. */
		FString Key;
		bool bYou = false;
		bool bIsAi = false;
		int32 Points = 0;
		int32 Wins = 0;
		/** Rounds whose fastest-lap points this line took (dropped rounds included). */
		int32 FastestLaps = 0;
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

	/** The line of a round's results with its fastest lap (the earliest line on a tie); INDEX_NONE when no lap was timed. */
	APEXSIM_API int32 FastestLapIndex(const FApexSeasonRound& Round);

	/**
	 * The line of a round's results that takes the season's fastest-lap
	 * points: the fastest lap's, when the season scores one and, under
	 * bFastestLapNeedsPoints, that line finished in the points; else
	 * INDEX_NONE (nobody takes it).
	 */
	APEXSIM_API int32 FastestLapScorer(const FApexSeason& Season, const FApexSeasonRound& Round);

	/** "+1 point for the fastest lap, in the points"; empty without the rule. */
	APEXSIM_API FString FastestLapRuleName(const FApexSeason& Season);

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

	/**
	 * The success ballast the season's next round hands its session, as
	 * CreateSession's ballast by driver: each driver with points, by their
	 * place in the drivers' standings, gets that place's weight (the player
	 * as `@host`, an AI by the name it raced under; the AI are named by seat,
	 * so the same field keeps its names). Empty before any round is raced,
	 * and without a preset.
	 */
	APEXSIM_API TArray<FApexDriverBallast> Ballast(const FApexSeason& Season);

	/** What the session made for a round is: the round's own rules over the season's. */
	struct APEXSIM_API FRoundSession
	{
		FString TrackId;
		/** CreateSession's lap_limit: the round's laps, else the season's (sent with a timed race too, which ignores it). */
		int32 Laps = 0;
		/** race_seconds; 0 for a race over laps. */
		int32 RaceSeconds = 0;
		/** qualifying_seconds; 0 for the race alone. */
		int32 QualifyingSeconds = 0;
		FApexSessionConditions Conditions;
		/** What the session counts into on join: Qualification before a race with qualifying, else Race. */
		EApexGameMode StartMode = EApexGameMode::Race;
		TArray<FApexDriverBallast> Ballast;
	};

	/**
	 * Round `Round`'s session: its circuit, length, qualifying and sky (the
	 * round's weather and start hour over `Base`, the profile's conditions;
	 * a round that names its weather drops Base's written forecast, which
	 * was written from another sky), and the ballast by the standings so
	 * far. A round with no rules of its own is the season's race exactly.
	 */
	APEXSIM_API FRoundSession RoundSession(const FApexSeason& Season, int32 Round, const FApexSessionConditions& Base);

	/** "8 laps", "Qualifying 15 min · 45 min · Heavy rain · 18:00": the round's format as the menus say it. */
	APEXSIM_API FString DescribeRound(const FApexSeason& Season, int32 Round);

	/** The season as the profile keeps it once over. */
	APEXSIM_API FApexSeasonSummary Summarise(const FApexSeason& Season);
}
