#include "ApexTestCommon.h"
#include "ApexPitPlanSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// The driver's pit plan as the pause menu's panel edits it, and the race's
// pit rule in words.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPitPlanChoicesTest, "ApexSim.Race.PitPlan.Choices", ApexTestFlags)

bool FApexPitPlanChoicesTest::RunTest(const FString& Parameters)
{
	using namespace ApexPitPlan;
	const TArray<FString> Defaults;
	const TArray<FString> Own = { TEXT("dry"), TEXT("wet") };

	// The crew's call is every row's first stop but fuel's.
	FApexPitPlan Plan;
	TestTrue(TEXT("a new plan is the crew's call"), IsCrewsCall(Plan));
	TestEqual(TEXT("setup's tyres"), TyreChoice(Plan), 1);
	TestEqual(TEXT("fuel to need"), FuelChoice(Plan), -1);
	TestEqual(TEXT("pads crew's call"), PadsChoice(Plan), 0);
	TestEqual(TEXT("setup's pad set"), PadSetChoice(Plan), 0);
	TestEqual(TEXT("summary"), Summary(Plan, Defaults), FString(TEXT("CREW'S CALL")));

	// Tyres: keep, the setup's, then each compound.
	TestEqual(TEXT("five defaults and two"), TyreChoiceCount(Defaults), 7);
	TestEqual(TEXT("a car's own two and two"), TyreChoiceCount(Own), 4);
	SetTyreChoice(Plan, 0);
	TestTrue(TEXT("keep"), Plan.bKeepTyres);
	TestEqual(TEXT("keep reads back"), TyreChoice(Plan), 0);
	SetTyreChoice(Plan, 3);
	TestFalse(TEXT("a compound fits a set"), Plan.bKeepTyres);
	TestEqual(TEXT("the car's second compound"), Plan.Compound, 1);
	TestEqual(TEXT("named from the car's list"), TyreChoiceLabel(3, Own), FString(TEXT("WET")));
	TestEqual(TEXT("named from the defaults"), TyreChoiceLabel(2, Defaults), FString(TEXT("SOFT")));
	TestEqual(TEXT("keep label"), TyreChoiceLabel(0, Own), FString(TEXT("KEEP")));
	SetTyreChoice(Plan, 1);
	TestEqual(TEXT("back to the setup's"), Plan.Compound, -1);

	// Fuel: to need, none, then 5 L steps.
	SetFuelChoice(Plan, 1);
	TestEqual(TEXT("5 L"), Plan.FuelL, 5.0f);
	TestEqual(TEXT("5 L label"), FuelChoiceLabel(1), FString(TEXT("+5 L")));
	SetFuelChoice(Plan, 0);
	TestEqual(TEXT("none"), Plan.FuelL, 0.0f);
	TestEqual(TEXT("none reads back"), FuelChoice(Plan), 0);
	SetFuelChoice(Plan, -1);
	TestTrue(TEXT("to need is negative"), Plan.FuelL < 0.0f);

	// Pads: crew's call, keep, new; and the set.
	SetPadsChoice(Plan, 1);
	TestEqual(TEXT("keep pads"), Plan.Pads, 0);
	SetPadsChoice(Plan, 2);
	TestEqual(TEXT("new pads"), Plan.Pads, 1);
	SetPadSetChoice(Plan, 1);
	TestTrue(TEXT("a chosen set"), Plan.bChoosePadSet);
	TestEqual(TEXT("endurance"), Plan.PadSet, -1);
	SetPadSetChoice(Plan, 3);
	TestEqual(TEXT("sprint"), Plan.PadSet, 1);
	SetPadSetChoice(Plan, 0);
	TestFalse(TEXT("the setup's set"), Plan.bChoosePadSet);

	// The check's plan: tyres kept and 5 L.
	FApexPitPlan Splash;
	SetTyreChoice(Splash, 0);
	SetFuelChoice(Splash, 1);
	TestFalse(TEXT("not the crew's call"), IsCrewsCall(Splash));
	TestEqual(TEXT("splash summary"), Summary(Splash, Defaults), FString(TEXT("KEEP TYRES · +5 L")));
	Splash.bBoxThisLap = true;
	TestEqual(TEXT("the request leads"), Summary(Splash, Defaults), FString(TEXT("BOX THIS LAP · KEEP TYRES · +5 L")));
	FApexPitPlan Boxed;
	Boxed.bBoxThisLap = true;
	TestTrue(TEXT("the request alone is still the crew's call"), IsCrewsCall(Boxed));

	// The driver row: the crew's call, stay in, change.
	FApexPitPlan Driver;
	TestEqual(TEXT("the crew's call on the driver"), DriverChoice(Driver), 0);
	SetDriverChoice(Driver, 2);
	TestEqual(TEXT("change"), Driver.ChangeDriver, 1);
	TestFalse(TEXT("an order on the driver"), IsCrewsCall(Driver));
	TestEqual(TEXT("change summary"), Summary(Driver, Defaults), FString(TEXT("DRIVER CHANGE")));
	SetDriverChoice(Driver, 1);
	TestEqual(TEXT("stay in"), Driver.ChangeDriver, 0);
	TestEqual(TEXT("stay label"), DriverChoiceLabel(1), FString(TEXT("STAY IN")));
	SetDriverChoice(Driver, 0);
	TestEqual(TEXT("back to the crew"), Driver.ChangeDriver, -1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPitPlanPadSetTest, "ApexSim.Race.PitPlan.PadSetFollows", ApexTestFlags)

bool FApexPitPlanPadSetTest::RunTest(const FString& Parameters)
{
	using namespace ApexPitPlan;
	FApexPitPlan Plan;
	TestEqual(TEXT("no chosen set: the setup keeps its own"), PadSetFitted(Plan, 80.0f, 0.0f), INDEX_NONE);

	SetPadSetChoice(Plan, 3);
	SetPadsChoice(Plan, 2);
	TestEqual(TEXT("new sprint pads"), PadSetFitted(Plan, 10.0f, 10.0f), 1);
	TestEqual(TEXT("new pads even with the wear unknown"), PadSetFitted(Plan, -1.0f, -1.0f), 1);

	SetPadsChoice(Plan, 1);
	TestEqual(TEXT("pads kept: no change"), PadSetFitted(Plan, 80.0f, 0.0f), INDEX_NONE);

	SetPadsChoice(Plan, 0);
	SetPadSetChoice(Plan, 1);
	TestEqual(TEXT("crew's call, worn pads replaced"), PadSetFitted(Plan, 70.0f, 0.0f), -1);
	TestEqual(TEXT("crew's call, pads left on"), PadSetFitted(Plan, 30.0f, 30.0f), INDEX_NONE);
	TestEqual(TEXT("crew's call, wear unknown"), PadSetFitted(Plan, -1.0f, 0.0f), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPitRuleWordsTest, "ApexSim.Race.PitRule.Words", ApexTestFlags)

bool FApexPitRuleWordsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("no rule"), ApexMandatoryStops::Describe(0, false), FString());
	TestEqual(TEXT("one stop"), ApexMandatoryStops::Describe(1, false), FString(TEXT("1 stop")));
	TestEqual(TEXT("two and compounds"), ApexMandatoryStops::Describe(2, true), FString(TEXT("2 stops · two compounds")));
	TestEqual(TEXT("compounds alone"), ApexMandatoryStops::Describe(0, true), FString(TEXT("two compounds")));
	TestEqual(TEXT("held to three"), ApexMandatoryStops::Describe(9, false), FString(TEXT("3 stops")));
	TestEqual(TEXT("owed"), ApexMandatoryStops::Owed(2, 1), 1);
	TestEqual(TEXT("met"), ApexMandatoryStops::Owed(1, 3), 0);
	TestEqual(TEXT("no rule owes nothing"), ApexMandatoryStops::Owed(0, 0), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
