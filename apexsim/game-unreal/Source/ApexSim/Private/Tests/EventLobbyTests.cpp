#include "ApexTestCommon.h"
#include "UI/ApexEventLabels.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEventLobbyLabelsTest,
	"ApexSim.UI.EventLobby.Labels",
	ApexTestFlags)

bool FApexEventLobbyLabelsTest::RunTest(const FString& Parameters)
{
	FApexSessionEvent Event;
	Event.Id = TEXT("autumn-cup");
	Event.Name = TEXT("Autumn Cup");

	// Resumed at its race after a restart: the weekend stands at its lobby,
	// practice and qualifying are done, the race comes next.
	Event.Phase = 3;
	TestEqual(TEXT("resumed: the event's phase"), ApexEventLabels::ShownPhase(Event, EApexWeekendPhase::Lobby), 3);
	TestEqual(TEXT("resumed: the state line"), ApexEventLabels::LobbyState(Event, EApexWeekendPhase::Lobby),
		FString(TEXT("AUTUMN CUP · RACE NEXT")));
	TestTrue(TEXT("resumed: practice done"), ApexEventLabels::IsPhaseDone(Event, EApexWeekendPhase::Lobby, 1));
	TestTrue(TEXT("resumed: qualifying done"), ApexEventLabels::IsPhaseDone(Event, EApexWeekendPhase::Lobby, 2));
	TestFalse(TEXT("resumed: the race not done"), ApexEventLabels::IsPhaseDone(Event, EApexWeekendPhase::Lobby, 3));
	TestEqual(TEXT("resumed: waiting line"), ApexEventLabels::WaitingLine(Event, EApexWeekendPhase::Lobby),
		FString(TEXT("WAITING FOR RACE CONTROL TO START THE RACE")));

	// A fresh event at its practice, then its weekend under way: the
	// weekend's own phase wins once it has begun.
	Event.Phase = 1;
	TestEqual(TEXT("fresh: practice next"), ApexEventLabels::LobbyState(Event, EApexWeekendPhase::Lobby),
		FString(TEXT("AUTUMN CUP · PRACTICE NEXT")));
	TestFalse(TEXT("fresh: nothing done"), ApexEventLabels::IsPhaseDone(Event, EApexWeekendPhase::Lobby, 1));
	TestEqual(TEXT("under way: the weekend's phase"), ApexEventLabels::LobbyState(Event, EApexWeekendPhase::Qualifying),
		FString(TEXT("AUTUMN CUP · QUALIFYING")));
	TestTrue(TEXT("under way: practice done"), ApexEventLabels::IsPhaseDone(Event, EApexWeekendPhase::Qualifying, 1));

	// Over.
	Event.Phase = 4;
	TestEqual(TEXT("over"), ApexEventLabels::LobbyState(Event, EApexWeekendPhase::Lobby), FString(TEXT("AUTUMN CUP · OVER")));

	// No event: nothing is done.
	TestFalse(TEXT("no event"), ApexEventLabels::IsPhaseDone(FApexSessionEvent(), EApexWeekendPhase::Race, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEventScheduleLabelsTest,
	"ApexSim.UI.EventLobby.Schedule",
	ApexTestFlags)

bool FApexEventScheduleLabelsTest::RunTest(const FString& Parameters)
{
	using namespace ApexEventLabels;
	TestEqual(TEXT("seconds"), Countdown(9), FString(TEXT("0:09")));
	TestEqual(TEXT("minutes"), Countdown(299), FString(TEXT("4:59")));
	TestEqual(TEXT("hours"), Countdown(3723), FString(TEXT("1:02:03")));
	TestEqual(TEXT("days"), Countdown(2 * 86400 + 4 * 3600 + 59), FString(TEXT("2D 04H")));
	TestEqual(TEXT("past"), Countdown(-5), FString(TEXT("0:00")));

	TestEqual(TEXT("to come"), ScheduledLine(3, 1300, 1000), FString(TEXT("THE RACE STARTS IN 5:00")));
	TestEqual(TEXT("its time"), ScheduledLine(1, 1000, 1000), FString(TEXT("THE PRACTICE IS STARTING")));
	TestTrue(TEXT("no time"), ScheduledLine(3, 0, 1000).IsEmpty());
	TestTrue(TEXT("over"), ScheduledLine(4, 1300, 1000).IsEmpty());

	FApexLobbyEvent Cup;
	Cup.Id = TEXT("autumn-cup");
	Cup.Name = TEXT("Autumn Cup");
	Cup.Phase = 2;
	Cup.StartsAt = 1300;
	TestEqual(TEXT("not open, to come"), Badge(Cup, nullptr, 1000), FString(TEXT("In 5:00")));
	TestEqual(TEXT("not open, its time gone"), Badge(Cup, nullptr, 1400), FString(TEXT("Not open")));
	TestEqual(TEXT("qualifying next"), PhaseWords(Cup, nullptr), FString(TEXT("Qualifying next")));

	FApexSessionSummary Session;
	Session.State = EApexSessionState::Lobby;
	TestEqual(TEXT("open, to come"), Badge(Cup, &Session, 1000), FString(TEXT("In 5:00")));
	TestEqual(TEXT("open"), Badge(Cup, &Session, 1400), FString(TEXT("Open")));
	Session.State = EApexSessionState::Racing;
	TestEqual(TEXT("under way"), Badge(Cup, &Session, 1400), FString(TEXT("Under way")));
	TestEqual(TEXT("qualifying"), PhaseWords(Cup, &Session), FString(TEXT("Qualifying")));
	Session.State = EApexSessionState::Finished;
	Session.bWeekendWaiting = true;
	TestEqual(TEXT("between phases: open"), Badge(Cup, &Session, 1400), FString(TEXT("Open")));

	Cup.Phase = 4;
	TestEqual(TEXT("over"), Badge(Cup, nullptr, 1000), FString(TEXT("Over")));
	TestEqual(TEXT("over words"), PhaseWords(Cup, nullptr), FString(TEXT("Over")));

	TArray<FApexLobbyEvent> Events;
	auto Add = [&Events](const TCHAR* Name, int32 Phase, int64 StartsAt)
	{
		FApexLobbyEvent& E = Events.AddDefaulted_GetRef();
		E.Name = Name;
		E.Phase = Phase;
		E.StartsAt = StartsAt;
	};
	Add(TEXT("Done"), 4, 0);
	Add(TEXT("Zed"), 1, 0);
	Add(TEXT("Late"), 3, 900);
	Add(TEXT("Alpha"), 1, 0);
	Add(TEXT("Soon"), 1, 500);
	SortEvents(Events);
	TArray<FString> Order;
	for (const FApexLobbyEvent& E : Events)
	{
		Order.Add(E.Name);
	}
	TestEqual(TEXT("timed first, by hand by name, over last"), FString::Join(Order, TEXT(",")),
		FString(TEXT("Soon,Late,Alpha,Zed,Done")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEventSheetLabelsTest,
	"ApexSim.UI.EventLobby.Sheets",
	ApexTestFlags)

bool FApexEventSheetLabelsTest::RunTest(const FString& Parameters)
{
	using namespace ApexEventLabels;
	TestEqual(TEXT("a lap"), Ms(81234), FString(TEXT("1:21.234")));
	TestEqual(TEXT("past an hour"), Ms(3723456), FString(TEXT("1:02:03.456")));
	TestEqual(TEXT("none"), Ms(0), FString(TEXT("—")));

	FApexEventSheetLine Ada;
	Ada.Name = TEXT("Ada");
	Ada.Laps = 10;
	Ada.BestLapMs = 80100;
	Ada.RaceTimeMs = 812345;
	Ada.Status = TEXT("finished");
	TestEqual(TEXT("race: the time"), SheetTime(Ada, 3), FString(TEXT("13:32.345")));
	TestEqual(TEXT("practice: the best lap"), SheetTime(Ada, 1), FString(TEXT("1:20.100")));
	FApexEventSheetLine Bea;
	Bea.Name = TEXT("Bea");
	Bea.Laps = 1;
	Bea.Status = TEXT("retired");
	Bea.bAi = true;
	Bea.KnockedOutIn = 2;
	TestEqual(TEXT("race: retired"), SheetTime(Bea, 3), FString(TEXT("RETIRED")));
	TestEqual(TEXT("no lap"), SheetTime(Bea, 2), FString(TEXT("no time")));
	TestEqual(TEXT("notes"), SheetNotes(Bea), FString(TEXT("1 lap · out in Q2 · AI")));
	TestEqual(TEXT("no notes"), SheetNotes(FApexEventSheetLine()), FString());

	FApexEventSheet Race;
	Race.Phase = 3;
	Race.Lines = { Ada, Bea };
	TestEqual(TEXT("race headline"), SheetHeadline(Race), FString(TEXT("won by Ada in 13:32.345")));
	Race.Phase = 2;
	TestEqual(TEXT("qualifying headline"), SheetHeadline(Race), FString(TEXT("pole Ada 1:20.100")));
	TestEqual(TEXT("empty"), SheetHeadline(FApexEventSheet()), FString(TEXT("nobody took part")));

	FApexChampionship Series;
	Series.Rounds.SetNum(3);
	Series.Rounds[0].bRaced = true;
	Series.Rounds[1].bRaced = true;
	FApexChampionshipStanding Lead;
	Lead.Name = TEXT("Ada");
	Lead.Points = 43;
	Lead.Wins = 2;
	Lead.Results = { 1, 3, 0 };
	Series.Standings = { Lead };
	TestEqual(TEXT("championship line"), ChampionshipLine(Series), FString(TEXT("2 / 3 rounds raced · leader Ada 43 pts")));
	TestEqual(TEXT("standing line"), StandingLine(Lead), FString(TEXT("43 pts · 2 wins · R1 P1 · R2 P3 · R3 —")));
	Series.Rounds[0].bRaced = Series.Rounds[1].bRaced = false;
	TestEqual(TEXT("nothing raced: no leader"), ChampionshipLine(Series), FString(TEXT("0 / 3 rounds raced")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexEventChampionshipPanelTest,
	"ApexSim.UI.EventLobby.Championship",
	ApexTestFlags)

bool FApexEventChampionshipPanelTest::RunTest(const FString& Parameters)
{
	using namespace ApexEventLabels;
	// Two championships, the event a round of the second only.
	FApexChampionships All;
	All.Championships.SetNum(2);
	All.Championships[0].Name = TEXT("Spring Series");
	All.Championships[0].Rounds.SetNum(1);
	All.Championships[0].Rounds[0].EventId = TEXT("spring-cup");
	FApexChampionship& Series = All.Championships[1];
	Series.Name = TEXT("Autumn Series");
	Series.FastestLapPoints = 1;
	Series.Rounds.SetNum(3);
	Series.Rounds[0].EventId = TEXT("round-1");
	Series.Rounds[0].bRaced = true;
	Series.Rounds[1].EventId = TEXT("autumn-cup");
	Series.Rounds[2].EventId = TEXT("round-3");
	for (int32 Place = 1; Place <= 8; ++Place)
	{
		FApexChampionshipStanding Standing;
		Standing.Position = Place;
		Standing.Name = FString::Printf(TEXT("Driver %d"), Place);
		Standing.Points = 30 - Place;
		Series.Standings.Add(Standing);
	}
	Series.Standings[6].Name = TEXT("Ada");
	// An AI driver of the player's name is not the player.
	Series.Standings[2].Name = TEXT("Ada");
	Series.Standings[2].bAi = true;

	const TArray<const FApexChampionship*> Of = ChampionshipsOfEvent(All, TEXT("autumn-cup"));
	if (TestEqual(TEXT("one championship has the round"), Of.Num(), 1))
	{
		TestEqual(TEXT("which"), Of[0]->Name, FString(TEXT("Autumn Series")));
	}
	TestEqual(TEXT("no event, none"), ChampionshipsOfEvent(All, FString()).Num(), 0);
	TestEqual(TEXT("own line: the human of that name"), OwnStanding(Series, TEXT("ada")), 6);
	TestEqual(TEXT("not scored yet"), OwnStanding(Series, TEXT("Bea")), INDEX_NONE);
	const auto Joined = [](const TArray<int32>& Indices)
	{
		TArray<FString> Parts;
		for (const int32 Index : Indices)
		{
			Parts.Add(FString::FromInt(Index));
		}
		return FString::Join(Parts, TEXT(","));
	};
	TestEqual(TEXT("top five and the player's line"), Joined(ShownStandings(Series, TEXT("Ada"), 5)), FString(TEXT("0,1,2,3,4,6")));
	TestEqual(TEXT("the player in the top five: no extra line"), Joined(ShownStandings(Series, TEXT("Driver 2"), 5)), FString(TEXT("0,1,2,3,4")));
	TestEqual(TEXT("caption before the race"), StandingsCaption(Series, TEXT("autumn-cup")), FString(TEXT("ROUND 2 OF 3 · AFTER 1 ROUND")));
	TestEqual(TEXT("the rule"), FastestLapRule(Series), FString(TEXT("+1 pt for the fastest lap on a points finish")));

	// The race kept: counted, its fastest lap named and scored.
	FApexChampionshipRound& Round = Series.Rounds[1];
	Round.bRaced = true;
	Round.FastestLap = TEXT("Ada");
	Round.FastestLapMs = 81234;
	Round.bFastestLapScored = true;
	TestEqual(TEXT("caption after the race"), StandingsCaption(Series, TEXT("autumn-cup")), FString(TEXT("ROUND 2 OF 3 · AFTER 2 ROUNDS")));
	if (TestNotNull(TEXT("the event's round"), RoundOf(Series, TEXT("autumn-cup"))))
	{
		TestEqual(TEXT("fastest lap scored"), FastestLapLine(Series, *RoundOf(Series, TEXT("autumn-cup"))),
			FString(TEXT("fastest lap Ada 1:21.234 · +1 pt")));
	}
	Round.bFastestLapScored = false;
	TestEqual(TEXT("fastest lap not scored"), FastestLapLine(Series, Round), FString(TEXT("fastest lap Ada 1:21.234")));
	TestEqual(TEXT("no fastest lap"), FastestLapLine(Series, Series.Rounds[2]), FString());
	Series.bFastestLapAnyPlace = true;
	Series.FastestLapPoints = 2;
	TestEqual(TEXT("any place"), FastestLapRule(Series), FString(TEXT("+2 pts for the fastest lap at any place")));
	Series.FastestLapPoints = 0;
	TestEqual(TEXT("no rule"), FastestLapRule(Series), FString());
	TestEqual(TEXT("nothing raced"), StandingsCaption(All.Championships[0], TEXT("spring-cup")), FString(TEXT("ROUND 1 OF 1 · NOTHING RACED YET")));

	FApexChampionshipStanding Fast;
	Fast.Points = 26;
	Fast.FastestLaps = 1;
	Fast.Results = { 1 };
	Fast.Wins = 1;
	TestEqual(TEXT("a standing's fastest laps"), StandingLine(Fast), FString(TEXT("26 pts · 1 win · R1 P1 · 1 fastest lap")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
