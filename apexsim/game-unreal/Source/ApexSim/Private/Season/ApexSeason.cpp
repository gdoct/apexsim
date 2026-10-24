#include "Season/ApexSeason.h"

namespace ApexSeason
{
	const TArray<int32>& PointsTable(EApexPointsPreset Preset)
	{
		static const TArray<int32> Modern = {25, 18, 15, 12, 10, 8, 6, 4, 2, 1};
		static const TArray<int32> Classic = {10, 6, 4, 3, 2, 1};
		static const TArray<int32> Deep = []
		{
			TArray<int32> Table;
			for (int32 Points = 50; Points > 25; Points -= 5)
			{
				Table.Add(Points);
			}
			for (int32 Points = 25; Points >= 1; --Points)
			{
				Table.Add(Points);
			}
			return Table;
		}();
		switch (Preset)
		{
		case EApexPointsPreset::Classic: return Classic;
		case EApexPointsPreset::Deep:    return Deep;
		default:                         return Modern;
		}
	}

	int32 PointsFor(EApexPointsPreset Preset, int32 Position)
	{
		const TArray<int32>& Table = PointsTable(Preset);
		return Position >= 1 && Position <= Table.Num() ? Table[Position - 1] : 0;
	}

	FString PresetName(EApexPointsPreset Preset)
	{
		switch (Preset)
		{
		case EApexPointsPreset::Classic: return TEXT("Classic: 10-6-4-3-2-1");
		case EApexPointsPreset::Deep:    return TEXT("Deep field: 50 down to 1");
		default:                         return TEXT("Modern: 25-18-15...1");
		}
	}

	const TArray<int32>& BallastTable(EApexSuccessBallast Preset)
	{
		static const TArray<int32> None;
		static const TArray<int32> Light = {30, 20, 10};
		static const TArray<int32> Heavy = {75, 57, 48, 39, 30, 21, 15, 9, 3};
		switch (Preset)
		{
		case EApexSuccessBallast::Light: return Light;
		case EApexSuccessBallast::Heavy: return Heavy;
		default:                         return None;
		}
	}

	FString BallastName(EApexSuccessBallast Preset)
	{
		switch (Preset)
		{
		case EApexSuccessBallast::Light: return TEXT("Light ballast: 30-20-10 kg");
		case EApexSuccessBallast::Heavy: return TEXT("Heavy ballast: 75 kg down to 3");
		default:                         return FString();
		}
	}

	namespace
	{
		/** Ahead by points, then wins, seconds, thirds..., then by key. */
		bool StandsAhead(const FStanding& A, const FStanding& B)
		{
			if (A.Points != B.Points)
			{
				return A.Points > B.Points;
			}
			const int32 Places = FMath::Max(A.Places.Num(), B.Places.Num());
			for (int32 Place = 0; Place < Places; ++Place)
			{
				const int32 OfA = A.Places.IsValidIndex(Place) ? A.Places[Place] : 0;
				const int32 OfB = B.Places.IsValidIndex(Place) ? B.Places[Place] : 0;
				if (OfA != OfB)
				{
					return OfA > OfB;
				}
			}
			return A.Key.Compare(B.Key, ESearchCase::IgnoreCase) < 0;
		}

		/** A line's points from a round: its place's, and the fastest lap's when it took them. */
		int32 RoundPointsOf(const FApexSeason& Season, const FApexSeasonRound& Round, int32 Index, int32 Scorer)
		{
			return PointsFor(Season.Points, Round.Results[Index].Position) + (Index == Scorer ? Season.FastestLapPoints : 0);
		}

		void Tally(FStanding& Line, int32 Round, int32 Rounds, int32 Position, int32 Points)
		{
			if (Line.RoundPoints.Num() != Rounds)
			{
				Line.RoundPoints.Init(-1, Rounds);
				Line.Dropped.Init(false, Rounds);
			}
			Line.RoundPoints[Round] = FMath::Max(Line.RoundPoints[Round], 0) + Points;
			if (Position >= 1)
			{
				if (Line.Places.Num() < Position)
				{
					Line.Places.SetNumZeroed(Position);
				}
				++Line.Places[Position - 1];
			}
		}
	}

	TArray<FStanding> DriverStandings(const FApexSeason& Season)
	{
		const int32 Rounds = Season.Rounds.Num();
		TArray<FStanding> Lines;
		auto LineFor = [&Lines](const FApexSeasonEntry& Entry) -> FStanding&
		{
			FStanding* Found = Lines.FindByPredicate([&Entry](const FStanding& L)
			{
				return Entry.bYou ? L.bYou : (!L.bYou && L.Key == Entry.Name);
			});
			if (Found)
			{
				if (Entry.bYou)
				{
					Found->Key = Entry.Name;
				}
				return *Found;
			}
			FStanding& Line = Lines.AddDefaulted_GetRef();
			Line.Key = Entry.Name;
			Line.bYou = Entry.bYou;
			Line.bIsAi = Entry.bIsAi;
			return Line;
		};
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			if (!Season.Rounds[Round].bDone)
			{
				continue;
			}
			const FApexSeasonRound& R = Season.Rounds[Round];
			const int32 Scorer = FastestLapScorer(Season, R);
			for (int32 Index = 0; Index < R.Results.Num(); ++Index)
			{
				const FApexSeasonEntry& Entry = R.Results[Index];
				FStanding& Line = LineFor(Entry);
				Tally(Line, Round, Rounds, Entry.Position, RoundPointsOf(Season, R, Index, Scorer));
				Line.FastestLaps += Index == Scorer ? 1 : 0;
			}
		}
		for (FStanding& Line : Lines)
		{
			// The worst rounds go first: a round not entered counts as nothing
			// and is dropped before a scoring one.
			TArray<int32> Order;
			for (int32 Round = 0; Round < Rounds; ++Round)
			{
				if (Season.Rounds[Round].bDone)
				{
					Order.Add(Round);
				}
			}
			Order.StableSort([&Line](int32 A, int32 B) { return FMath::Max(Line.RoundPoints[A], 0) < FMath::Max(Line.RoundPoints[B], 0); });
			const int32 Drops = FMath::Clamp(Season.DropRounds, 0, FMath::Max(0, Order.Num() - 1));
			for (int32 Drop = 0; Drop < Drops; ++Drop)
			{
				Line.Dropped[Order[Drop]] = true;
			}
			Line.Points = 0;
			for (int32 Round = 0; Round < Rounds; ++Round)
			{
				Line.Points += Line.Dropped[Round] ? 0 : FMath::Max(Line.RoundPoints[Round], 0);
			}
			Line.Wins = Line.Places.IsValidIndex(0) ? Line.Places[0] : 0;
		}
		Lines.Sort(StandsAhead);
		return Lines;
	}

	TArray<FStanding> CarStandings(const FApexSeason& Season)
	{
		const int32 Rounds = Season.Rounds.Num();
		TArray<FStanding> Lines;
		for (int32 Round = 0; Round < Rounds; ++Round)
		{
			if (!Season.Rounds[Round].bDone)
			{
				continue;
			}
			const FApexSeasonRound& R = Season.Rounds[Round];
			const int32 Scorer = FastestLapScorer(Season, R);
			for (int32 Index = 0; Index < R.Results.Num(); ++Index)
			{
				const FApexSeasonEntry& Entry = R.Results[Index];
				if (Entry.CarId.IsEmpty())
				{
					continue;
				}
				FStanding* Line = Lines.FindByPredicate([&Entry](const FStanding& L) { return L.Key.Equals(Entry.CarId, ESearchCase::IgnoreCase); });
				if (!Line)
				{
					Line = &Lines.AddDefaulted_GetRef();
					Line->Key = Entry.CarId;
				}
				const int32 Points = RoundPointsOf(Season, R, Index, Scorer);
				Tally(*Line, Round, Rounds, Entry.Position, Points);
				Line->Points += Points;
				Line->FastestLaps += Index == Scorer ? 1 : 0;
				Line->bYou |= Entry.bYou;
			}
		}
		for (FStanding& Line : Lines)
		{
			Line.Wins = Line.Places.IsValidIndex(0) ? Line.Places[0] : 0;
		}
		Lines.Sort(StandsAhead);
		return Lines;
	}

	bool FileRound(FApexSeason& Season, int32 Round, const TArray<FApexSeasonEntry>& Results)
	{
		if (Round == INDEX_NONE || Round != Season.NextRound() || Results.Num() == 0)
		{
			return false;
		}
		Season.Rounds[Round].Results = Results;
		Season.Rounds[Round].bDone = true;
		return true;
	}

	TArray<FString> SpreadCalendar(const TArray<FString>& TrackIds, int32 Count)
	{
		TArray<FString> Out;
		const int32 Total = TrackIds.Num();
		Count = FMath::Clamp(Count, 0, Total);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			// The middle of each of Count equal slices of the list.
			Out.Add(TrackIds[FMath::Min(Total - 1, (2 * Index + 1) * Total / (2 * Count))]);
		}
		return Out;
	}

	FApexSeasonSummary Summarise(const FApexSeason& Season)
	{
		FApexSeasonSummary Summary;
		Summary.Name = Season.Name;
		Summary.CarClass = Season.CarClass;
		Summary.Rounds = Season.RoundsDone();
		const TArray<FStanding> Standings = DriverStandings(Season);
		if (Standings.Num() > 0)
		{
			Summary.Champion = Standings[0].Key;
		}
		const int32 Mine = Standings.IndexOfByPredicate([](const FStanding& S) { return S.bYou; });
		if (Mine != INDEX_NONE)
		{
			Summary.YourPosition = Mine + 1;
			Summary.YourPoints = Standings[Mine].Points;
		}
		return Summary;
	}

	int32 FastestLapIndex(const FApexSeasonRound& Round)
	{
		int32 Best = INDEX_NONE;
		for (int32 Index = 0; Index < Round.Results.Num(); ++Index)
		{
			const float Lap = Round.Results[Index].BestLapSeconds;
			if (Lap > 0.0f && (Best == INDEX_NONE || Lap < Round.Results[Best].BestLapSeconds))
			{
				Best = Index;
			}
		}
		return Best;
	}

	int32 FastestLapScorer(const FApexSeason& Season, const FApexSeasonRound& Round)
	{
		if (Season.FastestLapPoints <= 0)
		{
			return INDEX_NONE;
		}
		const int32 Fastest = FastestLapIndex(Round);
		if (Fastest == INDEX_NONE || (Season.bFastestLapNeedsPoints && PointsFor(Season.Points, Round.Results[Fastest].Position) <= 0))
		{
			return INDEX_NONE;
		}
		return Fastest;
	}

	FString FastestLapRuleName(const FApexSeason& Season)
	{
		if (Season.FastestLapPoints <= 0)
		{
			return FString();
		}
		return FString::Printf(TEXT("+%d point%s for the fastest lap%s"), Season.FastestLapPoints, Season.FastestLapPoints == 1 ? TEXT("") : TEXT("s"),
			Season.bFastestLapNeedsPoints ? TEXT(", in the points") : TEXT(""));
	}

	FRoundSession RoundSession(const FApexSeason& Season, int32 Round, const FApexSessionConditions& Base)
	{
		FRoundSession Out;
		Out.Conditions = Base;
		Out.Laps = Season.Laps;
		if (!Season.Rounds.IsValidIndex(Round))
		{
			return Out;
		}
		const FApexSeasonRound& R = Season.Rounds[Round];
		Out.TrackId = R.TrackId;
		if (R.Laps > 0)
		{
			Out.Laps = R.Laps;
		}
		Out.RaceSeconds = ApexRaceLength::Clamp(R.RaceMinutes * 60);
		Out.QualifyingSeconds = ApexQualifyingLength::Clamp(R.QualifyingMinutes * 60);
		Out.StartMode = Out.QualifyingSeconds > 0 ? EApexGameMode::Qualification : EApexGameMode::Race;
		if (R.Weather >= 0)
		{
			Out.Conditions.Weather = static_cast<EApexWeather>(FMath::Clamp(R.Weather, 0, FApexSessionConditions::WeatherCount - 1));
			Out.Conditions.Forecast.Reset();
			if (Out.Conditions.HasAirTemp() && Out.Conditions.IsSnow())
			{
				Out.Conditions.AirTempC = FMath::Min(Out.Conditions.AirTempC, FApexSessionConditions::MaxSnowAirTempC);
			}
		}
		if (R.StartHour >= 0)
		{
			Out.Conditions.TimeOfDayMinutes = FMath::Clamp(R.StartHour, 0, 23) * 60;
		}
		Out.Ballast = Ballast(Season);
		return Out;
	}

	FString DescribeRound(const FApexSeason& Season, int32 Round)
	{
		if (!Season.Rounds.IsValidIndex(Round))
		{
			return FString();
		}
		const FApexSeasonRound& R = Season.Rounds[Round];
		TArray<FString> Parts;
		if (R.QualifyingMinutes > 0)
		{
			Parts.Add(FString::Printf(TEXT("Qualifying %s"), *ApexRaceLength::Describe(ApexQualifyingLength::Clamp(R.QualifyingMinutes * 60))));
		}
		Parts.Add(R.RaceMinutes > 0 ? ApexRaceLength::Describe(ApexRaceLength::Clamp(R.RaceMinutes * 60))
			: FString::Printf(TEXT("%d laps"), R.Laps > 0 ? R.Laps : Season.Laps));
		if (R.Weather >= 0)
		{
			Parts.Add(FApexSessionConditions::WeatherLabel(static_cast<EApexWeather>(FMath::Clamp(R.Weather, 0, FApexSessionConditions::WeatherCount - 1))));
		}
		if (R.StartHour >= 0)
		{
			Parts.Add(FString::Printf(TEXT("%02d:00"), FMath::Clamp(R.StartHour, 0, 23)));
		}
		return FString::Join(Parts, TEXT(" · "));
	}

	TArray<FApexDriverBallast> Ballast(const FApexSeason& Season)
	{
		TArray<FApexDriverBallast> Out;
		const TArray<int32>& Table = BallastTable(Season.Ballast);
		if (Table.IsEmpty() || Season.RoundsDone() == 0)
		{
			return Out;
		}
		const TArray<FStanding> Standings = DriverStandings(Season);
		for (int32 Place = 0; Place < Standings.Num() && Place < Table.Num(); ++Place)
		{
			const FStanding& Driver = Standings[Place];
			if (Driver.Points > 0)
			{
				Out.Add({Driver.bYou ? FString(TEXT("@host")) : Driver.Key, Table[Place]});
			}
		}
		return Out;
	}
}
