#include "ApexTestCommon.h"
#include "Race/ApexLobbyCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexLobbyCamTest
{
	using namespace ApexLobbyCam;

	constexpr double RadiusCm = 25000.0;
	/** The road climbs and falls 20 m round the lap, so heights are tested too. */
	constexpr double HillCm = 2000.0;

	/** A round circuit about the origin, Unreal space, its road at RoadZ. */
	TArray<FVector> Ring(double Hill = HillCm, int32 Points = 180)
	{
		TArray<FVector> Out;
		for (int32 Index = 0; Index < Points; ++Index)
		{
			const double Theta = 2.0 * PI * Index / Points;
			Out.Add(FVector(RadiusCm * FMath::Cos(Theta), RadiusCm * FMath::Sin(Theta), Hill * FMath::Sin(Theta)));
		}
		return Out;
	}

	/** Flat ground at 0, nothing in the way. */
	ApexTv::FWorldQueries FlatWorld()
	{
		ApexTv::FWorldQueries World;
		World.GroundZ = [](const FVector&, double& OutZ)
		{
			OutZ = 0.0;
			return true;
		};
		World.IsClear = [](const FVector&, const FVector&) { return true; };
		return World;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLobbyCamNoPathTest, "ApexSim.LobbyCam.NoPathNoShot", ApexTestFlags)

bool FApexLobbyCamNoPathTest::RunTest(const FString& Parameters)
{
	ApexLobbyCam::FFlyover Camera;
	Camera.Reset(1);
	ApexTv::FPose Pose;
	TestFalse(TEXT("no circuit, no shot"), Camera.Tick(0.1f, ApexLobbyCamTest::FlatWorld(), Pose));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLobbyCamRoadZTest, "ApexSim.LobbyCam.RoadHeight", ApexTestFlags)

bool FApexLobbyCamRoadZTest::RunTest(const FString& Parameters)
{
	using namespace ApexLobbyCamTest;
	ApexLobbyCam::FFlyover Camera;
	Camera.Reset(1);
	TArray<FVector> Points = Ring();
	// A loop written with its first point again at the end, as exports may.
	const FVector Closing = Points[0];
	Points.Add(Closing);
	Camera.SetPath(Points);
	TestTrue(TEXT("path"), Camera.HasPath());
	const double Quarter = Camera.GetPath().LengthCm * 0.25;
	TestNearlyEqual(TEXT("the top of the hill a quarter round"), Camera.RoadZ(Quarter), HillCm, 5.0);
	TestNearlyEqual(TEXT("the line"), Camera.RoadZ(0.0), 0.0, 1.0);
	TestNearlyEqual(TEXT("a station past the line wraps"), Camera.RoadZ(Camera.GetPath().LengthCm + Quarter), HillCm, 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLobbyCamFliesTheCircuitTest, "ApexSim.LobbyCam.FliesTheCircuit", ApexTestFlags)

bool FApexLobbyCamFliesTheCircuitTest::RunTest(const FString& Parameters)
{
	using namespace ApexLobbyCamTest;
	using ApexLobbyCam::EShot;
	ApexLobbyCam::FFlyover Camera;
	Camera.Reset(7);
	Camera.SetPath(Ring());
	const ApexTv::FWorldQueries World = FlatWorld();

	TSet<EShot> Seen;
	EShot Previous = EShot::None;
	int32 LastCut = 0;
	int32 Repeats = 0;
	int32 LowCameras = 0;
	int32 OffTheRoad = 0;
	int32 PansInside = 0;
	EShot First = EShot::None;
	constexpr float Dt = 1.0f / 30.0f;
	for (int32 Frame = 0; Frame < 30 * 180; ++Frame)
	{
		ApexTv::FPose Pose;
		if (!TestTrue(TEXT("a shot every frame"), Camera.Tick(Dt, World, Pose)))
		{
			return false;
		}
		const EShot Shot = Camera.GetShot();
		if (First == EShot::None)
		{
			First = Shot;
		}
		if (Camera.GetCutCount() != LastCut)
		{
			LastCut = Camera.GetCutCount();
			Repeats += Shot == Previous;
			Previous = Shot;
		}
		Seen.Add(Shot);
		LowCameras += Pose.Location.Z < 150.0;

		// What it looks at: along the view to the focus distance, which is
		// the subject on the road.
		const FVector Subject = Pose.Location + Pose.Rotation.Vector() * Pose.FocusDistanceCm;
		const double FromRoad = FMath::Abs(FVector(Subject.X, Subject.Y, 0.0).Size() - RadiusCm);
		// The rotation eases after a cut, so allow it a little drift.
		OffTheRoad += FromRoad > 3000.0;

		if (Shot == EShot::Pan && FVector(Pose.Location.X, Pose.Location.Y, 0.0).Size() < RadiusCm)
		{
			++PansInside;
		}
	}
	TestEqual(TEXT("it opens on the orbit"), First, EShot::Orbit);
	TestTrue(TEXT("every kind of shot in three minutes"),
		Seen.Contains(EShot::Orbit) && Seen.Contains(EShot::Glide) && Seen.Contains(EShot::Pan));
	TestTrue(FString::Printf(TEXT("cuts every few seconds (%d)"), Camera.GetCutCount()),
		Camera.GetCutCount() >= 12 && Camera.GetCutCount() <= 30);
	TestEqual(TEXT("never the same kind twice running"), Repeats, 0);
	TestEqual(TEXT("the camera never sits on the ground"), LowCameras, 0);
	TestEqual(TEXT("it looks at the road"), OffTheRoad, 0);
	// The ring bends one way all round: its outside is away from the middle.
	TestEqual(TEXT("trackside on the outside of the bend"), PansInside, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexLobbyCamBlockedTest, "ApexSim.LobbyCam.AvoidsABlockedView", ApexTestFlags)

bool FApexLobbyCamBlockedTest::RunTest(const FString& Parameters)
{
	using namespace ApexLobbyCamTest;
	ApexLobbyCam::FFlyover Camera;
	Camera.Reset(3);
	// Level with the ground: a pan stands at its height over the road.
	Camera.SetPath(Ring(/*Hill*/ 0.0));
	// A wall of trees: nothing under 15 m sees the road.
	ApexTv::FWorldQueries World = FlatWorld();
	World.IsClear = [](const FVector& From, const FVector&) { return From.Z > 1500.0; };

	int32 Pans = 0;
	for (int32 Frame = 0; Frame < 30 * 120; ++Frame)
	{
		ApexTv::FPose Pose;
		Camera.Tick(1.0f / 30.0f, World, Pose);
		Pans += Camera.GetShot() == ApexLobbyCam::EShot::Pan;
	}
	TestEqual(TEXT("no low shot that cannot see the road"), Pans, 0);
	TestTrue(TEXT("still cutting"), Camera.GetCutCount() >= 8);
	return true;
}

#endif
