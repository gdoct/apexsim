#include "ApexTestCommon.h"
#include "Race/ApexCockpitLayout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The race car actor's mesh mount: long axis on Y in the file, nose on +X once mounted. */
	const FTransform MeshMount(FRotator(0.0f, -90.0f, 0.0f));

	/** A 911-shaped mesh as imported: 4.5 m along Y, 1.9 m across X, 1.3 m tall, on the ground. */
	const FBoxSphereBounds GtMeshBounds(FVector(0.0f, 0.0f, 65.0f), FVector(95.0f, 225.0f, 65.0f), 250.0f);

	/** An F1 car: 5.5 m long, 2 m wide, 95 cm tall. */
	const FBoxSphereBounds F1MeshBounds(FVector(0.0f, 0.0f, 47.5f), FVector(100.0f, 275.0f, 47.5f), 300.0f);
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitActorFrameBoxTest,
	"ApexSim.Cockpit.ActorFrameBox",
	ApexTestFlags)

bool FApexCockpitActorFrameBoxTest::RunTest(const FString& Parameters)
{
	const FBox Box = ApexCockpit::ActorFrameBox(GtMeshBounds, MeshMount);
	const FVector Size = Box.GetSize();

	// The mount's yaw has to carry the long axis onto X, or every point below
	// is derived for a car lying across the track.
	TestEqual(TEXT("length lands on X"), Size.X, 450.0, 0.5);
	TestEqual(TEXT("width lands on Y"), Size.Y, 190.0, 0.5);
	TestEqual(TEXT("height stays on Z"), Size.Z, 130.0, 0.5);
	TestEqual(TEXT("floor stays at zero"), static_cast<float>(Box.Min.Z), 0.0f, 0.5f);

	// A mesh that has not loaded gives an empty box; the seat must still land
	// somewhere sensible rather than at the origin, underground.
	const FBox Fallback = ApexCockpit::ActorFrameBox(FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0f), MeshMount);
	TestTrue(TEXT("empty bounds fall back to a car-sized box"), Fallback.GetSize().X > 300.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitClosedLayoutTest,
	"ApexSim.Cockpit.ClosedLayout",
	ApexTestFlags)

bool FApexCockpitClosedLayoutTest::RunTest(const FString& Parameters)
{
	const FBox Box = ApexCockpit::ActorFrameBox(GtMeshBounds, MeshMount);
	const FApexCockpitLayout Layout = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::Closed, FApexCockpitOverrides());

	TestFalse(TEXT("closed"), Layout.bOpenWheel);
	TestTrue(TEXT("eye inside the body"), Box.IsInside(Layout.Eye));
	TestTrue(TEXT("driver sits on the left"), Layout.Eye.Y < -10.0f);
	TestTrue(TEXT("eye is in the upper half of the cabin"), Layout.Eye.Z > Box.GetCenter().Z);
	TestTrue(TEXT("eye is behind the middle of the car"), Layout.Eye.X < Box.GetCenter().X);

	TestTrue(TEXT("wheel ahead of the eye"), Layout.Wheel.X > Layout.Eye.X + 20.0f);
	TestTrue(TEXT("wheel below the eye"), Layout.Wheel.Z < Layout.Eye.Z - 10.0f);
	TestEqual(TEXT("wheel in front of the driver, not the passenger"), static_cast<float>(Layout.Wheel.Y), static_cast<float>(Layout.Eye.Y), 0.1f);

	TestTrue(TEXT("has a centre mirror"), Layout.bCentreMirror);
	TestTrue(TEXT("centre mirror ahead of and above the eye"),
		Layout.MirrorCentre.X > Layout.Eye.X && Layout.MirrorCentre.Z > Layout.Eye.Z);
	TestTrue(TEXT("door mirrors outside the body"),
		Layout.MirrorLeft.Y < Box.Min.Y && Layout.MirrorRight.Y > Box.Max.Y);
	TestTrue(TEXT("door mirrors ahead of the eye"),
		Layout.MirrorLeft.X > Layout.Eye.X && Layout.MirrorRight.X > Layout.Eye.X);
	TestEqual(TEXT("roof is the top of the box"), Layout.RoofZ, static_cast<float>(Box.Max.Z), 0.1f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitOpenWheelLayoutTest,
	"ApexSim.Cockpit.OpenWheelLayout",
	ApexTestFlags)

bool FApexCockpitOpenWheelLayoutTest::RunTest(const FString& Parameters)
{
	const FBox Box = ApexCockpit::ActorFrameBox(F1MeshBounds, MeshMount);

	// Auto with no class: a 95 cm body is an open cockpit.
	TestEqual(TEXT("height alone resolves open"),
		ApexCockpit::ResolveStyle(EApexCockpitStyle::Auto, FString(), Box), EApexCockpitStyle::OpenWheel);
	TestEqual(TEXT("class F1 resolves open"),
		ApexCockpit::ResolveStyle(EApexCockpitStyle::Auto, TEXT("F1"), FBox(ForceInit)), EApexCockpitStyle::OpenWheel);
	TestEqual(TEXT("class GT3 resolves closed"),
		ApexCockpit::ResolveStyle(EApexCockpitStyle::Auto, TEXT("GT3"), Box), EApexCockpitStyle::Closed);
	TestEqual(TEXT("explicit wins over class"),
		ApexCockpit::ResolveStyle(EApexCockpitStyle::Closed, TEXT("F1"), Box), EApexCockpitStyle::Closed);

	const FApexCockpitLayout Layout = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::OpenWheel, FApexCockpitOverrides());
	TestTrue(TEXT("open"), Layout.bOpenWheel);
	TestEqual(TEXT("eye on the centreline"), static_cast<float>(Layout.Eye.Y), 0.0f, 0.1f);
	TestTrue(TEXT("eye inside the body"), Box.IsInside(Layout.Eye));
	TestFalse(TEXT("no centre mirror"), Layout.bCentreMirror);
	TestTrue(TEXT("mirrors inside the track width, on the shoulders"),
		Layout.MirrorLeft.Y > Box.Min.Y && Layout.MirrorLeft.Y < -20.0f
			&& Layout.MirrorRight.Y < Box.Max.Y && Layout.MirrorRight.Y > 20.0f);
	TestTrue(TEXT("less lock than a road car"), Layout.WheelLockDeg < 100.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitOverridesTest,
	"ApexSim.Cockpit.Overrides",
	ApexTestFlags)

bool FApexCockpitOverridesTest::RunTest(const FString& Parameters)
{
	const FBox Box = ApexCockpit::ActorFrameBox(F1MeshBounds, MeshMount);

	FApexCockpitOverrides Overrides;
	Overrides.Eye = FVector(-80.0f, 0.0f, 70.0f);
	Overrides.MirrorCentre = FVector(10.0f, 0.0f, 90.0f);

	const FApexCockpitLayout Layout = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::OpenWheel, Overrides);
	TestEqual(TEXT("eye override taken"), Layout.Eye, Overrides.Eye);
	// A placed centre mirror exists even on a style that derives none.
	TestTrue(TEXT("placed centre mirror switches it on"), Layout.bCentreMirror);
	TestEqual(TEXT("centre mirror override taken"), Layout.MirrorCentre, Overrides.MirrorCentre);
	// Zero is "derive", not "at the origin".
	TestTrue(TEXT("unset wheel still derived"), Layout.Wheel.X > Layout.Eye.X);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitViewRotationTest,
	"ApexSim.Cockpit.ViewRotation",
	ApexTestFlags)

bool FApexCockpitViewRotationTest::RunTest(const FString& Parameters)
{
	const FRotator Banked(4.0f, 90.0f, -12.0f);

	// Lock 0 rides the car exactly.
	const FRotator Ride = ApexCockpit::ViewRotation(Banked, 0.0f, 0.0f, 0.0f).Rotator();
	TestEqual(TEXT("rides roll"), Ride.Roll, -12.0, 0.05);
	TestEqual(TEXT("rides pitch"), Ride.Pitch, 4.0, 0.05);
	TestEqual(TEXT("rides yaw"), Ride.Yaw, 90.0, 0.05);

	// Lock 1 levels the horizon but never the heading.
	const FRotator Level = ApexCockpit::ViewRotation(Banked, 1.0f, 0.0f, 0.0f).Rotator();
	TestEqual(TEXT("levels roll"), Level.Roll, 0.0, 0.05);
	TestEqual(TEXT("levels pitch"), Level.Pitch, 0.0, 0.05);
	TestEqual(TEXT("keeps yaw"), Level.Yaw, 90.0, 0.05);

	// Half lock is half the tilt.
	const FRotator Half = ApexCockpit::ViewRotation(Banked, 0.5f, 0.0f, 0.0f).Rotator();
	TestEqual(TEXT("half roll"), Half.Roll, -6.0, 0.1);

	// Gaze and head turn on a level car add straight onto the heading.
	const FRotator Turned = ApexCockpit::ViewRotation(FRotator(0.0f, 90.0f, 0.0f), 1.0f, -5.0f, 180.0f).Rotator();
	TestEqual(TEXT("look back adds 180"), FMath::Abs(FRotator::NormalizeAxis(Turned.Yaw - 270.0)), 0.0, 0.05);
	TestEqual(TEXT("gaze pitch applied"), Turned.Pitch, -5.0, 0.05);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitHeadAndWheelTest,
	"ApexSim.Cockpit.HeadAndWheel",
	ApexTestFlags)

bool FApexCockpitHeadAndWheelTest::RunTest(const FString& Parameters)
{
	// Braking at 1 g: forward. A right-hander at 1 g: left (-Y).
	const FVector Braking = ApexCockpit::HeadLean(0.0f, -1.0f, 1.0f);
	TestTrue(TEXT("braking throws the head forward"), Braking.X > 1.0f && FMath::IsNearlyZero(Braking.Y));
	const FVector RightTurn = ApexCockpit::HeadLean(1.0f, 0.0f, 1.0f);
	TestTrue(TEXT("right turn throws the head left"), RightTurn.Y < -1.0f);

	TestTrue(TEXT("setting at zero is still"), ApexCockpit::HeadLean(1.0f, -1.0f, 0.0f).IsNearlyZero());
	TestTrue(TEXT("half the setting is half the lean"),
		FMath::IsNearlyEqual(ApexCockpit::HeadLean(1.0f, 0.0f, 0.5f).Y, RightTurn.Y * 0.5f, 0.01f));
	// A crash is not a reason to leave the seat.
	TestTrue(TEXT("clamped"), ApexCockpit::HeadLean(20.0f, -20.0f, 1.0f).Size() < 10.0f);

	// Server steering is positive to the left; the rim turns counter-clockwise
	// as the driver sees it, which is negative Unreal roll.
	TestTrue(TEXT("left steering rolls the rim negative"), ApexCockpit::WheelRollDeg(0.5f, 120.0f) < 0.0f);
	TestEqual(TEXT("full lock"), ApexCockpit::WheelRollDeg(-1.0f, 120.0f), 120.0f, 0.01f);
	TestEqual(TEXT("over-range input clamps"), ApexCockpit::WheelRollDeg(3.0f, 90.0f), -90.0f, 0.01f);

	TestTrue(TEXT("left steering looks left"), ApexCockpit::ApexLookYawDeg(1.0f, 1.0f) < 0.0f);
	TestTrue(TEXT("apex look is a glance, not a head turn"), FMath::Abs(ApexCockpit::ApexLookYawDeg(1.0f, 1.0f)) < 30.0f);
	TestTrue(TEXT("apex look off"), FMath::IsNearlyZero(ApexCockpit::ApexLookYawDeg(1.0f, 0.0f)));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
