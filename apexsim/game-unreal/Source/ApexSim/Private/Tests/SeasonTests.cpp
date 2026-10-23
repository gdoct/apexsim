#include "ApexTestCommon.h"
#include "Season/ApexSeason.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FApexSeasonEntry Line(const TCHAR* Name, int32 Position, bool bYou = false, const TCHAR* Car = TEXT("car-a"))
	{
		FApexSeasonEntry Entry;
		Entry.Name = Name;
		Entry.Position = Position;
		Entry.bYou = bYou;
		Entry.bIsAi = !bYou;
		Entry.CarId = Car;
		return Entry;
	}

	FApexSeason ThreeRounds()
	{
		FApexSeason Season;
		for (const TCHAR* Track : { TEXT("t1"), TEXT("t2"), TEXT("t3") })
		{
			Season.Rounds.AddDefaulted_GetRef().TrackId = Track;
		}
		return Season;
	}
}

// Points presets and the order of the standings: points, then wins, then
// seconds; the player is one driver under any name; a round not classified
// scores nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonStandingsTest, "ApexSim.Season.Standings", ApexTestFlags)

bool FApexSeasonStandingsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("modern P1"), ApexSeason::PointsFor(EApexPointsPreset::Modern, 1), 25);
	TestEqual(TEXT("modern P10"), ApexSeason::PointsFor(EApexPointsPreset::Modern, 10), 1);
	TestEqual(TEXT("modern P11"), ApexSeason::PointsFor(EApexPointsPreset::Modern, 11), 0);
	TestEqual(TEXT("classic P6"), ApexSeason::PointsFor(EApexPointsPreset::Classic, 6), 1);
	TestEqual(TEXT("deep P1"), ApexSeason::PointsFor(EApexPointsPreset::Deep, 1), 50);
	TestEqual(TEXT("deep P6"), ApexSeason::PointsFor(EApexPointsPreset::Deep, 6), 25);
	TestEqual(TEXT("deep P30"), ApexSeason::PointsFor(EApexPointsPreset::Deep, 30), 1);
	TestEqual(TEXT("a DNF"), ApexSeason::PointsFor(EApexPointsPreset::Modern, 0), 0);

	FApexSeason Season = ThreeRounds();
	Season.Points = EApexPointsPreset::Classic;
	TestTrue(TEXT("active"), Season.IsActive());
	TestFalse(TEXT("round 2 is not next"), ApexSeason::FileRound(Season, 1, { Line(TEXT("Max"), 1) }));
	TestTrue(TEXT("round 1"), ApexSeason::FileRound(Season, 0,
		{ Line(TEXT("Max"), 1), Line(TEXT("Guido"), 2, true), Line(TEXT("Luna"), 3, false, TEXT("car-b")) }));
	TestTrue(TEXT("round 2, renamed"), ApexSeason::FileRound(Season, 1,
		{ Line(TEXT("Luna"), 1, false, TEXT("car-b")), Line(TEXT("Guido D"), 3, true), Line(TEXT("Max"), 0) }));
	TestEqual(TEXT("next is round 3"), Season.NextRound(), 2);

	const TArray<ApexSeason::FStanding> Drivers = ApexSeason::DriverStandings(Season);
	if (TestEqual(TEXT("three drivers"), Drivers.Num(), 3))
	{
		TestEqual(TEXT("the leader"), Drivers[0].Key, FString(TEXT("Luna")));
		TestEqual(TEXT("the leader's points"), Drivers[0].Points, 14);
		// Max and the player both have 10: Max won a race, the player did not.
		TestEqual(TEXT("tie broken by wins"), Drivers[1].Key, FString(TEXT("Max")));
		TestTrue(TEXT("the player is one driver"), Drivers[2].bYou && Drivers[2].Points == 10);
		TestEqual(TEXT("under their latest name"), Drivers[2].Key, FString(TEXT("Guido D")));
	}
	const TArray<ApexSeason::FStanding> Cars = ApexSeason::CarStandings(Season);
	if (TestEqual(TEXT("two cars"), Cars.Num(), 2))
	{
		TestEqual(TEXT("car-a has its drivers' points"), Cars[0].Points, 20);
		TestTrue(TEXT("the player's car"), Cars[0].bYou);
	}

	TestTrue(TEXT("round 3"), ApexSeason::FileRound(Season, 2, { Line(TEXT("Luna"), 1, false, TEXT("car-b")), Line(TEXT("Max"), 2) }));
	TestTrue(TEXT("finished"), Season.IsFinished());
	const FApexSeasonSummary Summary = ApexSeason::Summarise(Season);
	TestEqual(TEXT("champion"), Summary.Champion, FString(TEXT("Luna")));
	TestEqual(TEXT("the player's place"), Summary.YourPosition, 3);
	return true;
}

// Success ballast: none before a round is raced or without a preset, then
// each scoring driver by place, the player as the host.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonBallastTest, "ApexSim.Season.Ballast", ApexTestFlags)

bool FApexSeasonBallastTest::RunTest(const FString& Parameters)
{
	FApexSeason Season = ThreeRounds();
	Season.Points = EApexPointsPreset::Classic;
	Season.Ballast = EApexSuccessBallast::Light;
	TestTrue(TEXT("nothing before round 1"), ApexSeason::Ballast(Season).IsEmpty());
	ApexSeason::FileRound(Season, 0, { Line(TEXT("Max"), 1), Line(TEXT("Guido"), 2, true), Line(TEXT("Luna"), 3),
		Line(TEXT("Ada"), 4), Line(TEXT("Bo"), 0) });
	const TArray<FApexDriverBallast> Ballast = ApexSeason::Ballast(Season);
	if (TestEqual(TEXT("the top three"), Ballast.Num(), 3))
	{
		TestEqual(TEXT("the leader by name"), Ballast[0].Driver, FString(TEXT("Max")));
		TestEqual(TEXT("30 kg"), Ballast[0].Kg, 30);
		TestEqual(TEXT("the player as the host"), Ballast[1].Driver, FString(TEXT("@host")));
		TestEqual(TEXT("20 kg"), Ballast[1].Kg, 20);
		TestEqual(TEXT("third"), Ballast[2].Kg, 10);
	}
	Season.Ballast = EApexSuccessBallast::Heavy;
	// Four drivers scored (Classic pays six places); the fifth did not finish.
	TestEqual(TEXT("only the drivers with points"), ApexSeason::Ballast(Season).Num(), 4);
	TestEqual(TEXT("heavy's leader"), ApexSeason::Ballast(Season)[0].Kg, 75);
	Season.Ballast = EApexSuccessBallast::None;
	TestTrue(TEXT("none without a preset"), ApexSeason::Ballast(Season).IsEmpty());
	return true;
}

// Dropped rounds: each driver's worst rounds go, a missed round first; the
// last round is never dropped away to nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonDropsTest, "ApexSim.Season.Drops", ApexTestFlags)

bool FApexSeasonDropsTest::RunTest(const FString& Parameters)
{
	FApexSeason Season = ThreeRounds();
	Season.DropRounds = 1;
	ApexSeason::FileRound(Season, 0, { Line(TEXT("Max"), 1), Line(TEXT("Luna"), 2) });
	ApexSeason::FileRound(Season, 1, { Line(TEXT("Luna"), 1) });
	ApexSeason::FileRound(Season, 2, { Line(TEXT("Max"), 10), Line(TEXT("Luna"), 2) });
	const TArray<ApexSeason::FStanding> Drivers = ApexSeason::DriverStandings(Season);
	const ApexSeason::FStanding* Max = Drivers.FindByPredicate([](const ApexSeason::FStanding& S) { return S.Key == TEXT("Max"); });
	const ApexSeason::FStanding* Luna = Drivers.FindByPredicate([](const ApexSeason::FStanding& S) { return S.Key == TEXT("Luna"); });
	if (TestNotNull(TEXT("Max"), Max) && TestNotNull(TEXT("Luna"), Luna))
	{
		TestEqual(TEXT("Max drops the round he missed"), Max->Points, 26);
		TestTrue(TEXT("round 2 dropped"), Max->Dropped[1]);
		TestEqual(TEXT("Luna drops one 18"), Luna->Points, 43);
	}
	FApexSeason One = ThreeRounds();
	One.DropRounds = 3;
	ApexSeason::FileRound(One, 0, { Line(TEXT("Max"), 1) });
	TestEqual(TEXT("one round raced keeps its points"), ApexSeason::DriverStandings(One)[0].Points, 25);
	return true;
}

// The calendar spreads over the circuits rather than taking the first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonCalendarTest, "ApexSim.Season.Calendar", ApexTestFlags)

bool FApexSeasonCalendarTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Tracks = { TEXT("a"), TEXT("b"), TEXT("c"), TEXT("d"), TEXT("e"), TEXT("f"), TEXT("g"), TEXT("h") };
	TestEqual(TEXT("four of eight"), FString::Join(ApexSeason::SpreadCalendar(Tracks, 4), TEXT("")), FString(TEXT("bdfh")));
	TestEqual(TEXT("all of them"), FString::Join(ApexSeason::SpreadCalendar(Tracks, 8), TEXT("")), FString(TEXT("abcdefgh")));
	TestEqual(TEXT("more than there are"), ApexSeason::SpreadCalendar(Tracks, 20).Num(), 8);
	TestEqual(TEXT("one"), FString::Join(ApexSeason::SpreadCalendar(Tracks, 1), TEXT("")), FString(TEXT("e")));
	return true;
}

#endif
