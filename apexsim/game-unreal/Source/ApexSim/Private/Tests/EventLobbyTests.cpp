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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
