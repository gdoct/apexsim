#include "ApexTestCommon.h"
#include "Race/ApexGrassMarks.h"
#include "Track/ApexTrackSceneData.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGrassMarksTrailTest,
	"ApexSim.GrassMarks.Trail",
	ApexTestFlags)

bool FApexGrassMarksTrailTest::RunTest(const FString& Parameters)
{
	using namespace ApexGrassMarks;
	FTrail Trail;
	FMark Mark;
	const FVector Up = FVector::UpVector;

	TestFalse(TEXT("the first point on grass opens the trail"), Advance(Trail, FVector(0, 0, 0), Up, true, false, Mark));
	TestTrue(TEXT("open"), Trail.bOpen);
	TestFalse(TEXT("short of a step, nothing"), Advance(Trail, FVector(40, 0, 0), Up, true, false, Mark));
	TestTrue(TEXT("a step laid"), Advance(Trail, FVector(80, 0, 0), Up, true, true, Mark));
	TestTrue(TEXT("from the trail's start to here"), Mark.From.Equals(FVector::ZeroVector) && Mark.To.Equals(FVector(80, 0, 0)));
	TestTrue(TEXT("torn earth carried"), Mark.bEarth);
	TestFalse(TEXT("a jump past the break opens again, no mark across it"), Advance(Trail, FVector(800, 0, 0), Up, true, false, Mark));
	TestTrue(TEXT("restarted at the jump"), Trail.Last.Equals(FVector(800, 0, 0)));
	TestFalse(TEXT("off the grass closes it"), Advance(Trail, FVector(900, 0, 0), Up, false, false, Mark));
	TestFalse(TEXT("closed"), Trail.bOpen);
	TestFalse(TEXT("back on grass only opens it"), Advance(Trail, FVector(980, 0, 0), Up, true, false, Mark));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexGrassMarksLayTest,
	"ApexSim.GrassMarks.Lay",
	ApexTestFlags)

bool FApexGrassMarksLayTest::RunTest(const FString& Parameters)
{
	using namespace ApexGrassMarks;
	FMark Mark;
	Mark.From = FVector(0, 0, 100);
	Mark.To = FVector(0, 70, 100);
	Mark.Normal = FVector::UpVector;
	const FTransform T = MarkTransform(Mark, 30.0f);
	TestTrue(TEXT("centred, lifted off the ground"), T.GetLocation().Equals(FVector(0, 35, 100 + LiftCm), 0.01));
	TestTrue(TEXT("its X along the mark"), T.GetRotation().GetForwardVector().Equals(FVector(0, 1, 0), 1e-3));
	TestTrue(TEXT("its Z the ground's normal"), T.GetRotation().GetUpVector().Equals(FVector::UpVector, 1e-3));
	TestTrue(TEXT("a little longer than the mark, a print wide"), FMath::IsNearlyEqual(T.GetScale3D().X, 0.8, 1e-3) && FMath::IsNearlyEqual(T.GetScale3D().Y, 0.3, 1e-3));

	// On a bank the plane lies in the slope.
	Mark.Normal = FVector(0.3, 0, 1).GetSafeNormal();
	const FTransform Banked = MarkTransform(Mark, 30.0f);
	TestTrue(TEXT("on a bank its Z is the slope's normal"), Banked.GetRotation().GetUpVector().Equals(Mark.Normal, 1e-3));

	TestEqual(TEXT("too slow to slip"), SlipDeg(FVector(100, 0, 0), FVector::ForwardVector), 0.0f);
	TestTrue(TEXT("straight on is no slip"), SlipDeg(FVector(3000, 0, 0), FVector::ForwardVector) < 0.1f);
	TestTrue(TEXT("sideways is ninety"), FMath::IsNearlyEqual(SlipDeg(FVector(0, 3000, 0), FVector::ForwardVector), 90.0f, 0.1f));
	TestFalse(TEXT("a car running straight flattens the grass"), TearsEarth(3.0f, 2.0f));
	TestFalse(TEXT("a hairpin's usual slip does not"), TearsEarth(9.0f, 0.0f));
	TestTrue(TEXT("a slide tears it"), TearsEarth(20.0f, 0.0f));
	TestTrue(TEXT("hard braking tears it"), TearsEarth(0.0f, 9.0f));

	const TArray<FColor> Pixels = MarkPixels(32);
	TestEqual(TEXT("a print four times as long as wide"), Pixels.Num(), 32 * 128);
	TestEqual(TEXT("clear at the edge"), static_cast<int32>(Pixels[0].A), 0);
	int32 Solid = 0;
	for (int32 X = 0; X < 128; ++X)
	{
		Solid += Pixels[16 * 128 + X].A > 102 ? 1 : 0;
	}
	TestTrue(TEXT("down the middle mostly past the decal's 0.4 clip, with gaps"), Solid > 64 && Solid < 128);

	FApexTrackMaterial Ground;
	Ground.Key = TEXT("ground");
	Ground.Family = TEXT("surface");
	TestTrue(TEXT("grass terrain takes marks"), Ground.TakesTyreMarks());
	Ground.GroundSet = TEXT("sand");
	TestFalse(TEXT("a desert does not"), Ground.TakesTyreMarks());
	FApexTrackMaterial Gravel;
	Gravel.Key = TEXT("surface_gravel");
	Gravel.Family = TEXT("surface");
	TestFalse(TEXT("gravel does not"), Gravel.TakesTyreMarks());
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
