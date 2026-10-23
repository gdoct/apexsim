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

#endif // WITH_DEV_AUTOMATION_TESTS
