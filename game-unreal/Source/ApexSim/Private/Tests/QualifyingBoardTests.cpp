#include "ApexTestCommon.h"
#include "Race/ApexQualifyingBoard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FApexRosterEntry Entry(int32 Index, const TCHAR* Name, bool bAi)
	{
		FApexRosterEntry E;
		E.CarIndex = Index;
		E.PlayerName = Name;
		E.bIsAi = bAi;
		return E;
	}

	FApexCarTelemetry Car(int32 Index, int32 BestMs, int32 LastMs, bool bGarage)
	{
		FApexCarTelemetry C;
		C.CarIndex = Index;
		C.BestLapTimeMs = BestMs;
		C.LastLapTimeMs = LastMs;
		C.bInGarage = bGarage;
		return C;
	}
}

// -----------------------------------------------------------------------------
// The qualifying scoreboard: fastest first, no time behind, gaps to the leader,
// stable between refreshes.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexQualifyingBoardTest,
	"ApexSim.Race.QualifyingBoard",
	ApexTestFlags)

bool FApexQualifyingBoardTest::RunTest(const FString& Parameters)
{
	const TArray<FApexRosterEntry> Roster = {
		Entry(0, TEXT("Ann"), true), Entry(1, TEXT("Guido"), false), Entry(2, TEXT("Cy"), true),
		Entry(3, TEXT("Di"), true), Entry(4, TEXT("Ed"), true),
	};
	const TArray<FApexCarTelemetry> Cars = {
		Car(0, 91500, 92000, false),
		Car(1, 91200, 91200, true),
		Car(2, 0, 0, false),
		Car(3, 91500, 93000, false),
		// Car 4 sends nothing yet.
	};

	const TArray<ApexBoard::FRow> Rows = ApexBoard::Build(Roster, Cars, 1);
	if (!TestEqual(TEXT("every car is listed"), Rows.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("fastest first"), Rows[0].Name, FString(TEXT("Guido")));
	TestTrue(TEXT("that is you"), Rows[0].bYou);
	TestTrue(TEXT("in the garage"), Rows[0].bInGarage);
	TestEqual(TEXT("P1"), Rows[0].Position, 1);
	TestEqual(TEXT("no gap to itself"), Rows[0].GapMs, 0);

	// Equal times keep car order, so a refresh does not shuffle them.
	TestEqual(TEXT("then the earlier car of a tie"), Rows[1].Name, FString(TEXT("Ann")));
	TestEqual(TEXT("P2"), Rows[1].Position, 2);
	TestEqual(TEXT("gap to the leader"), Rows[1].GapMs, 300);
	TestEqual(TEXT("last lap"), Rows[1].LastMs, 92000);
	TestEqual(TEXT("the tie"), Rows[2].Name, FString(TEXT("Di")));
	TestEqual(TEXT("same gap"), Rows[2].GapMs, 300);
	TestEqual(TEXT("P3"), Rows[2].Position, 3);

	// No time: behind, in car order, no position and no gap.
	TestEqual(TEXT("untimed car 2"), Rows[3].CarIndex, 2);
	TestEqual(TEXT("untimed car 4"), Rows[4].CarIndex, 4);
	TestEqual(TEXT("no position"), Rows[3].Position, 0);
	TestEqual(TEXT("no gap"), Rows[4].GapMs, 0);
	TestEqual(TEXT("no time"), Rows[4].BestMs, 0);

	TestEqual(TEXT("an empty roster is an empty board"), ApexBoard::Build({}, Cars, 1).Num(), 0);
	return true;
}

#endif
