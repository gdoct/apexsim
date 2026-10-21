#include "ApexTestCommon.h"
#include "UI/ApexGlobe.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------
// The projection: the view's centre faces the viewer, the far side does not,
// north is up, and a pin's head stands out beyond its base.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGlobeProjectionTest,
	"ApexSim.UI.Globe.Projection",
	ApexTestFlags)

bool FApexGlobeProjectionTest::RunTest(const FString& Parameters)
{
	using namespace ApexGlobe;

	FView View;
	View.LonDeg = 9.28;
	View.LatDeg = 45.62;
	View.Zoom = 2.0;

	const FVector3d Centre = Project(View, 9.28, 45.62);
	TestTrue(TEXT("the view's centre is in the middle"), FMath::Abs(Centre.X) < 1e-9 && FMath::Abs(Centre.Y) < 1e-9);
	TestTrue(TEXT("and faces the viewer"), FMath::IsNearlyEqual(Centre.Z, 1.0, 1e-9));

	const FVector3d Antipode = Project(View, WrapLon(9.28 + 180.0), -45.62);
	TestTrue(TEXT("the far side faces away"), Antipode.Z < -0.99);

	const FVector3d North = Project(View, 9.28, 60.0);
	TestTrue(TEXT("north is up"), North.Y > 0.0 && FMath::Abs(North.X) < 1e-9);
	const FVector3d East = Project(View, 30.0, 45.62);
	TestTrue(TEXT("east is right"), East.X > 0.0);

	const FVector3d Base = Project(View, 30.0, 40.0, 1.0);
	const FVector3d Head = Project(View, 30.0, 40.0, 1.0 + PinHeight);
	TestTrue(TEXT("the head stands out from the base"), FVector2D(Head.X, Head.Y).Size() > FVector2D(Base.X, Base.Y).Size());
	TestTrue(TEXT("the zoom scales the picture"), FMath::IsNearlyEqual(FVector2D(Project(View, 9.28, 90.0).X, Project(View, 9.28, 90.0).Y).Size(),
		2.0 * FMath::Cos(FMath::DegreesToRadians(45.62)), 1e-6));

	// The porting note's direction is the same point the projection draws.
	View.LonDeg = 0.0;
	View.LatDeg = 0.0;
	View.Zoom = 1.0;
	const FVector3d Dir = Direction(144.97, -37.85);
	const FVector3d P = Project(View, 144.97, -37.85);
	TestTrue(TEXT("Direction matches Project at the origin view"),
		FMath::IsNearlyEqual(Dir.X, P.X, 1e-9) && FMath::IsNearlyEqual(Dir.Y, P.Y, 1e-9) && FMath::IsNearlyEqual(Dir.Z, P.Z, 1e-9));

	TestEqual(TEXT("wrap east"), WrapLon(190.0), -170.0);
	TestEqual(TEXT("wrap west"), WrapLon(-190.0), 170.0);
	TestEqual(TEXT("wrap far"), WrapLon(-910.0), 170.0);
	TestEqual(TEXT("fade behind"), PinFade(0.0), 0.0);
	TestEqual(TEXT("fade front"), PinFade(0.9), 1.0);
	TestTrue(TEXT("fade between"), PinFade(0.2) > 0.0 && PinFade(0.2) < 1.0);
	return true;
}

// -----------------------------------------------------------------------------
// The motion: a fly-to lands, across the date line the short way; the idle
// spin stops on hover and selection; a throw decays; zoom is clamped.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGlobeMotionTest,
	"ApexSim.UI.Globe.Motion",
	ApexTestFlags)

bool FApexGlobeMotionTest::RunTest(const FString& Parameters)
{
	using namespace ApexGlobe;

	FView View;
	View.LonDeg = 170.0;
	View.bSelected = true;
	FlyTo(View, -170.0, 10.0);
	double Travelled = 0.0;
	double Last = View.LonDeg;
	for (int32 i = 0; i < 600 && View.bFlying; ++i)
	{
		Step(View, 1.0 / 60.0);
		Travelled += FMath::Abs(WrapLon(View.LonDeg - Last));
		Last = View.LonDeg;
	}
	TestFalse(TEXT("the fly-to lands"), View.bFlying);
	TestEqual(TEXT("on its longitude"), View.LonDeg, -170.0);
	TestEqual(TEXT("and latitude"), View.LatDeg, 10.0);
	TestTrue(TEXT("over the date line, the short way"), Travelled < 25.0);
	TestTrue(TEXT("zooming in to the selection"), View.Zoom > 2.0);

	// Selected: the globe holds still.
	const double Held = View.LonDeg;
	Step(View, 1.0);
	TestEqual(TEXT("no idle spin while selected"), View.LonDeg, Held);
	View.bSelected = false;
	View.bHovered = true;
	Step(View, 1.0);
	TestEqual(TEXT("nor under the pointer"), View.LonDeg, Held);
	View.bHovered = false;
	Step(View, 1.0);
	TestTrue(TEXT("idle, it turns east"), FMath::IsNearlyEqual(View.LonDeg, WrapLon(Held + AutoRotateDegPerSec), 1e-6));

	// A drag throws the globe, and the spin dies away.
	FView Thrown;
	BeginDrag(Thrown);
	for (int32 i = 0; i < 5; ++i)
	{
		Drag(Thrown, FVector2D(-20.0, 0.0), 400.0, 1.0 / 60.0);
	}
	EndDrag(Thrown, 0.01);
	TestTrue(TEXT("a quick release keeps the throw"), Thrown.SpinLon > 10.0);
	for (int32 i = 0; i < 600; ++i)
	{
		Step(Thrown, 1.0 / 60.0);
	}
	TestTrue(TEXT("the throw dies away"), FMath::Abs(Thrown.SpinLon) <= 0.02);

	FView Rested;
	BeginDrag(Rested);
	Drag(Rested, FVector2D(-20.0, 0.0), 400.0, 1.0 / 60.0);
	EndDrag(Rested, 0.5);
	TestEqual(TEXT("a drag that rested throws nothing"), Rested.SpinLon, 0.0);

	FView Zoomed;
	for (int32 i = 0; i < 20; ++i)
	{
		ZoomBy(Zoomed, ZoomStep);
	}
	TestEqual(TEXT("zoom in stops at the limit"), Zoomed.TargetZoom, MaxZoom);
	for (int32 i = 0; i < 40; ++i)
	{
		ZoomBy(Zoomed, 1.0 / ZoomStep);
	}
	TestEqual(TEXT("zoom out stops at the limit"), Zoomed.TargetZoom, MinZoom);

	BeginDrag(Zoomed);
	Drag(Zoomed, FVector2D(0.0, 100000.0), 100.0, 0.1);
	TestEqual(TEXT("the view never tips over a pole"), Zoomed.LatDeg, LatLimitDeg);

	Reset(Zoomed);
	TestFalse(TEXT("reset clears the selection"), Zoomed.bSelected);
	TestEqual(TEXT("and heads home"), Zoomed.TargetZoom, HomeZoom);
	return true;
}

// -----------------------------------------------------------------------------
// Labels and the pad's jumps between pins.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGlobeLabelsTest,
	"ApexSim.UI.Globe.Labels",
	ApexTestFlags)

bool FApexGlobeLabelsTest::RunTest(const FString& Parameters)
{
	using namespace ApexGlobe;

	auto Label = [](double X, double Depth, bool bAlways = false)
	{
		FLabel L;
		L.Rect = FBox2D(FVector2D(X, 0.0), FVector2D(X + 100.0, 30.0));
		L.Depth = Depth;
		L.bAlways = bAlways;
		return L;
	};
	// Two overlapping, one clear of both.
	TArray<bool> Shown = PlaceLabels({ Label(0.0, 0.5), Label(50.0, 0.9), Label(300.0, 0.2) });
	TestFalse(TEXT("the farther of two overlapping is skipped"), Shown[0]);
	TestTrue(TEXT("the nearer is placed"), Shown[1]);
	TestTrue(TEXT("one clear of the rest shows"), Shown[2]);

	Shown = PlaceLabels({ Label(0.0, 0.5, true), Label(50.0, 0.9) });
	TestTrue(TEXT("the selected one always shows"), Shown[0]);
	TestFalse(TEXT("and wins over a nearer one"), Shown[1]);

	const TArray<FVector2D> Pins = { FVector2D(0.0, 0.0), FVector2D(100.0, 10.0), FVector2D(40.0, -80.0), FVector2D(-100.0, 0.0), FVector2D(300.0, 0.0) };
	TArray<bool> Usable = { true, true, true, true, true };
	TestEqual(TEXT("right goes to the nearest that way"), NearestToward(Pins, Usable, 0, Pins[0], FVector2D(1.0, 0.0)), 1);
	TestEqual(TEXT("up goes up (Y down)"), NearestToward(Pins, Usable, 0, Pins[0], FVector2D(0.0, -1.0)), 2);
	TestEqual(TEXT("left"), NearestToward(Pins, Usable, 0, Pins[0], FVector2D(-1.0, 0.0)), 3);
	TestEqual(TEXT("nothing below"), NearestToward(Pins, Usable, 0, Pins[0], FVector2D(0.0, 1.0)), INDEX_NONE);
	Usable[1] = false;
	TestEqual(TEXT("a hidden pin is passed over"), NearestToward(Pins, Usable, 0, Pins[0], FVector2D(1.0, 0.0)), 4);

	TestTrue(TEXT("the land mask has its dots"), LandDots().Num() > 1500);
	return true;
}

#endif
