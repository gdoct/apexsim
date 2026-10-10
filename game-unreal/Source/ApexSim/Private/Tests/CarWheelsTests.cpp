#include "ApexTestCommon.h"
#include "Race/ApexCarWheels.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexWheelsTest
{
	using ApexWheels::EWheel;

	/** An F1 car's wheel figures, as a car.toml gives them. */
	FApexWheelSpec F1Spec()
	{
		FApexWheelSpec Spec;
		Spec.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Cars/Wheels/f1/SM_Wheel_f1.SM_Wheel_f1")));
		Spec.FrontAxleM = 1.56f;
		Spec.RearAxleM = -1.985f;
		Spec.FrontTrackM = 1.627f;
		Spec.RearTrackM = 1.501f;
		Spec.FrontRadiusM = 0.328f;
		Spec.RearRadiusM = 0.316f;
		Spec.FrontWidthM = 0.333f;
		Spec.RearWidthM = 0.41f;
		Spec.MaxSteerRad = 0.35f;
		return Spec;
	}

	/** The f1 wheel as imported: 38 cm across the axle (X), 72 cm tall, centred on the hub. */
	const FBoxSphereBounds WheelBounds(FVector::ZeroVector, FVector(19.0f, 36.0f, 36.0f), 55.0f);

	/** Where a point on the wheel model ends up in the body mesh's frame. */
	FVector Place(const FApexWheelSpec& Spec, EWheel Wheel, float Steer, float Spin, const FVector& OnWheel)
	{
		return ApexWheels::WheelTransform(Spec, Wheel, WheelBounds, Steer, Spin).TransformPosition(OnWheel);
	}

	/** Body mesh frame, as imported: nose +Y, left +X, floor at 0. */
	const FVector Ahead(0.0f, 1.0f, 0.0f);
	const FVector Left(1.0f, 0.0f, 0.0f);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexWheelsPlacementTest, "ApexSim.Wheels.Placement", ApexTestFlags)

bool FApexWheelsPlacementTest::RunTest(const FString& Parameters)
{
	using namespace ApexWheelsTest;

	const FApexWheelSpec Spec = F1Spec();

	const FVector FL = Place(Spec, EWheel::FrontLeft, 0.0f, 0.0f, FVector::ZeroVector);
	TestEqual(TEXT("front left hub is ahead"), FL.Y, 156.0, 0.01);
	TestEqual(TEXT("front left hub is on the left (+X)"), FL.X, 81.35, 0.01);
	TestEqual(TEXT("hub is one radius up"), FL.Z, 32.8, 0.01);
	const FVector RR = Place(Spec, EWheel::RearRight, 0.0f, 0.0f, FVector::ZeroVector);
	TestEqual(TEXT("rear right hub is behind"), RR.Y, -198.5, 0.01);
	TestEqual(TEXT("rear right hub is on the right (-X)"), RR.X, -75.05, 0.01);

	// The model's face (+X) is outboard on both sides.
	TestTrue(TEXT("left face points left"), (Place(Spec, EWheel::FrontLeft, 0, 0, FVector(19.0, 0.0, 0.0)) - FL).X > 0.0);
	const FVector FR = Place(Spec, EWheel::FrontRight, 0.0f, 0.0f, FVector::ZeroVector);
	TestTrue(TEXT("right face points right"), (Place(Spec, EWheel::FrontRight, 0, 0, FVector(19.0, 0.0, 0.0)) - FR).X < 0.0);

	// Sized from the bounds: the tread sits on the floor, the width matches.
	TestEqual(TEXT("bottom of the front tyre on the floor"), Place(Spec, EWheel::FrontLeft, 0, 0, FVector(0.0, 0.0, -36.0)).Z, 0.0, 0.01);
	TestEqual(TEXT("bottom of the rear tyre on the floor"), Place(Spec, EWheel::RearLeft, 0, 0, FVector(0.0, 0.0, -36.0)).Z, 0.0, 0.01);
	TestEqual(TEXT("rear width"), (Place(Spec, EWheel::RearLeft, 0, 0, FVector(19.0, 0.0, 0.0)) - Place(Spec, EWheel::RearLeft, 0, 0, FVector(-19.0, 0.0, 0.0))).Size(), 41.0, 0.01);

	// Mirror images across the centreline, not inside out.
	const FTransform Right = ApexWheels::WheelTransform(Spec, EWheel::FrontRight, WheelBounds, 0.0f, 0.0f);
	TestTrue(TEXT("right wheel is a turn, not a mirror"), Right.GetDeterminant() > 0.0f);

	const FBox Box = ApexWheels::WheelsBox(Spec);
	TestEqual(TEXT("wheels box reaches the front tyre's leading edge"), Box.Max.Y, 156.0 + 32.8, 0.01);
	TestEqual(TEXT("wheels box floor"), Box.Min.Z, 0.0, 0.01);
	TestFalse(TEXT("no box for an empty spec"), ApexWheels::WheelsBox(FApexWheelSpec()).IsValid != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexWheelsSteerTest, "ApexSim.Wheels.Steer", ApexTestFlags)

bool FApexWheelsSteerTest::RunTest(const FString& Parameters)
{
	using namespace ApexWheelsTest;

	const FApexWheelSpec Spec = F1Spec();

	TestEqual(TEXT("full left lock"), ApexWheels::SteerAngleRad(Spec, 1.0f), 0.35f);
	TestEqual(TEXT("input is clamped"), ApexWheels::SteerAngleRad(Spec, -3.0f), -0.35f);

	// Steering left (positive, as the server holds it) swings the front of
	// the wheel toward the car's left. "Front of the wheel" is the rim's
	// leading point in the model: on the left wheel that is model +Y
	// (unturned); on the right, turned half round, model -Y.
	const float Lock = ApexWheels::SteerAngleRad(Spec, 1.0f);
	for (const EWheel Wheel : {EWheel::FrontLeft, EWheel::FrontRight})
	{
		const bool bLeft = ApexWheels::IsLeft(Wheel);
		const FVector Lead(0.0f, bLeft ? 36.0f : -36.0f, 0.0f);
		const FVector Hub = Place(Spec, Wheel, 0.0f, 0.0f, FVector::ZeroVector);
		const FVector Straight = (Place(Spec, Wheel, 0.0f, 0.0f, Lead) - Hub).GetSafeNormal();
		const FVector Turned = (Place(Spec, Wheel, Lock, 0.0f, Lead) - Hub).GetSafeNormal();
		TestEqual(TEXT("unsteered wheel points ahead"), FVector::DotProduct(Straight, Ahead), 1.0, 0.001);
		TestTrue(TEXT("left steer points the wheel left"), FVector::DotProduct(Turned, Left) > 0.3);
		TestEqual(TEXT("by the lock angle"), FMath::Acos(FVector::DotProduct(Turned, Ahead)), static_cast<double>(Lock), 0.001);
	}

	// The rear wheels never steer.
	const FVector RearHub = Place(Spec, EWheel::RearLeft, 0.0f, 0.0f, FVector::ZeroVector);
	const FVector RearLead = Place(Spec, EWheel::RearLeft, Lock, 0.0f, FVector(0.0, 36.0, 0.0)) - RearHub;
	TestEqual(TEXT("rear wheel ignores the steering"), FVector::DotProduct(RearLead.GetSafeNormal(), Ahead), 1.0, 0.001);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexWheelsRollTest, "ApexSim.Wheels.Roll", ApexTestFlags)

bool FApexWheelsRollTest::RunTest(const FString& Parameters)
{
	using namespace ApexWheelsTest;

	const FApexWheelSpec Spec = F1Spec();

	TestEqual(TEXT("one circumference is one turn"), ApexWheels::RollAngleRad(2.0f * UE_PI * 0.328f, 0.328f), 2.0f * UE_PI, 0.0001f);
	TestEqual(TEXT("no radius, no roll"), ApexWheels::RollAngleRad(1.0f, 0.0f), 0.0f);
	TestEqual(TEXT("slow steps roll true"), ApexWheels::DrawnSpinStepRad(0.1f, 0.2f), 0.1f);
	TestEqual(TEXT("fast steps are held"), ApexWheels::DrawnSpinStepRad(1.5f, 0.2f), 0.2f);
	TestEqual(TEXT("held backwards too"), ApexWheels::DrawnSpinStepRad(-1.5f, 0.2f), -0.2f);
	TestEqual(TEXT("no limit"), ApexWheels::DrawnSpinStepRad(1.5f, 0.0f), 1.5f);

	// Distance rolled: along the car's nose only (actor frame, cm).
	const FQuat Heading = FRotator(0.0f, 90.0f, 0.0f).Quaternion();	// nose on +Y
	const FVector From(1000.0, 2000.0, 50.0);
	TestEqual(TEXT("standing still rolls nothing"), ApexWheels::RolledDistanceM(From, From, Heading, 2000.0f), 0.0f);
	TestEqual(TEXT("bobbing on the springs rolls nothing"),
		ApexWheels::RolledDistanceM(From, From + FVector(0.0, 0.0, 3.0), Heading, 2000.0f), 0.0f);
	TestEqual(TEXT("sliding sideways rolls nothing"),
		ApexWheels::RolledDistanceM(From, From + FVector(40.0, 0.0, 0.0), Heading, 2000.0f), 0.0f, 0.0001f);
	TestEqual(TEXT("driving forward"), ApexWheels::RolledDistanceM(From, From + FVector(0.0, 150.0, 0.0), Heading, 2000.0f), 1.5f, 0.0001f);
	TestEqual(TEXT("backing up is negative"),
		ApexWheels::RolledDistanceM(From, From - FVector(0.0, 50.0, 0.0), Heading, 2000.0f), -0.5f, 0.0001f);
	TestEqual(TEXT("a teleport rolls nothing"),
		ApexWheels::RolledDistanceM(From, From + FVector(0.0, 5000.0, 0.0), Heading, 2000.0f), 0.0f);

	// Rolling forward, the top of every wheel moves ahead and the bottom
	// back: that is what a wheel on the road does.
	for (int32 i = 0; i < ApexWheels::NumWheels; ++i)
	{
		const EWheel Wheel = static_cast<EWheel>(i);
		const float Radius = ApexWheels::RadiusM(Spec, Wheel);
		const float Step = ApexWheels::RollAngleRad(0.01f, Radius);
		const FVector Top(0.0f, 0.0f, 36.0f);
		const FVector Bottom(0.0f, 0.0f, -36.0f);
		const double TopMoves = FVector::DotProduct(
			Place(Spec, Wheel, 0.0f, Step, Top) - Place(Spec, Wheel, 0.0f, 0.0f, Top), Ahead);
		const double BottomMoves = FVector::DotProduct(
			Place(Spec, Wheel, 0.0f, Step, Bottom) - Place(Spec, Wheel, 0.0f, 0.0f, Bottom), Ahead);
		TestTrue(FString::Printf(TEXT("wheel %d: top rolls forward"), i), TopMoves > 0.0);
		TestTrue(FString::Printf(TEXT("wheel %d: bottom rolls back"), i), BottomMoves < 0.0);
		// 1 cm of travel moves the top 1 cm relative to the hub.
		TestEqual(FString::Printf(TEXT("wheel %d: rim speed matches the road"), i), TopMoves, 1.0, 0.01);
	}

	// A steered, spinning front wheel keeps its hub and stays upright.
	const FVector Hub = Place(Spec, EWheel::FrontLeft, 0.3f, 1.7f, FVector::ZeroVector);
	TestEqual(TEXT("spin and steer leave the hub"), Hub, Place(Spec, EWheel::FrontLeft, 0.0f, 0.0f, FVector::ZeroVector), 0.01f);
	const FVector Axle = Place(Spec, EWheel::FrontLeft, 0.3f, 1.7f, FVector(19.0, 0.0, 0.0)) - Hub;
	TestEqual(TEXT("axle stays level"), Axle.Z, 0.0, 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexWheelsTreadedLookTest, "ApexSim.Wheels.TreadedLook", ApexTestFlags)

bool FApexWheelsTreadedLookTest::RunTest(const FString& Parameters)
{
	// A slick keeps the model's own tyre; the two rain tyres are tinted,
	// matte, and the wet darker than the intermediate, so the three can be
	// told apart from the pit wall.
	FLinearColor Colour;
	float Roughness = 0.0f;
	TestFalse(TEXT("a slick is not tinted"), ApexWheels::TreadedTint(EApexCompoundKind::Slick, Colour, Roughness));
	TestTrue(TEXT("an intermediate is"), ApexWheels::TreadedTint(EApexCompoundKind::Intermediate, Colour, Roughness));
	const FLinearColor Inter = Colour;
	TestTrue(TEXT("matte"), Roughness >= 0.9f);
	TestTrue(TEXT("bluish: more blue than red"), Inter.B > Inter.R * 1.2f);
	TestTrue(TEXT("a wet is"), ApexWheels::TreadedTint(EApexCompoundKind::Wet, Colour, Roughness));
	TestTrue(TEXT("the wet is darker than the inter"), Colour.GetLuminance() < Inter.GetLuminance());
	TestTrue(TEXT("and bluer still"), Colour.B / FMath::Max(Colour.R, 1e-4f) > Inter.B / FMath::Max(Inter.R, 1e-4f));
	TestEqual(TEXT("the tyre slot"), ApexWheels::TyreSlot, FName(TEXT("wheel_tyre")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexWheelsTyreSurfaceTest, "ApexSim.Wheels.TyreSurface", ApexTestFlags)

bool FApexWheelsTyreSurfaceTest::RunTest(const FString& Parameters)
{
	// The tyre parent's tread index per kind.
	TestEqual(TEXT("slick"), ApexWheels::TreadIndex(EApexCompoundKind::Slick), 0.0f);
	TestEqual(TEXT("intermediate"), ApexWheels::TreadIndex(EApexCompoundKind::Intermediate), 1.0f);
	TestEqual(TEXT("wet"), ApexWheels::TreadIndex(EApexCompoundKind::Wet), 2.0f);

	// The sidewall ring: green on an intermediate, blue on a wet, none on a slick.
	FLinearColor Band;
	TestFalse(TEXT("a slick keeps its model's ring"), ApexWheels::CompoundBandColour(EApexCompoundKind::Slick, Band));
	TestTrue(TEXT("an intermediate has one"), ApexWheels::CompoundBandColour(EApexCompoundKind::Intermediate, Band));
	TestTrue(TEXT("green"), Band.G > Band.R * 4.0f && Band.G > Band.B * 4.0f);
	TestTrue(TEXT("a wet has one"), ApexWheels::CompoundBandColour(EApexCompoundKind::Wet, Band));
	TestTrue(TEXT("blue"), Band.B > Band.R * 4.0f && Band.B > Band.G * 4.0f);

	// Deflation: round at running pressure and while unknown, flat at the
	// server's punctured pressure, partly down on the way.
	TestEqual(TEXT("unknown is round"), ApexWheels::DeflatedShare(-1.0f), 0.0f);
	TestEqual(TEXT("running pressure is round"), ApexWheels::DeflatedShare(175.0f), 0.0f);
	TestEqual(TEXT("100 kPa is still round"), ApexWheels::DeflatedShare(100.0f), 0.0f);
	TestEqual(TEXT("the server's PUNCTURED_KPA is flat"), ApexWheels::DeflatedShare(15.0f), 1.0f);
	const float Leaking = ApexWheels::DeflatedShare(60.0f);
	TestTrue(TEXT("a slow leak at 60 kPa is partly down"), Leaking > 0.2f && Leaking < 0.8f);

	// Water: a damp road is a sheen, a wet one soaks the tyre.
	TestEqual(TEXT("dry road, dry tyre"), ApexWheels::WetnessForWater(0.0f), 0.0f);
	TestTrue(TEXT("damp is part wet"), ApexWheels::WetnessForWater(10.0f) > 0.1f && ApexWheels::WetnessForWater(10.0f) < 0.5f);
	TestEqual(TEXT("standing water soaks it"), ApexWheels::WetnessForWater(150.0f), 1.0f);

	// Soaked quickly, dried slowly; no time, no change.
	TestEqual(TEXT("no time"), ApexWheels::WetnessStep(0.3f, 1.0f, 0.0f), 0.3f);
	TestTrue(TEXT("mostly soaked in two seconds"), ApexWheels::WetnessStep(0.0f, 1.0f, 2.0f) > 0.7f);
	TestTrue(TEXT("still wet two seconds after leaving the water"), ApexWheels::WetnessStep(1.0f, 0.0f, 2.0f) > 0.85f);
	TestTrue(TEXT("dry a minute later"), ApexWheels::WetnessStep(1.0f, 0.0f, 60.0f) < 0.1f);

	// Where a wheel touches the road, as the tyre parent's theta: an
	// unturned wheel (axle on X) touches at -Z, theta -90 degrees; turned a
	// quarter about its axle (+Y up toward +Z), the patch is on its -Y.
	TestTrue(TEXT("unturned: -90 degrees"),
		FMath::IsNearlyEqual(ApexWheels::ContactTheta(FTransform::Identity), -UE_HALF_PI, 1e-4f));
	const FTransform Turned(FQuat(FVector::XAxisVector, UE_HALF_PI));
	const float Theta = ApexWheels::ContactTheta(Turned);
	TestTrue(TEXT("turned a quarter: on its -Y (+-180 degrees)"), FMath::IsNearlyEqual(FMath::Abs(Theta), UE_PI, 1e-4f));
	TestTrue(TEXT("scale does not move it"),
		FMath::IsNearlyEqual(ApexWheels::ContactTheta(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(1.0, 3.0, 3.0))), -UE_HALF_PI, 1e-4f));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
