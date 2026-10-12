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

// -----------------------------------------------------------------------------
// The qualifying results sheet: grid order, gap to pole and to the car ahead,
// the purple splits and each car's ideal lap.
// -----------------------------------------------------------------------------

namespace
{
	ApexBoard::FSheetEntry SheetEntry(int32 Index, int32 BestMs, TArray<int32> Splits, TArray<int32> Sectors)
	{
		ApexBoard::FSheetEntry E;
		E.CarIndex = Index;
		E.BestMs = BestMs;
		E.BestLapSplitsMs = MoveTemp(Splits);
		E.BestSectorsMs = MoveTemp(Sectors);
		return E;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexQualifyingSheetTest,
	"ApexSim.Race.QualifyingSheet",
	ApexTestFlags)

bool FApexQualifyingSheetTest::RunTest(const FString& Parameters)
{
	const TArray<ApexBoard::FSheetEntry> Entries = {
		SheetEntry(0, 91500, { 30000, 31000, 30500 }, { 30000, 30900, 30500 }),
		SheetEntry(1, 0, {}, { 30100, 0, 0 }),
		SheetEntry(2, 91200, { 30200, 30700, 30300 }, { 30200, 30700, 30300 }),
		SheetEntry(3, 91500, { 30100, 30800, 30600 }, { 30100, 30800, 30600 }),
	};

	const TArray<ApexBoard::FSheetRow> Rows = ApexBoard::Sheet(Entries);
	if (!TestEqual(TEXT("every car is listed"), Rows.Num(), 4))
	{
		return false;
	}
	TestEqual(TEXT("pole"), Rows[0].CarIndex, 2);
	TestEqual(TEXT("pole is P1"), Rows[0].Position, 1);
	TestEqual(TEXT("pole has no gap"), Rows[0].GapMs, 0);
	TestEqual(TEXT("pole has no interval"), Rows[0].IntervalMs, 0);
	TestEqual(TEXT("an equal time keeps car order"), Rows[1].CarIndex, 0);
	TestEqual(TEXT("P2 gap"), Rows[1].GapMs, 300);
	TestEqual(TEXT("P2 interval"), Rows[1].IntervalMs, 300);
	TestEqual(TEXT("P3 is the other equal time"), Rows[2].CarIndex, 3);
	TestEqual(TEXT("P3 gap to pole"), Rows[2].GapMs, 300);
	TestEqual(TEXT("P3 interval to P2"), Rows[2].IntervalMs, 0);
	TestEqual(TEXT("no time last"), Rows[3].CarIndex, 1);
	TestEqual(TEXT("no time has no position"), Rows[3].Position, 0);
	TestTrue(TEXT("no time has no splits"), Rows[3].SplitsMs.IsEmpty());

	// Purple: S1 by car 0, S2 and S3 by pole.
	TestTrue(TEXT("car 0's S1 purple"), Rows[1].FastestSplit[0]);
	TestFalse(TEXT("pole's S1 not purple"), Rows[0].FastestSplit[0]);
	TestTrue(TEXT("pole's S2 purple"), Rows[0].FastestSplit[1]);
	TestTrue(TEXT("pole's S3 purple"), Rows[0].FastestSplit[2]);

	TestEqual(TEXT("ideal lap from the best sectors"), Rows[1].IdealMs, 30000 + 30900 + 30500);
	TestEqual(TEXT("no ideal lap with a sector missing"), Rows[3].IdealMs, 0);
	return true;
}

#endif
