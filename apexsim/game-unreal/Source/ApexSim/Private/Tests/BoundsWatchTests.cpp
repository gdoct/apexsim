#include "ApexTestCommon.h"
#include "ApexBoundsWatch.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexBoundsWatchProblemTest, "ApexSim.BoundsWatch.Problem", ApexTestFlags)

bool FApexBoundsWatchProblemTest::RunTest(const FString& Parameters)
{
	using namespace ApexBoundsWatch;
	const FBoxSphereBounds Car(FVector(120000.0, -40000.0, 300.0), FVector(280.0, 95.0, 50.0), 300.0);
	TestTrue(TEXT("a car is fine"), Problem(Car).IsEmpty());
	// An 8 km DEM horizon tile, kilometres out: large, not wrong.
	const FBoxSphereBounds Horizon(FVector(600000.0, 0.0, 0.0), FVector(400000.0, 400000.0, 20000.0), 566000.0);
	TestTrue(TEXT("a horizon tile is fine"), Problem(Horizon).IsEmpty());

	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TestEqual(TEXT("NaN origin"), Problem(FBoxSphereBounds(FVector(NaN, 0.0, 0.0), FVector(1.0), 1.0)), FString(TEXT("NaN or infinite bounds")));
	TestEqual(TEXT("NaN radius"), Problem(FBoxSphereBounds(FVector::ZeroVector, FVector(1.0), NaN)), FString(TEXT("NaN or infinite bounds")));
	const double Inf = std::numeric_limits<double>::infinity();
	TestEqual(TEXT("infinite extent"), Problem(FBoxSphereBounds(FVector::ZeroVector, FVector(Inf, 1.0, 1.0), 1.0)),
		FString(TEXT("NaN or infinite bounds")));
	TestFalse(TEXT("a 30 km radius"), Problem(FBoxSphereBounds(FVector::ZeroVector, FVector(3.0e6), 3.0e6)).IsEmpty());
	TestFalse(TEXT("a car 100 km out"), Problem(FBoxSphereBounds(FVector(1.0e7, 0.0, 0.0), FVector(280.0), 300.0)).IsEmpty());
	return true;
}

#endif
