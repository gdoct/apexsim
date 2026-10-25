#include "ApexTestCommon.h"
#include "Race/ApexMarshals.h"
#include "Race/ApexTvDirector.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexMarshalsTest
{
	using namespace ApexMarshals;

	/** A round circuit of radius 250 m (server frame, turning left), as the TV tests use. */
	ApexTv::FPath Ring()
	{
		TArray<FVector2D> Centerline;
		constexpr double RadiusM = 250.0;
		for (int32 Index = 0; Index < 180; ++Index)
		{
			const double Theta = 2.0 * PI * Index / 180;
			Centerline.Add(FVector2D(RadiusM * FMath::Sin(Theta), RadiusM * (1.0 - FMath::Cos(Theta))));
		}
		ApexTv::FPath Path;
		Path.BuildFromServerCenterline(Centerline);
		return Path;
	}

	/** A car `Lateral` cm to the right of the centerline at `Station` cm. */
	FCarSample CarAt(const ApexTv::FPath& Path, int32 Index, double Station, double Lateral, float SpeedMps, bool bOnTrack)
	{
		FVector Point;
		FVector Tangent;
		Path.Sample(Station, Point, Tangent);
		FCarSample Car;
		Car.CarIndex = Index;
		Car.Location = Point + FVector(-Tangent.Y, Tangent.X, 0.0).GetSafeNormal() * Lateral;
		Car.YawDeg = static_cast<float>(Tangent.Rotation().Yaw);
		Car.SpeedMps = SpeedMps;
		Car.bOnTrack = bOnTrack;
		return Car;
	}

	void Run(FTracker& Tracker, TArray<FCarSample>& Cars, const ApexTv::FPath& Path, float Seconds, bool bRacing = true)
	{
		constexpr float Dt = 1.0f / 30.0f;
		for (float T = 0.0f; T < Seconds - 1.0e-4f; T += Dt)
		{
			Tracker.Tick(Cars, bRacing, Dt, &Path);
		}
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexMarshalsOffRoadTest, "ApexSim.Marshals.ACarOffTheRoadIsRecovered", ApexTestFlags)

bool FApexMarshalsOffRoadTest::RunTest(const FString& Parameters)
{
	using namespace ApexMarshals;
	using namespace ApexMarshalsTest;

	const ApexTv::FPath Path = Ring();
	FTracker Tracker;
	// One car racing past, one in the gravel 9 m right of the centerline.
	const double Station = 40000.0;
	TArray<FCarSample> Cars = {CarAt(Path, 0, 20000.0, 0.0, 60.0f, true), CarAt(Path, 1, Station, 900.0, 0.0f, false)};

	Run(Tracker, Cars, Path, OffRoadAfterS - 0.5f);
	TestEqual(TEXT("not at once: a car may just be pausing"), Tracker.GetIncidents().Num(), 0);
	Run(Tracker, Cars, Path, 1.0f);
	if (!TestEqual(TEXT("the marshals come out"), Tracker.GetIncidents().Num(), 1))
	{
		return false;
	}
	const FIncident Scene = Tracker.GetIncidents()[0];
	TestEqual(TEXT("for the stopped car"), Scene.CarIndex, 1);
	TestTrue(TEXT("waving"), Scene.IsWaving());
	TestFalse(TEXT("no truck yet"), Scene.HasTruck());

	// The post is up the road from the car, on its side, further out than it.
	const double PostStation = Path.StationAt(Scene.Post);
	TestTrue(FString::Printf(TEXT("the post is up the road (%.0f cm before the car)"), Station - PostStation),
		FMath::IsNearlyEqual(Station - PostStation, PostUpCm, 150.0));
	FVector Point;
	FVector Tangent;
	Path.Sample(PostStation, Point, Tangent);
	const double PostLateral = FVector::DotProduct(Scene.Post - Point, FVector(-Tangent.Y, Tangent.X, 0.0).GetSafeNormal());
	TestTrue(FString::Printf(TEXT("on the car's side, past it (%.0f cm out)"), PostLateral), PostLateral > 900.0 + PostOutCm - 50.0);
	TestTrue(TEXT("facing along the road"), FVector::DotProduct(Scene.Along, Tangent.GetSafeNormal2D()) > 0.99);

	// The recovery vehicle sets off from up the road and stops behind the car.
	Run(Tracker, Cars, Path, TruckAfterS);
	const FIncident& Arriving = Tracker.GetIncidents()[0];
	TestTrue(TEXT("the recovery vehicle sets off"), Arriving.HasTruck());
	TestTrue(TEXT("from up the road"), FVector::Dist2D(Arriving.TruckLocation(), Cars[1].Location) > 2000.0);
	Run(Tracker, Cars, Path, TruckDriveS + 0.5f);
	const FIncident& Arrived = Tracker.GetIncidents()[0];
	const double TruckGap = FVector::Dist2D(Arrived.TruckLocation(), Cars[1].Location);
	TestTrue(FString::Printf(TEXT("it stops just behind the car (%.0f cm)"), TruckGap), TruckGap > 400.0 && TruckGap < 1200.0);
	TestTrue(TEXT("pointing along the road"), FVector::DotProduct(FRotator(0.0, Arrived.TruckYawDeg(), 0.0).Vector(), Scene.Along) > 0.95);

	// Towed: the car is in its pit box, far away. The server counts a box
	// as outside the lane, so it is still not in the pits.
	Cars[1] = CarAt(Path, 1, 100.0, -1500.0, 0.0f, true);
	Cars[1].bOut = true;
	Run(Tracker, Cars, Path, 0.5f);
	TestEqual(TEXT("the scene stays a moment after the tow"), Tracker.GetIncidents().Num(), 1);
	TestFalse(TEXT("the flags are down"), Tracker.GetIncidents()[0].IsWaving());
	Run(Tracker, Cars, Path, LingerS);
	TestEqual(TEXT("then it clears"), Tracker.GetIncidents().Num(), 0);
	Run(Tracker, Cars, Path, 20.0f);
	TestEqual(TEXT("no one comes out to it in its box"), Tracker.GetIncidents().Num(), 0);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexMarshalsWhenTest, "ApexSim.Marshals.WhenTheyComeOut", ApexTestFlags)

bool FApexMarshalsWhenTest::RunTest(const FString& Parameters)
{
	using namespace ApexMarshals;
	using namespace ApexMarshalsTest;

	const ApexTv::FPath Path = Ring();

	// Stopped on the road: a stall or a spin takes longer to be news.
	{
		FTracker Tracker;
		TArray<FCarSample> Cars = {CarAt(Path, 0, 30000.0, 100.0, 0.2f, true)};
		Run(Tracker, Cars, Path, OnRoadAfterS - 0.5f);
		TestEqual(TEXT("on the road: not before OnRoadAfterS"), Tracker.GetIncidents().Num(), 0);
		Run(Tracker, Cars, Path, 1.0f);
		TestEqual(TEXT("on the road: then out"), Tracker.GetIncidents().Num(), 1);

		// It drives on.
		Cars[0].SpeedMps = 12.0f;
		Run(Tracker, Cars, Path, 0.2f);
		TestFalse(TEXT("driving on: the flags go down"), Tracker.GetIncidents().Num() == 1 && Tracker.GetIncidents()[0].IsWaving());
		Run(Tracker, Cars, Path, LingerS);
		TestEqual(TEXT("driving on: cleared"), Tracker.GetIncidents().Num(), 0);
	}

	// Out of the race on the road: as soon as one off it.
	{
		FTracker Tracker;
		TArray<FCarSample> Cars = {CarAt(Path, 0, 30000.0, 0.0, 0.0f, true)};
		Cars[0].bOut = true;
		Run(Tracker, Cars, Path, OffRoadAfterS + 0.2f);
		TestEqual(TEXT("out on the road: marshals"), Tracker.GetIncidents().Num(), 1);
	}

	// Slow but moving, or stopped in the pits: never.
	{
		FTracker Tracker;
		TArray<FCarSample> Cars = {CarAt(Path, 0, 30000.0, 900.0, 3.0f, false), CarAt(Path, 1, 100.0, -1500.0, 0.0f, true)};
		Cars[1].bInPits = true;
		Run(Tracker, Cars, Path, 20.0f);
		TestEqual(TEXT("a crawl off the road, or the pits: nothing"), Tracker.GetIncidents().Num(), 0);
	}

	// Not racing (the grid, a finished race) clears everything.
	{
		FTracker Tracker;
		TArray<FCarSample> Cars = {CarAt(Path, 0, 30000.0, 900.0, 0.0f, false)};
		Run(Tracker, Cars, Path, 3.0f);
		TestEqual(TEXT("racing: out"), Tracker.GetIncidents().Num(), 1);
		Run(Tracker, Cars, Path, 0.1f, /*bRacing*/ false);
		TestEqual(TEXT("the race over: gone"), Tracker.GetIncidents().Num(), 0);
	}

	// A few at most: the newest stops are the ones drawn.
	{
		FTracker Tracker;
		TArray<FCarSample> Cars;
		for (int32 Index = 0; Index < MaxIncidents + 2; ++Index)
		{
			Cars.Add(CarAt(Path, Index, 10000.0 + Index * 20000.0, 900.0, 0.0f, false));
			Run(Tracker, Cars, Path, OffRoadAfterS + 0.5f);
		}
		TestEqual(TEXT("at most MaxIncidents"), Tracker.GetIncidents().Num(), MaxIncidents);
		TestTrue(TEXT("the newest is kept"),
			Tracker.GetIncidents().ContainsByPredicate([&](const FIncident& S) { return S.CarIndex == MaxIncidents + 1; }));
		TestFalse(TEXT("the oldest went"),
			Tracker.GetIncidents().ContainsByPredicate([](const FIncident& S) { return S.CarIndex == 0; }));
	}
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
