#include "ApexTestCommon.h"
#include "Race/ApexChaseView.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexChaseLadderTest,
	"ApexSim.Camera.ChaseLadder",
	ApexTestFlags)

bool FApexChaseLadderTest::RunTest(const FString& Parameters)
{
	const TArray<ApexChase::FView>& Views = ApexChase::Views();
	TestTrue(TEXT("the ladder has rungs between the cockpit and the old chase camera"), Views.Num() >= 4);

	// Closest first, and every rung further back, lower over the car and a
	// little narrower than the one before: that ordering is what C steps
	// along, and what the settings row's ROOF..FAR labels name.
	for (int32 Index = 1; Index < Views.Num(); ++Index)
	{
		TestTrue(TEXT("arm grows"), Views[Index].ArmLengthCm > Views[Index - 1].ArmLengthCm);
		TestTrue(TEXT("lift falls"), Views[Index].HeightCm <= Views[Index - 1].HeightCm);
		TestTrue(TEXT("looks further down"), Views[Index].PitchDeg <= Views[Index - 1].PitchDeg);
		TestTrue(TEXT("lens narrows toward the base fov"),
			Views[Index].FovDeltaDeg <= Views[Index - 1].FovDeltaDeg);
		// A close camera that lagged would swing the car about the frame.
		TestTrue(TEXT("lag loosens"), Views[Index].LagSpeed <= Views[Index - 1].LagSpeed);
	}

	// Every rung but the last clamps how far the lag may stretch it, or a
	// close camera ends up at the far camera's distance on a straight: the
	// spring arm's steady-state lag is speed / LagSpeed, nine metres at
	// 320 km/h.
	for (int32 Index = 0; Index < Views.Num() - 1; ++Index)
	{
		TestTrue(TEXT("a close rung clamps its lag"), Views[Index].LagMaxDistanceCm > 0.0f);
		TestTrue(TEXT("the clamp is a fraction of the arm"),
			Views[Index].LagMaxDistanceCm < Views[Index].ArmLengthCm);
	}

	// The far rung is the chase camera exactly as it was before the ladder,
	// so an existing profile and -ApexView=chase are unchanged.
	const ApexChase::FView& Far = ApexChase::Get(ApexChase::DefaultLevel());
	TestEqual(TEXT("far arm"), Far.ArmLengthCm, 900.0f);
	TestEqual(TEXT("far pitch"), Far.PitchDeg, -12.0f);
	TestEqual(TEXT("far lift"), Far.HeightCm, 0.0f);
	TestEqual(TEXT("far fov is the base"), Far.FovDeltaDeg, 0.0f);
	TestEqual(TEXT("far lag is unclamped"), Far.LagMaxDistanceCm, 0.0f);

	// An index from anywhere lands on the ladder rather than off the end.
	TestEqual(TEXT("below clamps to closest"), ApexChase::Get(-5).ArmLengthCm, Views[0].ArmLengthCm);
	TestEqual(TEXT("above clamps to farthest"), ApexChase::Get(99).ArmLengthCm, Far.ArmLengthCm);

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexChaseCycleTest,
	"ApexSim.Camera.ChaseCycle",
	ApexTestFlags)

bool FApexChaseCycleTest::RunTest(const FString& Parameters)
{
	// C: cockpit, each rung from the closest out, then back to the cockpit.
	int32 Level = ApexChase::CockpitLevel;
	for (int32 Expected = 0; Expected < ApexChase::Num(); ++Expected)
	{
		Level = ApexChase::NextLevel(Level);
		TestEqual(TEXT("steps out one rung"), Level, Expected);
	}
	TestEqual(TEXT("past the last rung is the cockpit again"),
		ApexChase::NextLevel(Level), ApexChase::CockpitLevel);

	// A saved index past the end (a ladder that has since shrunk) returns to
	// the cockpit rather than sticking on a rung that no longer exists.
	TestEqual(TEXT("an off-the-end index falls back to the cockpit"),
		ApexChase::NextLevel(99), ApexChase::CockpitLevel);

	int32 Named = -99;
	TestTrue(TEXT("far is named"), ApexChase::FindByName(TEXT("far"), Named));
	TestEqual(TEXT("far is the last rung"), Named, ApexChase::DefaultLevel());
	TestTrue(TEXT("roof is named"), ApexChase::FindByName(TEXT("ROOF"), Named));
	TestEqual(TEXT("roof is the closest rung"), Named, 0);
	TestTrue(TEXT("cockpit is named"), ApexChase::FindByName(TEXT("cockpit"), Named));
	TestEqual(TEXT("cockpit is not a rung"), Named, ApexChase::CockpitLevel);
	TestFalse(TEXT("anything else is not a view"), ApexChase::FindByName(TEXT("chase"), Named));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
