#include "ApexTestCommon.h"
#include "Race/ApexCarDrsFlap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexDrsTest
{
	/** The Fugazzi SF-26's figures: hinge at the flap's trailing edge, over the rear axle's tail. */
	FApexDrsFlapSpec Spec()
	{
		FApexDrsFlapSpec S;
		S.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Cars/fugazzi_sf26/Drs/SM_fugazzi_sf26_drs.SM_fugazzi_sf26_drs")));
		S.HingeForwardM = -2.52f;
		S.HingeUpM = 0.82f;
		S.OpenDeg = 24.0f;
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexDrsFlapTransformTest, "ApexSim.Drs.FlapTransform", ApexTestFlags)

bool FApexDrsFlapTransformTest::RunTest(const FString& Parameters)
{
	using namespace ApexDrsTest;
	const FApexDrsFlapSpec S = Spec();

	// Shut: the mesh sits where it was cut from, its origin on the hinge.
	const FTransform Shut = ApexDrs::FlapTransform(S, 0.0f);
	TestEqual(TEXT("hinge behind the body origin"), Shut.GetLocation().Y, -252.0, 0.01);
	TestEqual(TEXT("hinge height"), Shut.GetLocation().Z, 82.0, 0.01);
	const FVector LeadingEdge(0.0f, 14.0f, 3.0f);          // 14 cm ahead of the hinge, a little up
	TestEqual(TEXT("shut flap is not turned"), Shut.TransformPosition(LeadingEdge), Shut.GetLocation() + LeadingEdge, 0.01);

	// Open: the leading edge lifts, the hinge stays, nothing moves sideways.
	const FTransform Opened = ApexDrs::FlapTransform(S, 1.0f);
	const FVector Lifted = Opened.TransformPosition(LeadingEdge);
	TestTrue(TEXT("leading edge rises when open"), Lifted.Z > Shut.TransformPosition(LeadingEdge).Z + 4.0);
	TestEqual(TEXT("hinge does not move"), Opened.TransformPosition(FVector::ZeroVector), Shut.GetLocation(), 0.01);
	TestEqual(TEXT("no sideways swing"), Lifted.X, 0.0, 0.01);
	TestEqual(TEXT("leading edge keeps its distance from the hinge"), (Lifted - Opened.GetLocation()).Size(), LeadingEdge.Size(), 0.01);
	TestEqual(TEXT("open turns OpenDeg"), FMath::RadiansToDegrees(Opened.GetRotation().GetAngle()), 24.0, 0.01);
	TestEqual(TEXT("over-open is clamped"), FMath::RadiansToDegrees(ApexDrs::FlapTransform(S, 3.0f).GetRotation().GetAngle()), 24.0, 0.01);

	// The swing: a few frames to open, and back.
	float Open = 0.0f;
	Open = ApexDrs::StepOpen(Open, true, 1.0f / 60.0f);
	TestTrue(TEXT("one frame opens it part way"), Open > 0.0f && Open < 1.0f);
	Open = ApexDrs::StepOpen(Open, true, 1.0f);
	TestEqual(TEXT("fully open"), Open, 1.0f);
	Open = ApexDrs::StepOpen(Open, false, ApexDrs::SwingSeconds);
	TestEqual(TEXT("shut again in one swing"), Open, 0.0f, 1e-5f);
	return true;
}

#endif
