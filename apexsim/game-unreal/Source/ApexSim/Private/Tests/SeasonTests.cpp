#include "ApexTestCommon.h"
#include "ApexProtocolCodec.h"
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

namespace
{
	FApexSeasonEntry Timed(const TCHAR* Name, int32 Position, float BestLap, bool bYou = false)
	{
		FApexSeasonEntry Entry = Line(Name, Position, bYou);
		Entry.BestLapSeconds = BestLap;
		return Entry;
	}

	int32 PointsOf(const TArray<ApexSeason::FStanding>& Lines, const TCHAR* Key)
	{
		const ApexSeason::FStanding* Found = Lines.FindByPredicate([Key](const ApexSeason::FStanding& S) { return S.Key == Key; });
		return Found ? Found->Points : -1;
	}

	/** Whether the encoded message names a key (msgpack writes map keys as their bytes). */
	bool NamesKey(const TArray<uint8>& Bytes, const char* Key)
	{
		const int32 Length = FCStringAnsi::Strlen(Key);
		for (int32 At = 0; At + Length <= Bytes.Num(); ++At)
		{
			if (FMemory::Memcmp(Bytes.GetData() + At, Key, Length) == 0)
			{
				return true;
			}
		}
		return false;
	}

	TArray<uint8> EncodeRound(const ApexSeason::FRoundSession& Session)
	{
		return ApexProtocol::EncodeCreateSession(Session.TrackId, 12, 11, static_cast<uint8>(Session.Laps), EApexSessionKind::Practice,
			FApexAllowedAssists(), Session.Conditions, EApexDamageLevel::Full, ApexAiSkill::Mixed, Session.RaceSeconds, TArray<FString>(),
			Session.QualifyingSeconds, FApexWeekendFormat(), nullptr, 0, 0, false, EApexQualifyingFormat::Open, 0, FApexRaceRules(),
			TArray<FApexClassEntry>(), Session.Ballast);
	}
}

// The fastest lap's points: the round's quickest best lap takes them, a
// DNF's included, unless the season wants a points finish; a season without
// the rule, and a round filed before best laps were kept, score as before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonFastestLapTest, "ApexSim.Season.FastestLap", ApexTestFlags)

bool FApexSeasonFastestLapTest::RunTest(const FString& Parameters)
{
	FApexSeason Season = ThreeRounds();
	Season.Points = EApexPointsPreset::Classic;
	// Round 1: the fastest lap is the winner's. Round 2: a car out of the
	// points (P7, Classic pays six) set it. Round 3: a car that did not finish.
	ApexSeason::FileRound(Season, 0, { Timed(TEXT("Max"), 1, 80.1f), Timed(TEXT("Luna"), 2, 80.4f), Timed(TEXT("Guido"), 3, 0.0f, true) });
	ApexSeason::FileRound(Season, 1, { Timed(TEXT("Luna"), 1, 81.0f), Timed(TEXT("Max"), 2, 81.2f), Timed(TEXT("Ada"), 7, 80.9f) });
	ApexSeason::FileRound(Season, 2, { Timed(TEXT("Luna"), 1, 82.0f), Timed(TEXT("Guido"), 2, 82.5f, true), Timed(TEXT("Bo"), 0, 81.5f) });

	TestEqual(TEXT("round 1's fastest"), ApexSeason::FastestLapIndex(Season.Rounds[0]), 0);
	TestEqual(TEXT("round 2's fastest"), ApexSeason::FastestLapIndex(Season.Rounds[1]), 2);
	TestEqual(TEXT("no rule, no scorer"), ApexSeason::FastestLapScorer(Season, Season.Rounds[0]), INDEX_NONE);
	const TArray<ApexSeason::FStanding> Before = ApexSeason::DriverStandings(Season);
	TestEqual(TEXT("as before: Luna"), PointsOf(Before, TEXT("Luna")), 26);
	TestEqual(TEXT("as before: Ada"), PointsOf(Before, TEXT("Ada")), 0);

	// For anyone: each round's fastest lap scores, wherever it finished.
	Season.FastestLapPoints = 1;
	Season.bFastestLapNeedsPoints = false;
	const TArray<ApexSeason::FStanding> Anyone = ApexSeason::DriverStandings(Season);
	TestEqual(TEXT("Max: 10 + 6 and round 1's lap"), PointsOf(Anyone, TEXT("Max")), 17);
	TestEqual(TEXT("Ada out of the points still takes it"), PointsOf(Anyone, TEXT("Ada")), 1);
	TestEqual(TEXT("Bo did not finish and takes it"), PointsOf(Anyone, TEXT("Bo")), 1);
	TestEqual(TEXT("Luna never had it"), PointsOf(Anyone, TEXT("Luna")), 26);
	const ApexSeason::FStanding* Max = Anyone.FindByPredicate([](const ApexSeason::FStanding& S) { return S.Key == TEXT("Max"); });
	if (TestNotNull(TEXT("Max"), Max))
	{
		TestEqual(TEXT("one fastest lap"), Max->FastestLaps, 1);
		TestEqual(TEXT("round 1 carries it"), Max->RoundPoints[0], 11);
	}
	const TArray<ApexSeason::FStanding> Cars = ApexSeason::CarStandings(Season);
	TestEqual(TEXT("the car takes every fastest lap"), Cars.Num() > 0 ? Cars[0].Points : -1, 26 + 10 + 6 + 4 + 6 + 3);

	// In the points: only round 1's lap scores; nobody takes rounds 2 and 3's.
	Season.FastestLapPoints = 2;
	Season.bFastestLapNeedsPoints = true;
	TestEqual(TEXT("round 2: nobody"), ApexSeason::FastestLapScorer(Season, Season.Rounds[1]), INDEX_NONE);
	TestEqual(TEXT("round 3: nobody"), ApexSeason::FastestLapScorer(Season, Season.Rounds[2]), INDEX_NONE);
	const TArray<ApexSeason::FStanding> InPoints = ApexSeason::DriverStandings(Season);
	TestEqual(TEXT("Max takes two"), PointsOf(InPoints, TEXT("Max")), 18);
	TestEqual(TEXT("Ada nothing"), PointsOf(InPoints, TEXT("Ada")), 0);
	TestEqual(TEXT("Bo nothing"), PointsOf(InPoints, TEXT("Bo")), 0);

	// A round filed before best laps were kept has no fastest lap.
	FApexSeason Old = ThreeRounds();
	Old.FastestLapPoints = 1;
	ApexSeason::FileRound(Old, 0, { Line(TEXT("Max"), 1), Line(TEXT("Luna"), 2) });
	TestEqual(TEXT("an old round has no fastest lap"), ApexSeason::FastestLapIndex(Old.Rounds[0]), INDEX_NONE);
	TestEqual(TEXT("and scores as it did"), ApexSeason::DriverStandings(Old)[0].Points, 25);
	TestEqual(TEXT("the rule's name"), ApexSeason::FastestLapRuleName(Season), FString(TEXT("+2 points for the fastest lap, in the points")));
	return true;
}

// Each round's own rules reach the session it makes: a round left alone is
// the season's race byte for byte; weather, start, qualifying and a timed
// race go out as CreateSession's fields.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexSeasonRoundRulesTest, "ApexSim.Season.RoundRules", ApexTestFlags)

bool FApexSeasonRoundRulesTest::RunTest(const FString& Parameters)
{
	FApexSeason Season = ThreeRounds();
	Season.Laps = 8;
	FApexSessionConditions Base;
	Base.Weather = EApexWeather::Cloudy;
	Base.TimeOfDayMinutes = 15 * 60 + 30;
	Base.AirTempC = 20;
	Base.Forecast.Add({ 30, EApexWeather::LightRain });

	// Left alone: the season's laps under the profile's sky, a race and nothing else.
	const ApexSeason::FRoundSession Plain = ApexSeason::RoundSession(Season, 0, Base);
	TestEqual(TEXT("the round's circuit"), Plain.TrackId, FString(TEXT("t1")));
	TestEqual(TEXT("the season's laps"), Plain.Laps, 8);
	TestEqual(TEXT("over laps"), Plain.RaceSeconds, 0);
	TestEqual(TEXT("no qualifying"), Plain.QualifyingSeconds, 0);
	TestTrue(TEXT("counts into the race"), Plain.StartMode == EApexGameMode::Race);
	TestTrue(TEXT("no rules of its own"), !Season.Rounds[0].HasOwnRules());
	const TArray<uint8> Before = ApexProtocol::EncodeCreateSession(TEXT("t1"), 12, 11, 8, EApexSessionKind::Practice,
		FApexAllowedAssists(), Base, EApexDamageLevel::Full, ApexAiSkill::Mixed, 0, TArray<FString>(), 0, FApexWeekendFormat(),
		nullptr, 0, 0, false, EApexQualifyingFormat::Open, 0, FApexRaceRules(), TArray<FApexClassEntry>(), TArray<FApexDriverBallast>());
	TestTrue(TEXT("the season's race byte for byte"), EncodeRound(Plain) == Before);
	TestEqual(TEXT("described"), ApexSeason::DescribeRound(Season, 0), FString(TEXT("8 laps")));

	// Round 2: heavy rain at 18:00, 15 min qualifying, a 45-minute race.
	FApexSeasonRound& Own = Season.Rounds[1];
	Own.Weather = static_cast<int32>(EApexWeather::HeavyRain);
	Own.StartHour = 18;
	Own.QualifyingMinutes = 15;
	Own.RaceMinutes = 45;
	TestTrue(TEXT("rules of its own"), Own.HasOwnRules());
	const ApexSeason::FRoundSession Rules = ApexSeason::RoundSession(Season, 1, Base);
	TestEqual(TEXT("a timed race"), Rules.RaceSeconds, 45 * 60);
	TestEqual(TEXT("timed qualifying"), Rules.QualifyingSeconds, 15 * 60);
	TestTrue(TEXT("counts into qualifying"), Rules.StartMode == EApexGameMode::Qualification);
	TestTrue(TEXT("the round's weather"), Rules.Conditions.Weather == EApexWeather::HeavyRain);
	TestEqual(TEXT("the round's start"), Rules.Conditions.TimeOfDayMinutes, 18 * 60);
	TestEqual(TEXT("the profile's air kept"), Rules.Conditions.AirTempC, 20);
	TestEqual(TEXT("the profile's forecast dropped under the round's sky"), Rules.Conditions.Forecast.Num(), 0);
	const TArray<uint8> Bytes = EncodeRound(Rules);
	TestTrue(TEXT("race_seconds on the wire"), NamesKey(Bytes, "race_seconds"));
	TestTrue(TEXT("qualifying_seconds on the wire"), NamesKey(Bytes, "qualifying_seconds"));
	TestFalse(TEXT("neither for the plain round"), NamesKey(Before, "race_seconds") || NamesKey(Before, "qualifying_seconds"));
	FApexSessionConditions Expected = Base;
	Expected.Weather = EApexWeather::HeavyRain;
	Expected.TimeOfDayMinutes = 18 * 60;
	Expected.Forecast.Reset();
	TestTrue(TEXT("the round's create, field for field"), Bytes == ApexProtocol::EncodeCreateSession(TEXT("t2"), 12, 11, 8, EApexSessionKind::Practice,
		FApexAllowedAssists(), Expected, EApexDamageLevel::Full, ApexAiSkill::Mixed, 45 * 60, TArray<FString>(), 15 * 60, FApexWeekendFormat(),
		nullptr, 0, 0, false, EApexQualifyingFormat::Open, 0, FApexRaceRules(), TArray<FApexClassEntry>(), TArray<FApexDriverBallast>()));
	TestEqual(TEXT("described"), ApexSeason::DescribeRound(Season, 1), FString(TEXT("Qualifying 15 min · 45 min · Heavy rain · 18:00")));

	// Round 3: its own laps; snow holds the profile's air at what snow takes.
	Season.Rounds[2].Laps = 12;
	Season.Rounds[2].Weather = static_cast<int32>(EApexWeather::Snow);
	const ApexSeason::FRoundSession Snow = ApexSeason::RoundSession(Season, 2, Base);
	TestEqual(TEXT("the round's laps"), Snow.Laps, 12);
	TestEqual(TEXT("still over laps"), Snow.RaceSeconds, 0);
	TestEqual(TEXT("snow's air"), Snow.Conditions.AirTempC, FApexSessionConditions::MaxSnowAirTempC);
	TestEqual(TEXT("the profile's start"), Snow.Conditions.TimeOfDayMinutes, 15 * 60 + 30);

	// The rules back to the season's: the plain race again.
	Own.ClearRules();
	TestTrue(TEXT("cleared"), EncodeRound(ApexSeason::RoundSession(Season, 1, Base)) == ApexProtocol::EncodeCreateSession(TEXT("t2"), 12, 11, 8,
		EApexSessionKind::Practice, FApexAllowedAssists(), Base, EApexDamageLevel::Full, ApexAiSkill::Mixed, 0, TArray<FString>(), 0,
		FApexWeekendFormat(), nullptr, 0, 0, false, EApexQualifyingFormat::Open, 0, FApexRaceRules(), TArray<FApexClassEntry>(), TArray<FApexDriverBallast>()));
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
