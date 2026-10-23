#include "ApexTestCommon.h"
#include "Race/ApexPhotoMode.h"

#if WITH_DEV_AUTOMATION_TESTS

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPhotoLensTest, "ApexSim.Photo.Lens", ApexTestFlags)

bool FApexPhotoLensTest::RunTest(const FString& Parameters)
{
	using namespace ApexPhoto;

	// A 36 mm sensor: 18 mm is a right angle, 35 mm about 54 degrees.
	TestTrue(TEXT("18 mm is 90 degrees"), FMath::IsNearlyEqual(FovForFocalLength(18.0f), 90.0f, 0.01f));
	TestTrue(TEXT("35 mm is about 54.4 degrees"), FMath::IsNearlyEqual(FovForFocalLength(35.0f), 54.43f, 0.05f));
	TestTrue(TEXT("a field of view comes back as its focal length"), FMath::IsNearlyEqual(FocalLengthForFov(FovForFocalLength(85.0f)), 85.0f, 0.01f));
	TestEqual(TEXT("a fisheye's view clamps to the shortest lens"), FocalLengthForFov(170.0f), MinFocalLengthMm);

	// Steps: on a step to the next, between steps to the neighbour, held at the ends.
	TestEqual(TEXT("35 up is 50"), StepFocalLength(35.0f, 1), 50.0f);
	TestEqual(TEXT("37 down is 35"), StepFocalLength(37.0f, -1), 35.0f);
	TestEqual(TEXT("37 up is 50"), StepFocalLength(37.0f, 1), 50.0f);
	TestEqual(TEXT("the longest holds"), StepFocalLength(MaxFocalLengthMm, 1), MaxFocalLengthMm);
	TestEqual(TEXT("the shortest holds"), StepFocalLength(MinFocalLengthMm, -1), MinFocalLengthMm);
	TestEqual(TEXT("focus on the car, then a metre"), StepFocus(0.0f, 1), 1.0f);
	TestEqual(TEXT("a metre down is the car again"), StepFocus(1.0f, -1), 0.0f);
	TestEqual(TEXT("no blur, then f/1.4"), StepAperture(0.0f, 1), 1.4f);
	TestEqual(TEXT("f/2.8 stops down to f/4"), StepAperture(2.8f, 1), 4.0f);
	TestEqual(TEXT("f/1.4 opens to no blur"), StepAperture(1.4f, -1), 0.0f);

	// Exposure in thirds, there and back exactly, clamped.
	float Ev = 0.0f;
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Ev = StepExposure(Ev, 1);
	}
	TestTrue(TEXT("four thirds up"), FMath::IsNearlyEqual(Ev, 4.0f / 3.0f, 1.0e-4f));
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Ev = StepExposure(Ev, -1);
	}
	TestEqual(TEXT("and back to nothing"), Ev, 0.0f);
	TestEqual(TEXT("the top holds"), StepExposure(MaxExposureEv, 1), MaxExposureEv);

	TestEqual(TEXT("1x up is 2x"), StepResolution(1, 1), 2);
	TestEqual(TEXT("2x up is 4x"), StepResolution(2, 1), 4);
	TestEqual(TEXT("4x holds"), StepResolution(4, 1), 4);
	TestEqual(TEXT("2x down is 1x"), StepResolution(2, -1), 1);

	FLens Lens;
	Lens.FocalLengthMm = 85.0f;
	Lens.Fstop = 2.8f;
	Lens.ExposureEv = -2.0f / 3.0f;
	TestEqual(TEXT("focal length"), DescribeSetting(Lens, ESetting::FocalLength), FString(TEXT("85 mm")));
	TestEqual(TEXT("focus on the car"), DescribeSetting(Lens, ESetting::Focus), FString(TEXT("ON THE CAR")));
	TestEqual(TEXT("aperture"), DescribeSetting(Lens, ESetting::Aperture), FString(TEXT("f/2.8")));
	TestEqual(TEXT("exposure"), DescribeSetting(Lens, ESetting::Exposure), FString(TEXT("-0.7 EV")));
	TestEqual(TEXT("shot size"), DescribeSetting(Lens, ESetting::Resolution), FString(TEXT("2x SCREEN")));
	Lens.Fstop = 8.0f;
	TestEqual(TEXT("a whole f-number"), DescribeSetting(Lens, ESetting::Aperture), FString(TEXT("f/8")));

	StepSetting(Lens, ESetting::FocalLength, 1);
	TestEqual(TEXT("a row steps its own value"), Lens.FocalLengthMm, 105.0f);
	TestEqual(TEXT("the rows wrap downwards"), (int32)StepRow(ESetting::FocalLength, -1), (int32)ESetting::Resolution);
	TestEqual(TEXT("and upwards"), (int32)StepRow(ESetting::Resolution, 1), (int32)ESetting::FocalLength);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPhotoCameraTest, "ApexSim.Photo.Camera", ApexTestFlags)

bool FApexPhotoCameraTest::RunTest(const FString& Parameters)
{
	using namespace ApexPhoto;
	const FVector Pivot(0.0, 0.0, 50.0);

	// Free: forward is where it looks, up is the world's.
	{
		FCamera Camera = FreeAt(FVector(-1000.0, 0.0, 200.0), FRotator(0.0, 0.0, 0.0));
		FInput Input;
		Input.Move = FVector(1.0, 0.0, 1.0);
		Advance(Camera, Input, 1.0f, Pivot);
		TestTrue(TEXT("forward one second"), FMath::IsNearlyEqual(Camera.LocationCm.X, -1000.0 + MoveSpeedCmPerS, 0.1));
		TestTrue(TEXT("up one second"), FMath::IsNearlyEqual(Camera.LocationCm.Z, 200.0 + MoveSpeedCmPerS, 0.1));

		Input = FInput();
		Input.Move = FVector(0.0, 1.0, 0.0);
		Input.bFast = true;
		const FVector Before = Camera.LocationCm;
		Advance(Camera, Input, 0.1f, Pivot);
		TestTrue(TEXT("right is +Y facing +X, five times as fast held"),
			FMath::IsNearlyEqual(Camera.LocationCm.Y - Before.Y, MoveSpeedCmPerS * FastFactor * 0.1, 0.1));

		Input = FInput();
		Input.Look = FVector2D(1.0, 0.0);
		Input.LookDeltaDeg = FVector2D(0.0, 200.0);
		Advance(Camera, Input, 0.5f, Pivot);
		TestTrue(TEXT("the stick turns at its rate"), FMath::IsNearlyEqual(Camera.YawDeg, LookRateDegPerS * 0.5f, 0.01f));
		TestEqual(TEXT("the pitch stops short of straight up"), Camera.PitchDeg, MaxPitchDeg);
	}

	// Never further from the car than the range.
	{
		FCamera Camera = FreeAt(FVector(MaxRangeCm - 100.0, 0.0, 50.0), FRotator::ZeroRotator);
		FInput Input;
		Input.Move = FVector(1.0, 0.0, 0.0);
		Advance(Camera, Input, 2.0f, Pivot);
		TestTrue(TEXT("held at the range"), FMath::IsNearlyEqual((Camera.LocationCm - Pivot).Size(), (double)MaxRangeCm, 0.5));
	}

	// Orbit: taken from where it is, it keeps the distance and looks at the car.
	{
		FCamera Camera = FreeAt(FVector(-800.0, 0.0, 50.0), FRotator(0.0, 30.0, 0.0));
		SetOrbit(Camera, true, Pivot);
		TestTrue(TEXT("orbiting"), Camera.bOrbit);
		TestTrue(TEXT("it has not moved"), Camera.LocationCm.Equals(FVector(-800.0, 0.0, 50.0), 0.5));
		TestTrue(TEXT("it looks at the car"), FMath::IsNearlyEqual(Camera.YawDeg, 0.0f, 0.01f));

		FInput Input;
		Input.Look = FVector2D(1.0, 0.0);
		Advance(Camera, Input, 1.0f, Pivot);
		TestTrue(TEXT("the distance holds round the orbit"), FMath::IsNearlyEqual((Camera.LocationCm - Pivot).Size(), 800.0, 0.5));
		const FVector ToCar = (Pivot - Camera.LocationCm).GetSafeNormal();
		TestTrue(TEXT("still looking at the car"), ToCar.Equals(Rotation(Camera).Vector(), 1.0e-3));

		Input = FInput();
		Input.Move = FVector(1.0, 0.0, 0.0);
		Advance(Camera, Input, 1.0f, Pivot);
		TestTrue(TEXT("forward closes in"), (Camera.LocationCm - Pivot).Size() < 800.0);
		for (int32 Step = 0; Step < 50; ++Step)
		{
			Advance(Camera, Input, 1.0f, Pivot);
		}
		TestTrue(TEXT("never closer than the nearest"), FMath::IsNearlyEqual((Camera.LocationCm - Pivot).Size(), (double)MinOrbitCm, 0.5));

		SetOrbit(Camera, false, Pivot);
		TestFalse(TEXT("free again"), Camera.bOrbit);
	}

	// The ground.
	{
		FCamera Camera = FreeAt(FVector(0.0, 0.0, 10.0), FRotator::ZeroRotator);
		TestTrue(TEXT("lifted out of the ground"), KeepAboveGround(Camera, 0.0));
		TestEqual(TEXT("to the clearance"), Camera.LocationCm.Z, (double)GroundClearanceCm);
		TestFalse(TEXT("left alone above it"), KeepAboveGround(Camera, -100.0));
	}
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPhotoKeysTest, "ApexSim.Photo.Keys", ApexTestFlags)

bool FApexPhotoKeysTest::RunTest(const FString& Parameters)
{
	using namespace ApexPhoto;
	TestEqual(TEXT("Enter shoots"), (int32)CommandFor(EKeys::Enter), (int32)ECommand::TakeShot);
	TestEqual(TEXT("the pad's A shoots"), (int32)CommandFor(EKeys::Gamepad_FaceButton_Bottom), (int32)ECommand::TakeShot);
	TestEqual(TEXT("Escape leaves"), (int32)CommandFor(EKeys::Escape), (int32)ECommand::Exit);
	TestEqual(TEXT("P leaves, as it came in"), (int32)CommandFor(EKeys::P), (int32)ECommand::Exit);
	TestEqual(TEXT("the pad's B leaves"), (int32)CommandFor(EKeys::Gamepad_FaceButton_Right), (int32)ECommand::Exit);
	TestEqual(TEXT("D-pad down picks the next row"), (int32)CommandFor(EKeys::Gamepad_DPad_Down), (int32)ECommand::NextRow);
	TestEqual(TEXT("right steps the value up"), (int32)CommandFor(EKeys::Right), (int32)ECommand::Increase);
	TestEqual(TEXT("RB zooms in"), (int32)CommandFor(EKeys::Gamepad_RightShoulder), (int32)ECommand::ZoomIn);
	TestEqual(TEXT("Space pauses the replay"), (int32)CommandFor(EKeys::SpaceBar), (int32)ECommand::PlayPause);
	TestEqual(TEXT("W is no command"), (int32)CommandFor(EKeys::W), (int32)ECommand::None);

	FVector Move;
	FVector2D Look;
	TestTrue(TEXT("W is held"), HeldAxisFor(EKeys::W, Move, Look) && Move.X == 1.0);
	TestTrue(TEXT("Q goes down"), HeldAxisFor(EKeys::Q, Move, Look) && Move.Z == -1.0);
	TestTrue(TEXT("J looks left"), HeldAxisFor(EKeys::J, Move, Look) && Look.X == -1.0);
	TestFalse(TEXT("Enter is not held"), HeldAxisFor(EKeys::Enter, Move, Look));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexPhotoShotTest, "ApexSim.Photo.Shot", ApexTestFlags)

bool FApexPhotoShotTest::RunTest(const FString& Parameters)
{
	using namespace ApexPhoto;
	TestEqual(TEXT("1080p takes 4x"), FitMultiplier(FIntPoint(1920, 1080), 4), 4);
	TestEqual(TEXT("a 5K screen halves to 2x"), FitMultiplier(FIntPoint(5120, 2880), 4), 2);
	TestEqual(TEXT("never under 1x"), FitMultiplier(FIntPoint(20000, 1000), 4), 1);
	TestEqual(TEXT("0 is 1x"), FitMultiplier(FIntPoint(1920, 1080), 0), 1);

	const FDateTime When(2026, 10, 24, 21, 30, 5);
	TestEqual(TEXT("named by circuit, time and size"), MakeShotName(When, TEXT("Zandervoort"), 2),
		FString(TEXT("ApexSim_Zandervoort_20261024_213005_2x")));
	TestEqual(TEXT("odd characters dropped"), MakeShotName(When, TEXT("My Track/../x"), 1),
		FString(TEXT("ApexSim_MyTrackx_20261024_213005_1x")));
	TestEqual(TEXT("no circuit"), MakeShotName(When, FString(), 4), FString(TEXT("ApexSim_20261024_213005_4x")));
	TestTrue(TEXT("under Saved/Screenshots/ApexSim"), ShotFolder().EndsWith(TEXT("Screenshots/ApexSim")));
	return true;
}

#endif
