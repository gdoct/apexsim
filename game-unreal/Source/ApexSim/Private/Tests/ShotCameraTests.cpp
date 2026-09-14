#include "ApexTestCommon.h"
#include "Race/ApexShotCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexShotCameraParseTest,
	"ApexSim.Camera.ParseNumberList",
	ApexTestFlags)

bool FApexShotCameraParseTest::RunTest(const FString& Parameters)
{
	using ApexShotCamera::ParseNumberList;
	TArray<double> Values;

	// The command line's comma list and the console's spaces are one format.
	TestTrue(TEXT("comma list parses"), ParseNumberList(TEXT("120.5,-4,3.25,90,-10"), Values));
	TestEqual(TEXT("comma list count"), Values.Num(), 5);
	if (Values.Num() == 5)
	{
		TestEqual(TEXT("first"), Values[0], 120.5);
		TestEqual(TEXT("negative"), Values[1], -4.0);
		TestEqual(TEXT("last"), Values[4], -10.0);
	}
	TestTrue(TEXT("spaces and commas mix"), ParseNumberList(TEXT(" 1 , 2  3 "), Values) && Values.Num() == 3);

	TestFalse(TEXT("empty is not a pose"), ParseNumberList(TEXT(""), Values));
	TestFalse(TEXT("junk rejects the whole list"), ParseNumberList(TEXT("1,two,3"), Values));
	TestEqual(TEXT("rejected list is left empty"), Values.Num(), 0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexShotCameraPoseTest,
	"ApexSim.Camera.ServerFramePose",
	ApexTestFlags)

bool FApexShotCameraPoseTest::RunTest(const FString& Parameters)
{
	using namespace ApexShotCamera;
	ApexShotCamera::FPose Pose;

	// Position: metres to centimetres, +Y left becomes -Y.
	TestTrue(TEXT("goto with position only"), PoseFromGoto(TArray<double>{ 10.0, 5.0, 2.0 }, Pose));
	TestTrue(TEXT("goto location"), Pose.LocationCm.Equals(FVector(1000.0, -500.0, 200.0)));
	TestTrue(TEXT("goto defaults to +X, level"), Pose.Rotation.Equals(FRotator::ZeroRotator));

	// A server yaw of +90 faces +Y (left), which in Unreal is -Y; pitch up stays up.
	TestTrue(TEXT("goto with view"), PoseFromGoto(TArray<double>{ 0.0, 0.0, 0.0, 90.0, 20.0 }, Pose));
	const FVector Forward = Pose.Rotation.Vector();
	TestTrue(TEXT("yaw 90 looks to the server's left"), Forward.Y < -0.9);
	TestTrue(TEXT("pitch positive looks up"), Forward.Z > 0.3);

	double Yaw = 0.0;
	double Pitch = 0.0;
	ApexRace::UnrealRotationToServerView(Pose.Rotation, Yaw, Pitch);
	TestTrue(TEXT("yaw round-trips"), FMath::IsNearlyEqual(Yaw, 90.0, 1e-6));
	TestTrue(TEXT("pitch round-trips"), FMath::IsNearlyEqual(Pitch, 20.0, 1e-6));

	TestFalse(TEXT("goto needs three numbers"), PoseFromGoto(TArray<double>{ 1.0, 2.0 }, Pose));
	TestFalse(TEXT("goto takes at most five"), PoseFromGoto(TArray<double>{ 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 }, Pose));

	// Look-at: standing at the origin, 10 m up, at a point on the server's
	// left and ahead: the view points down, forward and to Unreal's -Y.
	TestTrue(TEXT("look-at parses"), PoseFromLookAt(TArray<double>{ 0.0, 0.0, 10.0, 10.0, 10.0, 0.0 }, Pose));
	TestTrue(TEXT("look-at location"), Pose.LocationCm.Equals(FVector(0.0, 0.0, 1000.0)));
	const FVector LookDir = Pose.Rotation.Vector();
	const FVector Expected = FVector(10.0, -10.0, -10.0).GetSafeNormal();
	TestTrue(TEXT("look-at aims at the target"), LookDir.Equals(Expected, 1e-4));
	TestEqual(TEXT("look-at has no roll"), Pose.Rotation.Roll, 0.0);
	TestFalse(TEXT("look-at needs six numbers"), PoseFromLookAt(TArray<double>{ 1.0, 2.0, 3.0 }, Pose));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
