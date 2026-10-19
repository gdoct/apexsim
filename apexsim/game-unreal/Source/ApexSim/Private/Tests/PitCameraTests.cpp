#include "ApexTestCommon.h"
#include "Race/ApexPitCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexPitCameraPoseTest,
	"ApexSim.Camera.PitBox",
	ApexTestFlags)

bool FApexPitCameraPoseTest::RunTest(const FString& Parameters)
{
	// A lane down the server's +X, 12 m wide, the box's spot 2.5 m left of
	// its middle: the garages are on the left, the pit wall on the right.
	const FVector Spot(100.0, 50.0, 3.0);
	const FVector Camera = ApexPitCamera::Position(Spot, 0.0f, 2.5f, 12.0f);
	TestTrue(TEXT("ahead of the car"), FMath::IsNearlyEqual(Camera.X, 100.0 + ApexPitCamera::AheadM, 1e-3));
	// The lane's middle is at y 47.5; its right edge 6 m further, the camera a metre inside it.
	TestTrue(TEXT("at the pit wall's edge"), FMath::IsNearlyEqual(Camera.Y, 47.5 - 5.0, 1e-3));
	TestEqual(TEXT("the spot's height"), Camera.Z, 3.0);

	// The garages on the right, the lane pointing down +Y: the camera goes left of it, at -X.
	const FVector Turned = ApexPitCamera::Position(FVector::ZeroVector, UE_HALF_PI, -2.0f, 10.0f);
	TestTrue(TEXT("down the lane"), FMath::IsNearlyEqual(Turned.Y, ApexPitCamera::AheadM, 1e-3));
	TestTrue(TEXT("across, away from the garages"), FMath::IsNearlyEqual(Turned.X, -(2.0 + 4.0), 1e-3));

	// The pose starts on the spot, all but stopped, and holds until the car pulls away.
	TestFalse(TEXT("rolling in"), ApexPitCamera::Holds(false, false, 1.0f, 2.0f));
	TestTrue(TEXT("stopped on it"), ApexPitCamera::Holds(false, false, 1.0f, 0.5f));
	TestTrue(TEXT("the crew at work"), ApexPitCamera::Holds(false, true, 1.0f, 0.0f));
	TestTrue(TEXT("pulling away, still slow"), ApexPitCamera::Holds(true, false, 2.0f, 2.5f));
	TestFalse(TEXT("away"), ApexPitCamera::Holds(true, false, 2.0f, 3.5f));
	TestFalse(TEXT("off the spot"), ApexPitCamera::Holds(true, false, 6.0f, 0.0f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
