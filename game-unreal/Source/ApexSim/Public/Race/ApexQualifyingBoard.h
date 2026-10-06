#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The qualifying scoreboard the garage shows: every car of the session,
 * fastest legal lap first, from what the client already holds - the roster
 * (who drives which car index) and the newest telemetry frame (each car's
 * best and last lap, and whether it is in its garage). Pure, so a test can
 * feed it a synthetic frame; the widget only draws the rows.
 *
 * A car with no legal lap yet is listed after the ones with a time, in car
 * order, with no gap. Equal times keep car order, which is the order the
 * server hands out indices in (seating order), so the list does not shuffle
 * between refreshes.
 */
namespace ApexBoard
{
	struct FRow
	{
		int32 CarIndex = -1;
		FString Name;
		bool bAi = false;
		bool bYou = false;
		/** 1-based among the cars with a time; 0 for a car with none. */
		int32 Position = 0;
		/** Best legal lap, ms; 0 for none. */
		int32 BestMs = 0;
		/** Behind the fastest, ms; 0 for the fastest and for a car with no time. */
		int32 GapMs = 0;
		/** The last lap completed, ms (struck or not: telemetry does not say); 0 before one. */
		int32 LastMs = 0;
		bool bInGarage = false;
	};

	inline TArray<FRow> Build(const TArray<FApexRosterEntry>& Roster, const TArray<FApexCarTelemetry>& Cars, int32 LocalCarIndex)
	{
		TArray<FRow> Rows;
		Rows.Reserve(Roster.Num());
		for (const FApexRosterEntry& Entry : Roster)
		{
			FRow Row;
			Row.CarIndex = Entry.CarIndex;
			Row.Name = Entry.PlayerName;
			Row.bAi = Entry.bIsAi;
			Row.bYou = Entry.CarIndex == LocalCarIndex;
			if (const FApexCarTelemetry* Car = Cars.FindByPredicate(
					[&Entry](const FApexCarTelemetry& Candidate) { return Candidate.CarIndex == Entry.CarIndex; }))
			{
				Row.BestMs = FMath::Max(0, Car->BestLapTimeMs);
				Row.LastMs = FMath::Max(0, Car->LastLapTimeMs);
				Row.bInGarage = Car->bInGarage;
			}
			Rows.Add(Row);
		}

		// Stable: equal times (and cars with none) stay in car order.
		Rows.StableSort([](const FRow& A, const FRow& B)
		{
			if ((A.BestMs > 0) != (B.BestMs > 0))
			{
				return A.BestMs > 0;
			}
			if (A.BestMs > 0 && A.BestMs != B.BestMs)
			{
				return A.BestMs < B.BestMs;
			}
			return A.CarIndex < B.CarIndex;
		});

		const int32 Fastest = Rows.IsEmpty() ? 0 : Rows[0].BestMs;
		int32 Position = 0;
		for (FRow& Row : Rows)
		{
			if (Row.BestMs > 0)
			{
				Row.Position = ++Position;
				Row.GapMs = Row.BestMs - Fastest;
			}
		}
		return Rows;
	}
}
