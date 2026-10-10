#include "ApexTestCommon.h"
#include "ApexSessionRecorder.h"
#include "UI/ApexRaceResultsWidget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexRaceResultsTest
{
	FApexCarResult Car(int32 CarIndex, const TCHAR* Name, int32 FinishPosition, TArray<float> Laps, bool bAi = true)
	{
		FApexCarResult Result;
		Result.CarIndex = CarIndex;
		Result.DriverName = Name;
		Result.PlayerId = bAi ? FString() : FString(TEXT("player-1"));
		Result.bIsAi = bAi;
		Result.FinishPosition = FinishPosition;
		Result.LapTimes = MoveTemp(Laps);
		for (float Lap : Result.LapTimes)
		{
			Result.BestLapSeconds = Result.BestLapSeconds > 0.0f ? FMath::Min(Result.BestLapSeconds, Lap) : Lap;
		}
		return Result;
	}

	/** Three laps: two cars in, the player third, one lapped, one still racing (in the recorder's running order). */
	TArray<FApexCarResult> Field()
	{
		return {
			Car(4, TEXT("Still Racing"), 0, { 92.0f, 91.0f }),
			Car(2, TEXT("Second"), 2, { 90.5f, 90.0f, 90.1f }),
			Car(0, TEXT("You"), 3, { 91.0f, 90.4f, 90.2f }, /*bAi*/ false),
			Car(1, TEXT("Winner"), 1, { 90.0f, 89.5f, 89.6f }),
			Car(3, TEXT("Lapped"), 4, { 95.0f, 96.0f }),
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRaceResultsOrderTest, "ApexSim.RaceResults.ClassificationThenRunningOrder", ApexTestFlags)

bool FApexRaceResultsOrderTest::RunTest(const FString& Parameters)
{
	using namespace ApexRaceResultsTest;
	const TArray<FApexCarResult> Results = Field();
	const TArray<ApexRaceResults::FRow> Rows = ApexRaceResults::BuildRows(Results, TEXT("player-1"), /*bLive*/ true);
	if (!TestEqual(TEXT("a row a car"), Rows.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("P1 the winner"), Rows[0].CarIndex, 1);
	TestEqual(TEXT("P2"), Rows[1].CarIndex, 2);
	TestEqual(TEXT("P3 the player"), Rows[2].CarIndex, 0);
	TestEqual(TEXT("P4 the lapped car"), Rows[3].CarIndex, 3);
	TestEqual(TEXT("the car still racing last"), Rows[4].CarIndex, 4);
	TestEqual(TEXT("positions run on"), Rows[4].Position, 5);
	TestTrue(TEXT("the player's row is theirs"), Rows[2].bYou && !Rows[0].bYou);

	TestEqual(TEXT("the winner's race time"), Rows[0].Time, FString(TEXT("4:29.100")));
	TestEqual(TEXT("a gap on the lead lap"), Rows[1].Time, FString(TEXT("+1.500")));
	TestEqual(TEXT("laps down"), Rows[3].Time, FString(TEXT("+1 LAP")));
	TestEqual(TEXT("still racing"), Rows[4].Time, FString(TEXT("ON TRACK")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRaceResultsWatchableTest, "ApexSim.RaceResults.OnlyCarsStillRacingAreWatched", ApexTestFlags)

bool FApexRaceResultsWatchableTest::RunTest(const FString& Parameters)
{
	using namespace ApexRaceResultsTest;
	const TArray<FApexCarResult> Results = Field();

	const TArray<ApexRaceResults::FRow> Live = ApexRaceResults::BuildRows(Results, TEXT("player-1"), /*bLive*/ true);
	TestTrue(TEXT("a car still racing can be watched"), Live[4].bWatchable);
	TestFalse(TEXT("a finisher cannot"), Live[0].bWatchable);
	TestFalse(TEXT("nor a lapped finisher"), Live[3].bWatchable);
	TestTrue(TEXT("our own row brings the panorama back"), Live[2].bWatchable);

	const TArray<ApexRaceResults::FRow> Over = ApexRaceResults::BuildRows(Results, TEXT("player-1"), /*bLive*/ false);
	TestEqual(TEXT("out of time: did not finish"), Over[4].Time, FString(TEXT("DNF")));
	TestFalse(TEXT("and is nothing to watch"), Over[4].bWatchable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRaceResultsTimeTest, "ApexSim.RaceResults.RaceTime", ApexTestFlags)

bool FApexRaceResultsTimeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("minutes"), ApexRaceResults::FormatRaceTime(83.4561f), FString(TEXT("1:23.456")));
	TestEqual(TEXT("hours"), ApexRaceResults::FormatRaceTime(3723.5f), FString(TEXT("1:02:03.500")));
	TestEqual(TEXT("never negative"), ApexRaceResults::FormatRaceTime(-1.0f), FString(TEXT("0:00.000")));
	return true;
}

#endif
