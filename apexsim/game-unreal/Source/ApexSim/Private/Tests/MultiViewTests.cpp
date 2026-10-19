#include "ApexMultiView.h"
#include "ApexTestCommon.h"
#include "Camera/CameraTypes.h"
#include "SceneView.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ApexMultiView;

	FTripleGeometry Rig()
	{
		FTripleGeometry G;
		G.ScreenWidthCm = 60.0f;
		G.BezelCm = 2.0f;
		G.EyeDistanceCm = 65.0f;
		G.SideAngleDeg = 45.0f;
		G.AspectRatio = 16.0f / 9.0f;
		return G;
	}

	/**
	 * The ends of the left panel's picture in the world, with the eye at the
	 * origin looking down +X and the centre panel EyeDistance away: the same
	 * construction SideView documents, done in world space so the test does
	 * not share its algebra.
	 */
	void LeftPanelEnds(const FTripleGeometry& G, float EyeDistance, FVector& OutNear, FVector& OutFar)
	{
		const float Theta = FMath::DegreesToRadians(G.SideAngleDeg);
		const FVector Hinge(EyeDistance, -(0.5f * G.ScreenWidthCm + 0.5f * G.BezelCm), 0.0f);
		const FVector Along(-FMath::Sin(Theta), -FMath::Cos(Theta), 0.0f);
		OutNear = Hinge + Along * (0.5f * G.BezelCm);
		OutFar = OutNear + Along * G.ScreenWidthCm;
	}

	/** Where a world point lands in a view's clip space, through the engine's own projection. */
	FVector2f Ndc(const FMinimalViewInfo& View, const FVector& Point)
	{
		FMinimalViewInfo Copy = View;
		FSceneViewProjectionData Data;
		Data.ViewOrigin = View.Location;
		// ULocalPlayer::GetProjectionData's frame: the inverse rotation, then
		// the engine's swap of X-forward/Y-right/Z-up into view X-right/Y-up/Z-depth.
		Data.ViewRotationMatrix = FInverseRotationMatrix(View.Rotation) * FMatrix(
			FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
		// The engine reads the view's size from the projection data's own rect.
		const FIntRect Rect(0, 0, 1920, 1080);
		Data.SetViewRectangle(Rect);
		FMinimalViewInfo::CalculateProjectionMatrixGivenViewRectangle(Copy, AspectRatio_MaintainXFOV, Rect, Data);
		const FVector4 Clip = Data.ComputeViewProjectionMatrix().TransformFVector4(FVector4(Point, 1.0f));
		return FVector2f(static_cast<float>(Clip.X / Clip.W), static_cast<float>(Clip.Y / Clip.W));
	}

	FMinimalViewInfo ViewOf(const FSideView& Side)
	{
		FMinimalViewInfo View;
		View.Location = FVector::ZeroVector;
		View.Rotation = FRotator(0.0f, Side.YawDeg, 0.0f);
		View.FOV = Side.FovDeg;
		View.OffCenterProjectionOffset = FVector2D(Side.OffCenterX, 0.0f);
		return View;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMultiViewFlatRowTest,
	"ApexSim.MultiView.FlatRowIsOnePlane",
	ApexTestFlags)

bool FApexMultiViewFlatRowTest::RunTest(const FString& Parameters)
{
	// Three monitors in a flat row with no bezel are one plane: each side view
	// is the centre view's frustum slid by a whole panel, unturned.
	FTripleGeometry G = Rig();
	G.SideAngleDeg = 0.0f;
	G.BezelCm = 0.0f;
	const float Centre = CentreFovDeg(G);
	TestEqual(TEXT("centre fov from width and distance"), Centre,
		FMath::RadiansToDegrees(2.0f * FMath::Atan(30.0f / 65.0f)), 0.01f);

	const FSideView Left = SideView(G, Centre, true);
	const FSideView Right = SideView(G, Centre, false);
	TestEqual(TEXT("left not turned"), Left.YawDeg, 0.0f, 0.001f);
	TestEqual(TEXT("left same field"), Left.FovDeg, Centre, 0.01f);
	TestEqual(TEXT("left slid one panel"), Left.OffCenterX, -2.0f, 0.001f);
	TestEqual(TEXT("right slid one panel"), Right.OffCenterX, 2.0f, 0.001f);

	// A bezel pushes the panel further out by the gap, in panel halves.
	G.BezelCm = 6.0f;
	TestEqual(TEXT("bezel widens the slide"), SideView(G, Centre, true).OffCenterX, -2.2f, 0.001f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMultiViewPanelEdgesTest,
	"ApexSim.MultiView.SidePanelsProjectToTheirEdges",
	ApexTestFlags)

bool FApexMultiViewPanelEdgesTest::RunTest(const FString& Parameters)
{
	// The whole point: through the engine's own projection, a toed-in side
	// panel's picture fills its view exactly — its inner end on the view's
	// inner edge, its outer end on the outer edge, its top on the top — so
	// what the panel shows is what the eye would see through that panel.
	const FTripleGeometry G = Rig();
	const float Centre = CentreFovDeg(G);
	const float Height = G.ScreenWidthCm / G.AspectRatio;

	FVector Near, Far;
	LeftPanelEnds(G, G.EyeDistanceCm, Near, Far);

	const FMinimalViewInfo LeftView = ViewOf(SideView(G, Centre, true));
	TestEqual(TEXT("left panel's inner end is the view's right edge"), Ndc(LeftView, Near).X, 1.0f, 0.002f);
	TestEqual(TEXT("left panel's outer end is the view's left edge"), Ndc(LeftView, Far).X, -1.0f, 0.002f);
	TestEqual(TEXT("left panel's top is the view's top"),
		Ndc(LeftView, Near + FVector(0.0f, 0.0f, 0.5f * Height)).Y, 1.0f, 0.002f);
	TestEqual(TEXT("left panel's middle is the view's middle"),
		Ndc(LeftView, 0.5f * (Near + Far)).X, 0.0f, 0.002f);

	// The right panel is the mirror image.
	const FMinimalViewInfo RightView = ViewOf(SideView(G, Centre, false));
	const FVector MirrorNear(Near.X, -Near.Y, Near.Z);
	const FVector MirrorFar(Far.X, -Far.Y, Far.Z);
	TestEqual(TEXT("right panel's inner end is the view's left edge"), Ndc(RightView, MirrorNear).X, -1.0f, 0.002f);
	TestEqual(TEXT("right panel's outer end is the view's right edge"), Ndc(RightView, MirrorFar).X, 1.0f, 0.002f);

	// And the centre panel's edges are the centre view's.
	FMinimalViewInfo CentreView;
	CentreView.FOV = Centre;
	TestEqual(TEXT("centre panel's left edge"), Ndc(CentreView, FVector(G.EyeDistanceCm, -0.5f * G.ScreenWidthCm, 0.0f)).X, -1.0f, 0.002f);
	TestEqual(TEXT("centre panel's top edge"), Ndc(CentreView, FVector(G.EyeDistanceCm, 0.0f, 0.5f * Height)).Y, 1.0f, 0.002f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMultiViewSeamTest,
	"ApexSim.MultiView.ALensKeepsTheSeam",
	ApexTestFlags)

bool FApexMultiViewSeamTest::RunTest(const FString& Parameters)
{
	// A broadcast camera draws the centre with a narrower field than the rig
	// has. The side views then have to be laid out as if the eye sat further
	// back, or the picture breaks at the joins: with no bezel the shared
	// edge must be on the centre view's edge and on the side view's edge at
	// every field and every angle.
	FTripleGeometry G = Rig();
	G.BezelCm = 0.0f;
	for (const float Fov : { 18.0f, 35.0f, 49.5f, 75.0f })
	{
		for (const float Angle : { 0.0f, 25.0f, 45.0f, 70.0f })
		{
			G.SideAngleDeg = Angle;
			const float Eye = EyeDistanceForFov(G, Fov);
			FVector Near, Far;
			LeftPanelEnds(G, Eye, Near, Far);

			FMinimalViewInfo CentreView;
			CentreView.FOV = Fov;
			const FMinimalViewInfo LeftView = ViewOf(SideView(G, Fov, true));
			const FString Case = FString::Printf(TEXT("fov %.1f angle %.0f: "), Fov, Angle);
			TestEqual(Case + TEXT("the join is the centre view's left edge"), Ndc(CentreView, Near).X, -1.0f, 0.002f);
			TestEqual(Case + TEXT("the join is the left view's right edge"), Ndc(LeftView, Near).X, 1.0f, 0.002f);
			TestEqual(Case + TEXT("the far end is the left view's left edge"), Ndc(LeftView, Far).X, -1.0f, 0.002f);
		}
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexMultiViewMonitorRowTest,
	"ApexSim.MultiView.FindsTheMonitorRow",
	ApexTestFlags)

bool FApexMultiViewMonitorRowTest::RunTest(const FString& Parameters)
{
	auto Monitor = [](int32 X, int32 Y, int32 W, int32 H, bool bPrimary = false)
	{
		FMonitorRect M;
		M.Rect = FIntRect(X, Y, X + W, Y + H);
		M.bPrimary = bPrimary;
		return M;
	};

	FIntRect Span;

	// The usual rig: the primary in the middle, the others either side, and a
	// fourth monitor above for the pit wall, listed in no particular order.
	TArray<FMonitorRect> Rig = {
		Monitor(1920, 0, 1920, 1080, true),
		Monitor(0, -1080, 2560, 1440),
		Monitor(3840, 0, 1920, 1080),
		Monitor(0, 0, 1920, 1080),
	};
	TestTrue(TEXT("a row of three is found"), FindTripleRow(Rig, Span));
	TestEqual(TEXT("span left"), Span.Min.X, 0);
	TestEqual(TEXT("span top"), Span.Min.Y, 0);
	TestEqual(TEXT("span width"), Span.Width(), 5760);
	TestEqual(TEXT("span height"), Span.Height(), 1080);

	// A desktop laid out by hand can be a pixel or two off; a monitor of a
	// different size cannot be part of the row.
	TArray<FMonitorRect> Rough = { Monitor(-1922, 1, 1920, 1080), Monitor(0, 0, 1920, 1080, true), Monitor(1921, 0, 1920, 1080) };
	TestTrue(TEXT("a pixel of slack is allowed"), FindTripleRow(Rough, Span));
	TestEqual(TEXT("the row starts at the left monitor"), Span.Min.X, -1922);

	TArray<FMonitorRect> Mixed = { Monitor(-2560, 0, 2560, 1440), Monitor(0, 0, 1920, 1080, true), Monitor(1920, 0, 1920, 1080) };
	TestFalse(TEXT("three of two sizes are not a row"), FindTripleRow(Mixed, Span));

	TArray<FMonitorRect> Two = { Monitor(0, 0, 1920, 1080, true), Monitor(1920, 0, 1920, 1080) };
	TestFalse(TEXT("two monitors are not a row"), FindTripleRow(Two, Span));

	TArray<FMonitorRect> Stacked = { Monitor(0, 0, 1920, 1080, true), Monitor(0, 1080, 1920, 1080), Monitor(0, 2160, 1920, 1080) };
	TestFalse(TEXT("a column is not a row"), FindTripleRow(Stacked, Span));

	// Four in a row: the three with the primary in the middle win.
	TArray<FMonitorRect> Four = { Monitor(0, 0, 1920, 1080), Monitor(1920, 0, 1920, 1080), Monitor(3840, 0, 1920, 1080, true), Monitor(5760, 0, 1920, 1080) };
	TestTrue(TEXT("four in a row hold a triple"), FindTripleRow(Four, Span));
	TestEqual(TEXT("the triple is centred on the primary"), Span.Min.X, 1920);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
