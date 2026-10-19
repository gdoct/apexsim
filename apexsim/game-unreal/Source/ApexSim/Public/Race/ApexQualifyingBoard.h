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

	// --- The one-lap shoot-out ----------------------------------------------------

	/**
	 * Whether the qualifying running now is a shoot-out (`is_shootout_round`):
	 * the session's format is one, and it is a plain session's qualifying or
	 * a weekend's final round.
	 */
	inline bool IsShootoutRound(EApexQualifyingFormat Format, bool bWeekend, int32 Round, int32 Rounds)
	{
		return Format == EApexQualifyingFormat::Shootout && (!bWeekend || Round >= Rounds);
	}

	/** "1st", "2nd", "3rd", "4th", "11th", "22nd". */
	inline FString Ordinal(int32 N)
	{
		const int32 Tens = N % 100;
		const TCHAR* Suffix = (Tens >= 11 && Tens <= 13) ? TEXT("th")
			: N % 10 == 1 ? TEXT("st") : N % 10 == 2 ? TEXT("nd") : N % 10 == 3 ? TEXT("rd") : TEXT("th");
		return FString::Printf(TEXT("%d%s"), N, Suffix);
	}

	/** Where one car of a shoot-out's run order stands. */
	enum class EShootoutRun : uint8 { Waiting, Running, Done };

	struct FShootoutLine
	{
		/** 1-based place in the run order. */
		int32 Place = 0;
		int32 CarIndex = -1;
		FString Name;
		bool bYou = false;
		EShootoutRun Run = EShootoutRun::Waiting;
	};

	/** The waiting display: the run order with names, who is out and where the local car stands. */
	struct FShootoutView
	{
		TArray<FShootoutLine> Lines;
		/** The car on its run, -1 for none; whether it has started its flying lap. */
		int32 RunningCar = -1;
		bool bFlying = false;
		/** The local car's place in the order, 0 when it is not in it. */
		int32 YourPlace = 0;
		/** Runs still to come before the local car's, the one under way included. */
		int32 ToGo = 0;
		bool bYourTurn = false;
		bool bYouDone = false;
	};

	inline FShootoutView Shootout(const FApexShootoutState& State, const TArray<FApexRosterEntry>& Roster, int32 LocalCarIndex)
	{
		FShootoutView Out;
		for (int32 Index = 0; Index < State.Order.Num(); ++Index)
		{
			FShootoutLine Line;
			Line.Place = Index + 1;
			Line.CarIndex = State.Order[Index];
			const FApexRosterEntry* Entry = Roster.FindByPredicate(
				[&Line](const FApexRosterEntry& Candidate) { return Candidate.CarIndex == Line.CarIndex; });
			Line.Name = Entry ? Entry->PlayerName : FString::Printf(TEXT("Car %d"), Line.CarIndex + 1);
			Line.bYou = Line.CarIndex == LocalCarIndex;
			Line.Run = Line.Place == State.Running ? EShootoutRun::Running
				: Line.Place <= State.Done ? EShootoutRun::Done
				: EShootoutRun::Waiting;
			if (Line.Run == EShootoutRun::Running)
			{
				Out.RunningCar = Line.CarIndex;
				Out.bFlying = State.bFlying;
			}
			if (Line.bYou)
			{
				Out.YourPlace = Line.Place;
				Out.bYourTurn = Line.Run == EShootoutRun::Running;
				Out.bYouDone = Line.Run == EShootoutRun::Done;
			}
			Out.Lines.Add(MoveTemp(Line));
		}
		if (Out.YourPlace > 0 && !Out.bYourTurn && !Out.bYouDone)
		{
			for (const FShootoutLine& Line : Out.Lines)
			{
				Out.ToGo += Line.Place < Out.YourPlace && Line.Run != EShootoutRun::Done ? 1 : 0;
			}
		}
		return Out;
	}

	/** "YOU RUN 4TH · 2 TO GO", "YOU ARE NEXT", "YOUR TURN", "YOUR RUN IS DONE"; empty when not in the order. */
	inline FString ShootoutCall(const FShootoutView& View)
	{
		if (View.YourPlace <= 0)
		{
			return FString();
		}
		if (View.bYourTurn)
		{
			return TEXT("YOUR TURN");
		}
		if (View.bYouDone)
		{
			return TEXT("YOUR RUN IS DONE");
		}
		if (View.ToGo == 0)
		{
			return FString::Printf(TEXT("YOU RUN %s · YOU ARE NEXT"), *Ordinal(View.YourPlace).ToUpper());
		}
		return FString::Printf(TEXT("YOU RUN %s · %d TO GO"), *Ordinal(View.YourPlace).ToUpper(), View.ToGo);
	}
}
