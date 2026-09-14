#include "ApexTestCommon.h"
#include "Race/ApexRaceCoordinate.h"
#include "Race/ApexTvDirector.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexTvTest
{
	using namespace ApexTv;

	/** A round circuit, radius R metres, starting at the origin heading +X and turning left (server frame). */
	struct FTvRing
	{
		double RadiusM = 250.0;

		TArray<FVector2D> Centerline(int32 Points = 180) const
		{
			TArray<FVector2D> Out;
			for (int32 Index = 0; Index < Points; ++Index)
			{
				const double Theta = 2.0 * PI * Index / Points;
				Out.Add(FVector2D(RadiusM * FMath::Sin(Theta), RadiusM * (1.0 - FMath::Cos(Theta))));
			}
			return Out;
		}

		double LengthM() const { return 2.0 * PI * RadiusM; }

		/** A car `DistanceM` round the lap, moving at `SpeedMps`, in Unreal space. */
		FCar Car(int32 Index, double DistanceM, double SpeedMps) const
		{
			const double Theta = DistanceM / RadiusM;
			FCar Car;
			Car.CarIndex = Index;
			Car.Location = ApexRace::ServerToUnrealPosition(
				FVector(RadiusM * FMath::Sin(Theta), RadiusM * (1.0 - FMath::Cos(Theta)), 0.0));
			const FVector Direction(FMath::Cos(Theta), -FMath::Sin(Theta), 0.0);
			Car.Rotation = Direction.Rotation();
			Car.Velocity = Direction * SpeedMps * 100.0;
			Car.RaceDistanceM = static_cast<float>(DistanceM);
			return Car;
		}
	};

	/** Six cars nose to tail round the ring; `Stopped` parks one where it is. */
	struct FTvField
	{
		FTvRing Ring;
		TArray<double> Distances;
		TArray<double> Speeds;
		TArray<FCar> Cars;

		explicit FTvField(int32 Count = 6)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				// A pair racing at the front, then gaps opening down the field.
				Distances.Add(-Index * (Index < 2 ? 9.0 : 30.0 + Index * 5.0));
				Speeds.Add(50.0 + Index);
			}
			Build();
		}

		void Step(double DeltaSeconds, bool bMoving)
		{
			for (int32 Index = 0; Index < Distances.Num(); ++Index)
			{
				if (bMoving)
				{
					Distances[Index] += Speeds[Index] * DeltaSeconds;
				}
			}
			Build(bMoving);
		}

		void Build(bool bMoving = false)
		{
			Cars.Reset();
			for (int32 Index = 0; Index < Distances.Num(); ++Index)
			{
				Cars.Add(Ring.Car(Index, Distances[Index], bMoving ? Speeds[Index] : 0.0));
			}
		}
	};

	bool IsFinitePose(const ApexTv::FPose& Pose)
	{
		return !Pose.Location.ContainsNaN() && !Pose.Rotation.ContainsNaN() && FMath::IsFinite(Pose.FovDeg);
	}

	/** Angle between where the camera looks and where the car is, degrees. */
	double OffAxisDeg(const ApexTv::FPose& Pose, const FCar& Car)
	{
		const FVector ToCar = (Car.Centre() - Pose.Location).GetSafeNormal();
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Pose.Rotation.Vector(), ToCar), -1.0, 1.0)));
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvFramingTest, "ApexSim.Tv.Framing", ApexTestFlags)

bool FApexTvFramingTest::RunTest(const FString& Parameters)
{
	using ApexTv::FramingFovDeg;
	// A 4.8 m car filling 30% of the frame 100 m away: 16 m of width, 9.15 degrees.
	TestNearlyEqual(TEXT("long lens at 100 m"), FramingFovDeg(480.0f, 10000.0f, 0.3f, 1.0f, 170.0f), 9.15f, 0.05f);
	TestTrue(TEXT("further away is a longer lens"),
		FramingFovDeg(480.0f, 20000.0f, 0.3f, 1.0f, 170.0f) < FramingFovDeg(480.0f, 10000.0f, 0.3f, 1.0f, 170.0f));
	TestTrue(TEXT("a bigger share of the frame is a longer lens"),
		FramingFovDeg(480.0f, 5000.0f, 0.5f, 1.0f, 170.0f) < FramingFovDeg(480.0f, 5000.0f, 0.2f, 1.0f, 170.0f));
	TestEqual(TEXT("clamped to the widest lens"), FramingFovDeg(480.0f, 10.0f, 0.3f, 7.0f, 55.0f), 55.0f);
	TestEqual(TEXT("clamped to the longest lens"), FramingFovDeg(480.0f, 1.0e6f, 0.3f, 7.0f, 55.0f), 7.0f);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvPathTest, "ApexSim.Tv.Path", ApexTestFlags)

bool FApexTvPathTest::RunTest(const FString& Parameters)
{
	// A 100 m square, anticlockwise as the server sees it: +X, then left.
	ApexTv::FPath Path;
	Path.BuildFromServerCenterline({ FVector2D(0, 0), FVector2D(100, 0), FVector2D(100, 100), FVector2D(0, 100), FVector2D(0, 0) });
	TestTrue(TEXT("valid"), Path.IsValid());
	TestEqual(TEXT("the repeated closing point is dropped"), Path.Points.Num(), 4);
	TestNearlyEqual(TEXT("length"), Path.LengthCm, 40000.0, 1.0);

	TestNearlyEqual(TEXT("station half way down the first side"), Path.StationAt(FVector(5000.0, 300.0, 50.0)), 5000.0, 1.0);

	FVector Point;
	FVector Tangent;
	Path.Sample(Path.LengthCm + 2500.0, Point, Tangent);
	TestTrue(TEXT("a station past the line wraps"), Point.Equals(FVector(2500.0, 0.0, 0.0), 1.0));
	TestTrue(TEXT("tangent along the first side"), Tangent.Equals(FVector::ForwardVector, 1e-3));

	// Server +Y is Unreal -Y: a left turn is towards Unreal's left, negative.
	TestTrue(TEXT("the corner turns left"), Path.TurnDeg(10000.0, 2000.0) < -45.0);
	TestNearlyEqual(TEXT("a straight does not turn"), Path.TurnDeg(5000.0, 2000.0), 0.0, 1e-3);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvPickTargetTest, "ApexSim.Tv.PickTarget", ApexTestFlags)

bool FApexTvPickTargetTest::RunTest(const FString& Parameters)
{
	using namespace ApexTv;
	TArray<FCar> Cars;
	auto Add = [&Cars](int32 Index, float Distance) {
		FCar& Car = Cars.AddDefaulted_GetRef();
		Car.CarIndex = Index;
		Car.RaceDistanceM = Distance;
	};
	Add(10, 1000.0f); // alone in the lead
	Add(11, 505.0f);  // a battle...
	Add(12, 500.0f);  // ...for second
	Add(13, 200.0f);  // alone at the back

	const TSet<int32> Nobody;
	int32 Battles = 0;
	for (int32 Seed = 0; Seed < 40; ++Seed)
	{
		FRandomStream Rng(Seed);
		const int32 Picked = PickTarget(Cars, INDEX_NONE, 0, Nobody, Rng);
		Battles += Picked == 1 || Picked == 2;
	}
	TestEqual(TEXT("a battle beats a lone leader"), Battles, 40);

	const TSet<int32> BackMarkerOff = { 13 };
	FRandomStream Rng(7);
	TestEqual(TEXT("a car in trouble beats a battle"), PickTarget(Cars, INDEX_NONE, 0, BackMarkerOff, Rng), 3);
	TestNotEqual(TEXT("an incident gets a couple of shots, not the rest of the race"), PickTarget(Cars, 13, 2, BackMarkerOff, Rng), 3);

	// Kept for a few shots, then let go of.
	int32 Stayed = 0;
	int32 LetGo = 0;
	for (int32 Seed = 0; Seed < 40; ++Seed)
	{
		FRandomStream Kept(Seed);
		Stayed += PickTarget(Cars, 11, 1, Nobody, Kept) == 1;
		FRandomStream Tired(Seed);
		LetGo += PickTarget(Cars, 11, 5, Nobody, Tired) != 1;
	}
	TestEqual(TEXT("a car just cut to is kept over its rival"), Stayed, 40);
	TestEqual(TEXT("a car watched for five shots is let go"), LetGo, 40);

	TestEqual(TEXT("an empty field has no target"), PickTarget(TArray<FCar>(), INDEX_NONE, 0, Nobody, Rng), static_cast<int32>(INDEX_NONE));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvCuttingTest, "ApexSim.Tv.CutsLikeTelevision", ApexTestFlags)

bool FApexTvCuttingTest::RunTest(const FString& Parameters)
{
	using namespace ApexTv;
	using namespace ApexTvTest;

	FTvField Field;
	FDirector Director;
	Director.Reset(1234);
	Director.SetPath(Field.Ring.Centerline());
	const FWorldQueries Flat;

	constexpr double Dt = 1.0 / 30.0;
	constexpr double GridSeconds = 6.0;
	TArray<EShot> Sequence;
	TArray<float> Lengths;
	int32 LastCuts = 0;
	float LastAge = 0.0f;
	int32 Framed = 0;
	int32 Judged = 0;
	bool bGridOnlyOnGrid = true;
	bool bAllFinite = true;
	bool bLensesSane = true;

	for (int32 Frame = 0; Frame < static_cast<int32>(240.0 / Dt); ++Frame)
	{
		const double Time = Frame * Dt;
		const bool bCountdown = Time < GridSeconds;
		Field.Step(Dt, !bCountdown);

		ApexTv::FPose Pose;
		if (!Director.Tick(Field.Cars, bCountdown, Dt, static_cast<float>(Time), Flat, Pose))
		{
			AddError(TEXT("the director had nothing to film with a full field"));
			return false;
		}
		bAllFinite &= IsFinitePose(Pose);
		bLensesSane &= Pose.FovDeg >= 5.0f && Pose.FovDeg <= 100.0f;

		if (Director.GetCutCount() != LastCuts)
		{
			if (Sequence.Num() > 0)
			{
				Lengths.Add(LastAge);
			}
			Sequence.Add(Director.GetShot());
			LastCuts = Director.GetCutCount();
		}
		LastAge = Director.GetShotAge();
		bGridOnlyOnGrid &= bCountdown == (Director.GetShot() == EShot::Grid);

		// Once a shot has settled, its car is in the picture.
		const FCar* Target = Field.Cars.FindByPredicate([&Director](const FCar& C) { return C.CarIndex == Director.GetTargetCarIndex(); });
		if (Target && Director.GetShotAge() > 0.6f && Director.GetShot() != EShot::Grid
			&& Director.GetShot() != EShot::Onboard && Director.GetShot() != EShot::Nose)
		{
			++Judged;
			Framed += OffAxisDeg(Pose, *Target) < Pose.FovDeg * 0.5 + 2.0;
		}
	}

	TestTrue(TEXT("every pose is finite"), bAllFinite);
	TestTrue(TEXT("lenses stay between 5 and 100 degrees"), bLensesSane);
	TestTrue(TEXT("the grid shot is used on the grid and only there"), bGridOnlyOnGrid);

	const int32 FirstRace = Sequence.IndexOfByPredicate([](EShot S) { return S != EShot::Grid; });
	TestTrue(TEXT("the lights go out on the helicopter"), FirstRace != INDEX_NONE && Sequence[FirstRace] == EShot::Helicopter);

	bool bRepeats = false;
	TSet<EShot> Kinds;
	for (int32 Index = FirstRace + 1; Index < Sequence.Num(); ++Index)
	{
		bRepeats |= Sequence[Index] == Sequence[Index - 1];
		Kinds.Add(Sequence[Index]);
	}
	TestFalse(TEXT("never the same angle twice running"), bRepeats);
	TestTrue(FString::Printf(TEXT("a varied cut (%d kinds of shot)"), Kinds.Num()), Kinds.Num() >= 5);

	double Total = 0.0;
	for (float Length : Lengths)
	{
		Total += Length;
	}
	const double Average = Lengths.Num() > 0 ? Total / Lengths.Num() : 0.0;
	TestTrue(FString::Printf(TEXT("shots held like coverage, not a slideshow (%.1f s average over %d)"), Average, Lengths.Num()),
		Average > 3.0 && Average < 12.0);

	const double FramedShare = Judged > 0 ? static_cast<double>(Framed) / Judged : 0.0;
	TestTrue(FString::Printf(TEXT("the car is in the frame (%.1f%% of settled frames)"), FramedShare * 100.0), FramedShare > 0.95);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvTracksideTest, "ApexSim.Tv.TracksideStandsAheadOffTheRoad", ApexTestFlags)

bool FApexTvTracksideTest::RunTest(const FString& Parameters)
{
	using namespace ApexTv;
	using namespace ApexTvTest;

	FTvField Field(1);
	FDirector Director;
	Director.Reset(99);
	Director.SetPath(Field.Ring.Centerline());
	Director.ForceShot(EShot::Trackside);
	const FWorldQueries Flat;

	constexpr double Dt = 1.0 / 30.0;
	ApexTv::FPose Pose;
	Field.Step(Dt, true);
	TestTrue(TEXT("filmed"), Director.Tick(Field.Cars, false, Dt, 0.0f, Flat, Pose));
	TestTrue(TEXT("on the trackside shot"), Director.GetShot() == EShot::Trackside);

	const FPath& Path = Director.GetPath();
	const double CameraStation = Path.StationAt(Pose.Location);
	FVector OnRoad;
	FVector Tangent;
	Path.Sample(CameraStation, OnRoad, Tangent);
	const double OffRoadM = FVector::Dist2D(OnRoad, Pose.Location) / 100.0;
	const double AheadM = FMath::Fmod(CameraStation - Path.StationAt(Field.Cars[0].Location) + Path.LengthCm, Path.LengthCm) / 100.0;
	TestTrue(FString::Printf(TEXT("stands off the road (%.1f m)"), OffRoadM), OffRoadM > 12.0 && OffRoadM < 30.0);
	TestTrue(FString::Printf(TEXT("stands ahead of the car (%.0f m)"), AheadM), AheadM > 30.0 && AheadM < 260.0);
	// The ring bends to Unreal's left, so its outside is to the right of the road.
	TestTrue(TEXT("on the outside of the bend"), (Pose.Location - OnRoad).Dot(FVector(-Tangent.Y, Tangent.X, 0.0)) > 0.0);

	// The camera does not move while the car goes past, and lets go once it has.
	const FVector Stand = Pose.Location;
	bool bStood = true;
	Director.ForceShot(EShot::None);
	int32 Frames = 0;
	for (; Frames < static_cast<int32>(15.0 / Dt) && Director.GetCutCount() == 1; ++Frames)
	{
		Field.Step(Dt, true);
		Director.Tick(Field.Cars, false, Dt, static_cast<float>(Frames * Dt), Flat, Pose);
		if (Director.GetCutCount() == 1)
		{
			bStood &= Pose.Location.Equals(Stand, 1.0);
		}
	}
	TestTrue(TEXT("a tripod stays where it was put"), bStood);
	TestTrue(FString::Printf(TEXT("cuts away after the car has passed (%.1f s)"), Frames * Dt), Director.GetCutCount() > 1 && Frames * Dt < 12.0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvBlockedTest, "ApexSim.Tv.CutsAwayFromABlockedView", ApexTestFlags)

bool FApexTvBlockedTest::RunTest(const FString& Parameters)
{
	using namespace ApexTv;
	using namespace ApexTvTest;

	FTvField Field(3);
	FDirector Director;
	Director.Reset(5);
	Director.SetPath(Field.Ring.Centerline());

	// A wall between every camera and every car.
	FWorldQueries Walled;
	Walled.IsClear = [](const FVector&, const FVector&) { return false; };

	constexpr double Dt = 1.0 / 30.0;
	ApexTv::FPose Pose;
	int32 LastCuts = 0;
	EShot LastShot = EShot::None;
	float LastAge = 0.0f;
	int32 BlockedShots = 0;
	float LongestBlocked = 0.0f;
	for (int32 Frame = 0; Frame < static_cast<int32>(20.0 / Dt); ++Frame)
	{
		Field.Step(Dt, true);
		Director.Tick(Field.Cars, false, Dt, static_cast<float>(Frame * Dt), Walled, Pose);
		if (Director.GetCutCount() != LastCuts)
		{
			if (LastShot != EShot::None && LastShot != EShot::Onboard && LastShot != EShot::Nose)
			{
				++BlockedShots;
				LongestBlocked = FMath::Max(LongestBlocked, LastAge);
			}
			LastCuts = Director.GetCutCount();
		}
		LastShot = Director.GetShot();
		LastAge = Director.GetShotAge();
	}
	TestTrue(FString::Printf(TEXT("blocked shots were cut (%d of them)"), BlockedShots), BlockedShots >= 3);
	TestTrue(FString::Printf(TEXT("each soon after it is blocked (longest %.2f s)"), LongestBlocked),
		LongestBlocked < FDirector::BlockedCutSeconds + 0.1f);

	// The onboard cameras ride the car: nothing is between them and it.
	Director.ForceShot(EShot::Onboard);
	Field.Step(Dt, true);
	Director.Tick(Field.Cars, false, Dt, 3.0f, Walled, Pose);
	const int32 CutsAtOnboard = Director.GetCutCount();
	for (int32 Frame = 0; Frame < static_cast<int32>(2.0 / Dt); ++Frame)
	{
		Field.Step(Dt, true);
		Director.Tick(Field.Cars, false, Dt, static_cast<float>(3.0 + Frame * Dt), Walled, Pose);
	}
	TestEqual(TEXT("the onboard camera is not cut for a wall"), Director.GetCutCount(), CutsAtOnboard);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexTvIncidentTest, "ApexSim.Tv.GoesToACarInTrouble", ApexTestFlags)

bool FApexTvIncidentTest::RunTest(const FString& Parameters)
{
	using namespace ApexTv;
	using namespace ApexTvTest;

	FTvField Field;
	FDirector Director;
	Director.Reset(77);
	Director.SetPath(Field.Ring.Centerline());
	const FWorldQueries Flat;

	constexpr double Dt = 1.0 / 30.0;
	constexpr int32 Stricken = 4;
	double FoundAfter = -1.0;
	ApexTv::FPose Pose;
	for (int32 Frame = 0; Frame < static_cast<int32>(40.0 / Dt); ++Frame)
	{
		const double Time = Frame * Dt;
		if (Time > 20.0)
		{
			// Spun and parked.
			Field.Speeds[Stricken] = 0.0;
		}
		Field.Step(Dt, true);
		Director.Tick(Field.Cars, false, Dt, static_cast<float>(Time), Flat, Pose);
		if (Time > 20.0 && FoundAfter < 0.0 && Director.GetTargetCarIndex() == Stricken)
		{
			FoundAfter = Time - 20.0;
		}
	}
	TestTrue(FString::Printf(TEXT("the director goes to the stopped car (after %.1f s)"), FoundAfter), FoundAfter >= 0.0 && FoundAfter < 8.0);
	return true;
}

#endif
