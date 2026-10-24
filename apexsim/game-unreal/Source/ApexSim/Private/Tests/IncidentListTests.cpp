#include "ApexTestCommon.h"
#include "UI/ApexIncidentList.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexIncidentListWordsTest,
	"ApexSim.UI.IncidentList.Words",
	ApexTestFlags)

bool FApexIncidentListWordsTest::RunTest(const FString& Parameters)
{
	FApexIncident Contact;
	Contact.Kind = ApexIncident::Contact;
	Contact.Name = TEXT("Ada");
	Contact.OtherName = TEXT("Bea");
	Contact.Severity = ApexIncident::Heavy;
	Contact.ClosingMps = 9.5f;
	TestEqual(TEXT("contact"), ApexIncidentList::What(Contact), FString(TEXT("Contact")));
	TestEqual(TEXT("contact cars"), ApexIncidentList::Cars(Contact), FString(TEXT("Ada and Bea")));
	TestEqual(TEXT("contact detail"), ApexIncidentList::Detail(Contact), FString(TEXT("heavy · 9.5 m/s")));

	FApexIncident Yellow = Contact;
	Yellow.Kind = ApexIncident::YellowPass;
	TestEqual(TEXT("yellow cars"), ApexIncidentList::Cars(Yellow), FString(TEXT("Ada passed Bea")));

	FApexIncident Cut;
	Cut.Kind = ApexIncident::TrackLimits;
	Cut.Name = TEXT("Cy");
	Cut.bCut = true;
	TestEqual(TEXT("cut cars"), ApexIncidentList::Cars(Cut), FString(TEXT("Cy")));
	TestEqual(TEXT("cut detail"), ApexIncidentList::Detail(Cut), FString(TEXT("cut the course")));

	FApexIncident Penalty;
	Penalty.Kind = ApexIncident::Penalty;
	Penalty.Offence = ApexPenalty::JumpStart;
	Penalty.Penalty = ApexPenalty::Time;
	Penalty.Seconds = 5;
	TestEqual(TEXT("penalty detail"), ApexIncidentList::Detail(Penalty), FString(TEXT("5 s added · jump start")));
	Penalty.Kind = ApexIncident::PenaltyCleared;
	Penalty.Penalty = ApexPenalty::DriveThrough;
	TestEqual(TEXT("cleared detail"), ApexIncidentList::Detail(Penalty), FString(TEXT("drive-through · jump start, taken back")));

	TestEqual(TEXT("race clock"), ApexIncidentList::RaceClock(724.5), FString(TEXT("12:04.5")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexIncidentListReplayTest,
	"ApexSim.UI.IncidentList.ReplaySeconds",
	ApexTestFlags)

bool FApexIncidentListReplayTest::RunTest(const FString& Parameters)
{
	// A replay begun at tick 840 at 420 Hz: tick 5040 is 10 s into it, and
	// the jump starts 3 s before.
	double Seconds = -1.0;
	TestTrue(TEXT("in the replay"), ApexIncidentList::ReplaySecondsOf(5040, 840, 420, 0, Seconds));
	TestEqual(TEXT("its second"), Seconds, 10.0);
	TestEqual(TEXT("the jump"), ApexIncidentList::JumpSeconds(Seconds), 7.0);
	TestEqual(TEXT("the jump at the start"), ApexIncidentList::JumpSeconds(1.0), 0.0);
	// Garage time cut from the recording comes off the server's ticks.
	TestTrue(TEXT("after a cut"), ApexIncidentList::ReplaySecondsOf(5040 + 420, 840, 420, 420, Seconds));
	TestEqual(TEXT("its second after a cut"), Seconds, 10.0);
	// Before the replay began, or no rate: none.
	TestFalse(TEXT("before the replay"), ApexIncidentList::ReplaySecondsOf(420, 840, 420, 0, Seconds));
	TestFalse(TEXT("no rate"), ApexIncidentList::ReplaySecondsOf(5040, 840, 0, 0, Seconds));

	// The list's seconds of the race count from the green.
	FApexIncidents List;
	List.TickRate = 420;
	List.RaceStartTick = 2520;
	FApexIncident Incident;
	Incident.Tick = 5040;
	TestEqual(TEXT("second of the race"), List.RaceSeconds(Incident), 6.0);
	List.RaceStartTick = -1;
	TestEqual(TEXT("before the green"), List.RaceSeconds(Incident), 0.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
