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

	/** One car's qualifying, as the results screen holds it after the flag. */
	struct FSheetEntry
	{
		int32 CarIndex = -1;
		/** Best legal lap, ms; 0 for none. */
		int32 BestMs = 0;
		/** The splits that best lap was made of, ms. */
		TArray<int32> BestLapSplitsMs;
		/** The car's best time in each sector, whatever lap it came from. */
		TArray<int32> BestSectorsMs;
	};

	/** A line of the classification: the board's order, with the sectors laid out. */
	struct FSheetRow
	{
		int32 CarIndex = -1;
		/** 1-based among the cars with a time (the grid slot the race will use); 0 for none. */
		int32 Position = 0;
		int32 BestMs = 0;
		/** Behind pole, ms; 0 for pole and for a car with no time. */
		int32 GapMs = 0;
		/** Behind the car one place up, ms; 0 for pole and for a car with no time. */
		int32 IntervalMs = 0;
		TArray<int32> SplitsMs;
		/** Per split: the quickest that sector was driven on anyone's best lap. */
		TArray<bool> FastestSplit;
		/** The car's best sectors added up; 0 until every sector has a time. */
		int32 IdealMs = 0;
	};

	/** The car's best sectors added up, or 0 while any is missing. */
	inline int32 IdealLapMs(const TArray<int32>& BestSectorsMs)
	{
		int32 Total = 0;
		for (const int32 Sector : BestSectorsMs)
		{
			if (Sector <= 0)
			{
				return 0;
			}
			Total += Sector;
		}
		return Total;
	}

	/**
	 * The qualifying classification: fastest legal lap first, cars with no
	 * time after them in car order, as `Build` orders the garage board. Equal
	 * times keep car order (the server breaks a tie by who set it first,
	 * which the client cannot see).
	 */
	inline TArray<FSheetRow> Sheet(const TArray<FSheetEntry>& Entries)
	{
		TArray<FSheetEntry> Sorted = Entries;
		Sorted.StableSort([](const FSheetEntry& A, const FSheetEntry& B)
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

		// The quickest each sector was driven on a best lap: the purple splits.
		TArray<int32> FastestSplits;
		for (const FSheetEntry& Entry : Sorted)
		{
			if (Entry.BestMs <= 0)
			{
				continue;
			}
			for (int32 Index = 0; Index < Entry.BestLapSplitsMs.Num(); ++Index)
			{
				const int32 Split = Entry.BestLapSplitsMs[Index];
				if (!FastestSplits.IsValidIndex(Index))
				{
					FastestSplits.Add(0);
				}
				if (Split > 0 && (FastestSplits[Index] <= 0 || Split < FastestSplits[Index]))
				{
					FastestSplits[Index] = Split;
				}
			}
		}

		TArray<FSheetRow> Rows;
		Rows.Reserve(Sorted.Num());
		const int32 Pole = Sorted.IsEmpty() ? 0 : Sorted[0].BestMs;
		int32 Position = 0;
		int32 AheadMs = 0;
		for (const FSheetEntry& Entry : Sorted)
		{
			FSheetRow Row;
			Row.CarIndex = Entry.CarIndex;
			Row.BestMs = Entry.BestMs;
			Row.IdealMs = IdealLapMs(Entry.BestSectorsMs);
			if (Entry.BestMs > 0)
			{
				Row.Position = ++Position;
				Row.GapMs = Entry.BestMs - Pole;
				Row.IntervalMs = Position == 1 ? 0 : Entry.BestMs - AheadMs;
				AheadMs = Entry.BestMs;
				Row.SplitsMs = Entry.BestLapSplitsMs;
				for (int32 Index = 0; Index < Row.SplitsMs.Num(); ++Index)
				{
					Row.FastestSplit.Add(Row.SplitsMs[Index] > 0 && FastestSplits.IsValidIndex(Index)
						&& Row.SplitsMs[Index] == FastestSplits[Index]);
				}
			}
			Rows.Add(MoveTemp(Row));
		}
		return Rows;
	}
}
