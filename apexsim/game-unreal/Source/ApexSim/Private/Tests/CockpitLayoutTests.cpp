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

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitCarWheelTest,
	"ApexSim.Cockpit.CarWheel",
	ApexTestFlags)

bool FApexCockpitCarWheelTest::RunTest(const FString& Parameters)
{
	const FBox Box = ApexCockpit::ActorFrameBox(GtMeshBounds, MeshMount);
	const FApexCockpitLayout Derived = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::Closed, FApexCockpitOverrides());
	TestTrue(TEXT("the rig's rim by default"), Derived.bRigWheel);
	TestTrue(TEXT("the rig's display by default"), Derived.bRigDash);
	TestTrue(TEXT("no car wheel by default"), Derived.SteeringWheelModel.IsEmpty());

	FApexCockpitOverrides Overrides;
	Overrides.bRigWheel = false;
	Overrides.bRigDash = false;
	Overrides.WheelRakeDeg = -6.0f;
	Overrides.WheelLockDeg = 270.0f;
	Overrides.RuntimeSteeringWheel = TEXT("C:/cars/x/steer.glb");
	const FApexCockpitLayout Own = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::Closed, Overrides);
	TestFalse(TEXT("rim off"), Own.bRigWheel);
	TestFalse(TEXT("display off"), Own.bRigDash);
	TestEqual(TEXT("the car's rake"), Own.WheelRakeDeg, -6.0f);
	TestEqual(TEXT("the car's lock"), Own.WheelLockDeg, 270.0f);
	TestEqual(TEXT("the car's wheel"), Own.SteeringWheelModel, Overrides.RuntimeSteeringWheel);
	TestEqual(TEXT("no screen named: the display keeps its place"), Own.DashCm, Derived.DashCm);
	TestEqual(TEXT("no screen named: the display keeps its width"), Own.DashWidthCm, Derived.DashWidthCm);
	{
		// A class steering wheel names its screen; the display goes on it.
		FApexCockpitOverrides Class = Overrides;
		Class.RuntimeDashCm = FVector(-1.1f, 0.0f, 2.4f);
		Class.RuntimeDashWidthCm = 10.0f;
		const FApexCockpitLayout OnScreen = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::Closed, Class);
		TestEqual(TEXT("the display on the wheel's screen"), OnScreen.DashCm, Class.RuntimeDashCm);
		TestEqual(TEXT("the display the screen's width"), OnScreen.DashWidthCm, 10.0f);
		// Without a wheel to carry it, a screen means nothing.
		Class.RuntimeSteeringWheel.Reset();
		const FApexCockpitLayout NoWheel = ApexCockpit::DeriveLayout(Box, EApexCockpitStyle::Closed, Class);
		TestEqual(TEXT("no wheel: the rig's display width"), NoWheel.DashWidthCm, Derived.DashWidthCm);
	}
	TestEqual(TEXT("the seat is not moved by any of it"), Own.Eye, Derived.Eye);

	// The rig mounts a car GLB's steering wheel turned -90 about Z, as the
	// race car mounts the body: the column (glTF +Z, the mesh's +Y) must land
	// on the pivot's +X, which the rake tips and the steering rolls about,
	// and the car's left (glTF +X) on the pivot's -Y.
	const FQuat Mount = FRotator(0.0f, -90.0f, 0.0f).Quaternion();
	TestTrue(TEXT("column along the pivot's X"), Mount.RotateVector(FVector(0.0, 1.0, 0.0)).Equals(FVector(1.0, 0.0, 0.0), 1.0e-4));
	TestTrue(TEXT("the car's left on the pivot's left"), Mount.RotateVector(FVector(1.0, 0.0, 0.0)).Equals(FVector(0.0, -1.0, 0.0), 1.0e-4));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitScreenNitsTest,
	"ApexSim.Cockpit.ScreenNitsFollowExposure",
	ApexTestFlags)

bool FApexCockpitScreenNitsTest::RunTest(const FString& Parameters)
{
	using namespace ApexCockpit;
	auto ExposureAt = [](float Ev) { return 1.0f / (1.2f * FMath::Exp2(Ev)); };

	TestEqual(TEXT("EV100 round trip"), ExposureToEv100(ExposureAt(8.0f)), 8.0f, 1.0e-3f);
	TestEqual(TEXT("unknown exposure: as set"), ScreenNitsAt(4500.0f, 0.0f), 4500.0f);
	TestEqual(TEXT("a sunny race at the reference: as set"), ScreenNitsAt(4500.0f, ExposureAt(ScreenReferenceEv)), 4500.0f, 1.0f);
	TestEqual(TEXT("brighter than the reference: never above the knob"), ScreenNitsAt(4500.0f, ExposureAt(15.0f)), 4500.0f, 1.0f);
	// Night: the exposure opens six stops at a floodlit EV 8, nine at a dark
	// circuit's EV 5; the screen comes down by as many.
	TestEqual(TEXT("EV 8: six stops down"), ScreenNitsAt(4500.0f, ExposureAt(8.0f)), 4500.0f / 64.0f, 0.5f);
	TestEqual(TEXT("EV 5: nine stops down"), ScreenNitsAt(4500.0f, ExposureAt(5.0f)), 4500.0f / 512.0f, 0.1f);
	TestEqual(TEXT("a moonless EV -2: never under 1"), ScreenNitsAt(4500.0f, ExposureAt(-2.0f)), 1.0f);
	TestEqual(TEXT("the reference moves with its knob"), ScreenNitsAt(4500.0f, ExposureAt(8.0f), 10.0f), 4500.0f / 4.0f, 1.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitArmSolveTest,
	"ApexSim.Cockpit.ArmSolve",
	ApexTestFlags)

bool FApexCockpitArmSolveTest::RunTest(const FString& Parameters)
{
	using namespace ApexCockpit;
	// A GT3 seat: eye 45 cm behind the hub and 18 cm above it, the right
	// wrist 21 cm out from the hub turned through every rim angle a GT3 uses.
	const FVector Eye(-35.0f, 38.0f, 108.0f);
	const FVector Hub(10.0f, 38.0f, 90.0f);
	const FVector Shoulder = ShoulderFromEye(Eye, 1.0f);
	TestTrue(TEXT("the right shoulder is right of and below the eye"), Shoulder.Y > Eye.Y && Shoulder.Z < Eye.Z);
	const FVector Pole(-0.2f, 0.75f, -1.0f);
	for (int32 Deg = -270; Deg <= 270; Deg += 15)
	{
		const FVector Wrist = Hub + FRotator(0.0f, 0.0f, static_cast<float>(Deg)).RotateVector(FVector(-10.0f, 21.0f, -4.0f));
		const FVector Elbow = SolveElbow(Shoulder, Wrist, UpperArmCm, ForearmCm, Pole);
		const FString At = FString::Printf(TEXT(" at %d deg"), Deg);
		// The bones keep their length wherever the wrist goes in reach.
		TestEqual(TEXT("upper arm length") + At, static_cast<float>((Elbow - Shoulder).Size()), UpperArmCm, 0.05f);
		if ((Wrist - Shoulder).Size() <= UpperArmCm + ForearmCm)
		{
			TestEqual(TEXT("forearm reaches the wrist") + At, static_cast<float>((Wrist - Elbow).Size()), ForearmCm, 0.05f);
		}
		// The elbow hangs: below the line from shoulder to wrist, and within
		// a cabin's half width of the seat.
		const FVector Mid = (Shoulder + Wrist) * 0.5f;
		TestTrue(TEXT("the elbow hangs below the reach") + At, Elbow.Z < Mid.Z + 1.0f);
		TestTrue(TEXT("the elbow stays in the cabin") + At, FMath::Abs(Elbow.Y - Eye.Y) < 60.0f);
	}
	// Out of reach: the arm lies straight along the line.
	const FVector Far = Shoulder + FVector(100.0f, 0.0f, 0.0f);
	const FVector Straight = SolveElbow(Shoulder, Far, UpperArmCm, ForearmCm, Pole);
	TestEqual(TEXT("past full reach the elbow is on the line"), static_cast<float>(Straight.Y), static_cast<float>(Shoulder.Y), 0.05f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexCockpitHeadShakeTest,
	"ApexSim.Cockpit.HeadShake",
	ApexTestFlags)

namespace
{
	/** A kerb: the wheels' suspension speeds swinging at 20 Hz, half a metre a second. */
	ApexCockpit::FHeadShakeInput KerbAt(float Seconds)
	{
		ApexCockpit::FHeadShakeInput In;
		const float V = 0.5f * FMath::Sin(UE_TWO_PI * 20.0f * Seconds);
		for (float& Wheel : In.SuspensionVelocityMps)
		{
			Wheel = V;
		}
		return In;
	}

	/** Run `Seconds` at `Fps` from rest; the input at each frame's start. The peak |z|, cm, and the last offset. */
	template <typename InputAt>
	float RunShake(float Fps, float Seconds, float Amount, InputAt Input, ApexCockpit::FHeadShakeOffset* Last = nullptr, float* PeakPitch = nullptr)
	{
		ApexCockpit::FHeadShake State;
		ApexCockpit::FHeadShakeOffset Out;
		float Peak = 0.0f;
		const float Dt = 1.0f / Fps;
		const int32 Frames = FMath::RoundToInt32(Seconds * Fps);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Out = ApexCockpit::StepHeadShake(State, Input(Frame * Dt), Dt, Amount);
			Peak = FMath::Max(Peak, FMath::Abs(static_cast<float>(Out.OffsetCm.Z)));
			if (PeakPitch)
			{
				*PeakPitch = FMath::Max(*PeakPitch, FMath::Abs(Out.PitchDeg));
			}
		}
		if (Last)
		{
			*Last = Out;
		}
		return Peak;
	}
}

bool FApexCockpitHeadShakeTest::RunTest(const FString& Parameters)
{
	using namespace ApexCockpit;
	const auto Still = [](float) { return FHeadShakeInput(); };
	const auto Idle = [](float) { FHeadShakeInput In; In.EngineRpm = 1200.0f; return In; };
	// A crest's compression: the body pushed up at 1 g for half a second.
	const auto Crest = [](float T) { FHeadShakeInput In; In.VerticalAccelMps2 = T < 0.5f ? 9.81f : 0.0f; return In; };
	const auto Kerb = [](float T) { return KerbAt(T); };
	const auto Porpoising = [](float) { FHeadShakeInput In; In.PorpoiseAmp = 0.25f; return In; };

	TestEqual(TEXT("a smooth road at a standstill is still"), RunShake(60.0f, 2.0f, 1.0f, Still), 0.0f);
	const float Buzz = RunShake(60.0f, 2.0f, 1.0f, Idle);
	TestTrue(TEXT("the engine buzzes under a millimetre"), Buzz > 0.0f && Buzz < 0.1f);

	// 0 on the slider is today's eye, whatever the car does.
	FHeadShakeOffset Last;
	TestEqual(TEXT("off: no crest"), RunShake(60.0f, 1.0f, 0.0f, Crest, &Last), 0.0f);
	TestTrue(TEXT("off: no nod"), Last.PitchDeg == 0.0f && Last.OffsetCm.IsZero());
	TestEqual(TEXT("off: no kerb"), RunShake(60.0f, 1.0f, 0.0f, Kerb), 0.0f);

	// Millimetres to a few centimetres, never the seat.
	const float CrestPeak = RunShake(60.0f, 2.0f, 1.0f, Crest);
	TestTrue(TEXT("a crest moves the head a few centimetres"), CrestPeak > 1.0f && CrestPeak < 5.01f);
	float KerbPitch = 0.0f;
	const float KerbPeak = RunShake(60.0f, 1.0f, 1.0f, Kerb, nullptr, &KerbPitch);
	TestTrue(TEXT("a kerb shakes it by millimetres"), KerbPeak > 0.1f && KerbPeak < 3.0f);
	// A kerb's edge reads as the server's 6 m/s cap for a snapshot: a jolt, not a leap.
	const float StrikePeak = RunShake(60.0f, 1.0f, 1.0f, [](float T) {
		FHeadShakeInput In;
		for (float& Wheel : In.SuspensionVelocityMps) { Wheel = T < 0.01f ? 6.0f : 0.0f; }
		return In; });
	TestTrue(TEXT("a capped strike jolts the head under a centimetre and a half"), StrikePeak > 0.1f && StrikePeak < 1.5f);
	const float PorpoisePeak = RunShake(60.0f, 2.0f, 1.0f, Porpoising);
	TestTrue(TEXT("porpoising bounces it about the floor's swing"), PorpoisePeak > 0.5f && PorpoisePeak < 4.0f);
	TestTrue(TEXT("half the setting is half the crest"),
		FMath::IsNearlyEqual(RunShake(60.0f, 2.0f, 0.5f, Crest), CrestPeak * 0.5f, 0.01f));

	// The front against the rear nods it, a fraction of a degree.
	float Nod = 0.0f;
	RunShake(60.0f, 1.0f, 1.0f, [](float T) { FHeadShakeInput In = KerbAt(T); In.SuspensionVelocityMps[2] = In.SuspensionVelocityMps[3] = 0.0f; return In; }, nullptr, &Nod);
	TestTrue(TEXT("a front kerb nods the head under a degree"), Nod > 0.01f && Nod <= 0.6f);

	// The same over 30, 60 and 144 frames a second.
	FHeadShakeOffset At30, At60, At144;
	RunShake(30.0f, 0.4f, 1.0f, Crest, &At30);
	RunShake(60.0f, 0.4f, 1.0f, Crest, &At60);
	RunShake(144.0f, 0.4f, 1.0f, Crest, &At144);
	TestTrue(TEXT("frame rate: 30 like 60"), FMath::IsNearlyEqual(At30.OffsetCm.Z, At60.OffsetCm.Z, 0.05));
	TestTrue(TEXT("frame rate: 144 like 60"), FMath::IsNearlyEqual(At144.OffsetCm.Z, At60.OffsetCm.Z, 0.05));
	const float Peak30 = RunShake(30.0f, 1.0f, 1.0f, Porpoising);
	const float Peak144 = RunShake(144.0f, 1.0f, 1.0f, Porpoising);
	TestTrue(TEXT("frame rate: porpoising's swing alike"), FMath::Abs(Peak30 - Peak144) < 0.25f);

	// A formula tub carries more of the road than a closed car's seat.
	TestTrue(TEXT("a tub is stiffer than a seat"), SeatStiffness(true) > SeatStiffness(false));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
