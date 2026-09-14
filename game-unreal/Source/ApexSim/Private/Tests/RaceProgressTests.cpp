#include "ApexTestCommon.h"
#include "Race/ApexRaceCoordinate.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Monza-ish, but any positive length exercises the same branches. */
	constexpr float L = 5793.0f;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRaceDistanceTest,
	"ApexSim.Race.RaceDistance",
	ApexTestFlags)

bool FApexRaceDistanceTest::RunTest(const FString& Parameters)
{
	using ApexRace::RaceDistanceM;

	// The grid sits behind the start/finish line: lap 0 with a station just
	// short of the full length is distance still to cover, not a lap banked.
	TestTrue(TEXT("grid car reads negative"), RaceDistanceM(0, L - 40.0f, L) < 0.0f);
	TestTrue(TEXT("pole (nearest the line) leads the grid"),
		RaceDistanceM(0, L - 10.0f, L) > RaceDistanceM(0, L - 40.0f, L));

	// Crossing the line on lap 0 must not jump: just behind is just below
	// zero, just past is just above.
	TestTrue(TEXT("continuous across the line"),
		RaceDistanceM(0, 2.0f, L) - RaceDistanceM(0, L - 2.0f, L) < 8.0f);
	TestTrue(TEXT("ordered across the line"),
		RaceDistanceM(0, 2.0f, L) > RaceDistanceM(0, L - 2.0f, L));

	// The server flips lap 0 -> 1 at 10% of the lap; the value must agree on
	// both sides of that flip.
	TestEqual(TEXT("lap 0 -> 1 handoff"),
		RaceDistanceM(0, L * 0.1f, L), RaceDistanceM(1, L * 0.1f, L));

	// A genuine end of lap 1 is nearly a lap of progress — the grid rule must
	// only ever fire on lap 0.
	TestTrue(TEXT("end of lap 1 is not the grid"), RaceDistanceM(1, L - 10.0f, L) > L * 0.9f);

	// The station wraps to zero exactly as the lap counter steps.
	TestTrue(TEXT("continuous across a lap wrap"),
		RaceDistanceM(2, 2.0f, L) - RaceDistanceM(1, L - 2.0f, L) < 8.0f);
	TestTrue(TEXT("ordered across a lap wrap"),
		RaceDistanceM(2, 2.0f, L) > RaceDistanceM(1, L - 2.0f, L));

	// A completed lap outranks any station on the lap before.
	TestTrue(TEXT("laps dominate stations"),
		RaceDistanceM(3, 5.0f, L) > RaceDistanceM(2, L - 5.0f, L));

	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexRaceOrderTest,
	"ApexSim.Race.RaceOrder",
	ApexTestFlags)

bool FApexRaceOrderTest::RunTest(const FString& Parameters)
{
	using ApexRace::RanksAhead;
	using ApexRace::DisplayLap;

	// A finished car drives on into its cool-down lap: its distance keeps
	// growing, but a car still racing must never be ranked with it.
	TestTrue(TEXT("finisher ahead of a car further round"), RanksAhead(2, L * 3.0f, 0, L * 3.5f));
	TestFalse(TEXT("car still racing behind a finisher"), RanksAhead(0, L * 3.5f, 2, L * 3.0f));

	// Finishers stay in crossing order whatever they do afterwards.
	TestTrue(TEXT("winner ahead of second"), RanksAhead(1, L * 3.0f, 2, L * 3.4f));
	TestFalse(TEXT("second behind winner"), RanksAhead(2, L * 3.4f, 1, L * 3.0f));

	// Nobody finished: race distance decides, and a tie is not "ahead".
	TestTrue(TEXT("further round is ahead"), RanksAhead(0, 1200.0f, 0, 1100.0f));
	TestFalse(TEXT("a tie is not ahead"), RanksAhead(0, 1200.0f, 0, 1200.0f));

	// The lap counter keeps stepping past the race distance on the cool-down
	// lap; the driver still reads the last lap.
	TestEqual(TEXT("grid shows lap 1"), DisplayLap(0, 3), 1);
	TestEqual(TEXT("mid race"), DisplayLap(2, 3), 2);
	TestEqual(TEXT("cool-down lap clamps"), DisplayLap(4, 3), 3);
	TestEqual(TEXT("no limit, no clamp"), DisplayLap(7, 0), 7);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
