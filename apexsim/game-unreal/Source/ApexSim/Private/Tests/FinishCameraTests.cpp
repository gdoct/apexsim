#include "ApexTestCommon.h"
#include "Race/ApexFinishCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexFinishCamTest
{
	using namespace ApexFinishCam;

	constexpr float Step = 1.0f / 60.0f;

	/** Flat ground at `Z`, nothing in the way. */
	ApexTv::FWorldQueries Ground(double Z = 0.0)
	{
		ApexTv::FWorldQueries World;
		World.GroundZ = [Z](const FVector&, double& OutZ)
		{
			OutZ = Z;
			return true;
		};
		return World;
	}

	/** A car going round a 200 m circle about the origin at 40 m/s, anticlockwise; `T` in seconds. */
	void CarOnCircle(float T, FVector& OutLocation, FRotator& OutRotation)
	{
		constexpr double Radius = 20000.0;
		constexpr double Speed = 4000.0;
		const double Theta = Speed * T / Radius;
		OutLocation = FVector(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), 0.0);
		OutRotation = FRotator(0.0, FMath::RadiansToDegrees(Theta) + 90.0, 0.0);
	}

	/** The chase camera's place on a car: 6 m back, 2 m up, looking at it. */
	void ChasePose(const FVector& Car, const FRotator& CarRotation, FVector& OutEye, FRotator& OutRotation)
	{
		OutEye = Car + CarRotation.RotateVector(FVector(-600.0, 0.0, 200.0));
		OutRotation = (Car - OutEye).Rotation();
	}

	double BearingOffNose(const FVector& Eye, const FVector& Car, const FRotator& CarRotation)
	{
		const FVector Rel = Eye - Car;
		return FMath::FindDeltaAngleDegrees(CarRotation.Yaw, FMath::RadiansToDegrees(FMath::Atan2(Rel.Y, Rel.X)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexFinishCamZoomsOutTest, "ApexSim.FinishCam.ZoomsOutBehindTheCar", ApexTestFlags)

bool FApexFinishCamZoomsOutTest::RunTest(const FString& Parameters)
{
	using namespace ApexFinishCamTest;
	FMove Move;
	FVector Car;
	FRotator CarRotation;
	CarOnCircle(0.0f, Car, CarRotation);
	FVector Eye;
	FRotator EyeRotation;
	ChasePose(Car, CarRotation, Eye, EyeRotation);
	Move.Start(Eye, EyeRotation, 75.0f, Car, CarRotation);

	const ApexTv::FWorldQueries World = Ground();
	double LastDistance = 0.0;
	bool bAlwaysOut = true;
	ApexTv::FPose Pose;
	float T = 0.0f;
	while (T + Step <= Move.Tuning().ZoomSeconds)
	{
		T += Step;
		CarOnCircle(T, Car, CarRotation);
		Pose = Move.Tick(Car, CarRotation, Step, World);
		const double Distance = FVector::Dist(Pose.Location, Car);
		bAlwaysOut &= Distance >= LastDistance - 1.0;
		LastDistance = Distance;
	}
	TestEqual(TEXT("still zooming out"), Move.GetPhase(), EPhase::ZoomOut);
	TestTrue(TEXT("the camera only ever moves away from the car"), bAlwaysOut);
	TestTrue(TEXT("well back by the end of the zoom"), FVector::Dist2D(Pose.Location, Car) > 0.85 * Move.Tuning().ZoomDistanceCm);
	TestTrue(TEXT("and up"), Pose.Location.Z - Car.Z > 0.85 * Move.Tuning().ZoomHeightCm);
	TestTrue(TEXT("behind the car, give or take its turning"), FMath::Abs(BearingOffNose(Pose.Location, Car, CarRotation)) > 150.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexFinishCamPanoramaTest, "ApexSim.FinishCam.RisesIntoAPanorama", ApexTestFlags)

bool FApexFinishCamPanoramaTest::RunTest(const FString& Parameters)
{
	using namespace ApexFinishCamTest;
	FMove Move;
	FVector Car;
	FRotator CarRotation;
	CarOnCircle(0.0f, Car, CarRotation);
	FVector Eye;
	FRotator EyeRotation;
	ChasePose(Car, CarRotation, Eye, EyeRotation);
	Move.Start(Eye, EyeRotation, 75.0f, Car, CarRotation);

	const ApexTv::FWorldQueries World = Ground();
	const float Settle = Move.Tuning().ZoomSeconds + Move.Tuning().PanoramaSeconds + 0.5f;
	FVector LastRel = FVector::ZeroVector;
	double WorstJumpCm = 0.0;
	double WorstAimDeg = 0.0;
	float T = 0.0f;
	auto RunUntil = [&](float Until)
	{
		ApexTv::FPose Pose;
		while (T + Step * 0.5f < Until)
		{
			T += Step;
			CarOnCircle(T, Car, CarRotation);
			Pose = Move.Tick(Car, CarRotation, Step, World);
			// The camera rides the car; what must not jump is where it sits about it.
			const FVector Rel = Pose.Location - Car;
			if (T > 1.5f * Step)
			{
				WorstJumpCm = FMath::Max(WorstJumpCm, FVector::Dist(Rel, LastRel));
			}
			LastRel = Rel;
			if (T > Move.Tuning().AimSeconds)
			{
				const FVector ToCar = (Car + FVector(0.0, 0.0, 100.0) - Pose.Location).GetSafeNormal();
				WorstAimDeg = FMath::Max(WorstAimDeg,
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Pose.Rotation.Vector() | ToCar, -1.0, 1.0))));
			}
		}
		return Pose;
	};

	const ApexTv::FPose Pose = RunUntil(Settle);
	TestEqual(TEXT("in the panorama"), Move.GetPhase(), EPhase::Panorama);
	TestNearlyEqual(TEXT("orbit distance"), FVector::Dist2D(Pose.Location, Car), Move.Tuning().OrbitDistanceCm, 50.0);
	TestNearlyEqual(TEXT("orbit height"), Pose.Location.Z - Car.Z, Move.Tuning().OrbitHeightCm, 50.0);
	TestNearlyEqual(TEXT("panorama lens"), static_cast<double>(Pose.FovDeg), static_cast<double>(Move.Tuning().PanoramaFovDeg), 0.1);
	RunUntil(Settle + 10.0f);
	AddInfo(FString::Printf(TEXT("worst frame-to-frame jump %.1f cm, worst aim %.2f deg"), WorstJumpCm, WorstAimDeg));
	TestTrue(TEXT("no jump anywhere in the move (under 1 m a frame)"), WorstJumpCm < 100.0);
	TestTrue(TEXT("aimed at the car throughout"), WorstAimDeg < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexFinishCamOrbitRateTest, "ApexSim.FinishCam.OrbitsInTheWorldFrame", ApexTestFlags)

bool FApexFinishCamOrbitRateTest::RunTest(const FString& Parameters)
{
	using namespace ApexFinishCamTest;
	FMove Move;
	FVector Car;
	FRotator CarRotation;
	CarOnCircle(0.0f, Car, CarRotation);
	Move.Start(FVector::ZeroVector, FRotator::ZeroRotator, 0.0f, Car, CarRotation);
	const ApexTv::FWorldQueries World = Ground();

	const float Settle = Move.Tuning().ZoomSeconds + Move.Tuning().PanoramaSeconds + 0.5f;
	float T = 0.0f;
	auto WorldBearingAt = [&](float Until)
	{
		ApexTv::FPose Pose;
		while (T + Step * 0.5f < Until)
		{
			T += Step;
			CarOnCircle(T, Car, CarRotation);
			Pose = Move.Tick(Car, CarRotation, Step, World);
		}
		const FVector Rel = Pose.Location - Car;
		return FMath::RadiansToDegrees(FMath::Atan2(Rel.Y, Rel.X));
	};
	const double From = WorldBearingAt(Settle);
	const double To = WorldBearingAt(Settle + 5.0f);
	TestNearlyEqual(TEXT("five seconds of orbit, not of the car's turning"),
		FMath::FindDeltaAngleDegrees(From, To), 5.0 * Move.Tuning().OrbitDegPerSecond, 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexFinishCamFarStartTest, "ApexSim.FinishCam.FarCameraStartsBehindTheCar", ApexTestFlags)

bool FApexFinishCamFarStartTest::RunTest(const FString& Parameters)
{
	using namespace ApexFinishCamTest;
	FMove Move;
	const FVector Car(1000.0, 2000.0, 300.0);
	const FRotator CarRotation(0.0, 30.0, 0.0);
	// A broadcast camera on another car, a kilometre away.
	Move.Start(Car + FVector(100000.0, 0.0, 5000.0), FRotator(-10.0, 180.0, 0.0), 30.0f, Car, CarRotation);
	const ApexTv::FPose Pose = Move.Tick(Car, CarRotation, Step, Ground());
	TestTrue(TEXT("next to the car, not flying in from far away"), FVector::Dist(Pose.Location, Car) < 1000.0);
	TestTrue(TEXT("behind it"), FMath::Abs(BearingOffNose(Pose.Location, Car, CarRotation)) > 170.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexFinishCamGroundTest, "ApexSim.FinishCam.ClearsRisingGround", ApexTestFlags)

bool FApexFinishCamGroundTest::RunTest(const FString& Parameters)
{
	using namespace ApexFinishCamTest;
	FMove Move;
	FVector Car;
	FRotator CarRotation;
	CarOnCircle(0.0f, Car, CarRotation);
	FVector Eye;
	FRotator EyeRotation;
	ChasePose(Car, CarRotation, Eye, EyeRotation);
	Move.Start(Eye, EyeRotation, 75.0f, Car, CarRotation);

	// A hillside 40 m above the road wherever the camera goes.
	constexpr double HillZ = 4000.0;
	const ApexTv::FWorldQueries World = Ground(HillZ);
	bool bAbove = true;
	for (float T = Step; T < 12.0f; T += Step)
	{
		CarOnCircle(T, Car, CarRotation);
		const ApexTv::FPose Pose = Move.Tick(Car, CarRotation, Step, World);
		bAbove &= Pose.Location.Z >= HillZ;
	}
	TestTrue(TEXT("never under the ground"), bAbove);
	return true;
}

#endif
