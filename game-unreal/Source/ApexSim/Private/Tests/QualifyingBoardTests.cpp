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

// -----------------------------------------------------------------------------
// The shoot-out's waiting display: the run order with names and where the
// local car stands.
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexQualifyingShootoutTest,
	"ApexSim.Race.QualifyingBoard.Shootout",
	ApexTestFlags)

bool FApexQualifyingShootoutTest::RunTest(const FString& Parameters)
{
	const TArray<FApexRosterEntry> Roster = {
		Entry(0, TEXT("Alder"), true), Entry(1, TEXT("Birch"), true), Entry(2, TEXT("You"), false),
		Entry(3, TEXT("Cedar"), true), Entry(4, TEXT("Dogwood"), true) };
	FApexShootoutState State;
	State.bValid = true;
	State.Order = { 4, 3, 1, 2, 0 };
	State.Running = 2;
	State.Done = 1;
	State.bFlying = true;

	ApexBoard::FShootoutView View = ApexBoard::Shootout(State, Roster, 2);
	TestEqual(TEXT("five in the order"), View.Lines.Num(), 5);
	TestEqual(TEXT("named"), View.Lines[0].Name, FString(TEXT("Dogwood")));
	TestTrue(TEXT("the first ran"), View.Lines[0].Run == ApexBoard::EShootoutRun::Done);
	TestTrue(TEXT("the second is out"), View.Lines[1].Run == ApexBoard::EShootoutRun::Running);
	TestEqual(TEXT("the car out"), View.RunningCar, 3);
	TestTrue(TEXT("on its flying lap"), View.bFlying);
	TestEqual(TEXT("you run 4th"), View.YourPlace, 4);
	TestEqual(TEXT("two to go: the one out and the one after"), View.ToGo, 2);
	TestEqual(TEXT("the call"), ApexBoard::ShootoutCall(View), FString(TEXT("YOU RUN 4TH · 2 TO GO")));

	State.Running = 4;
	State.Done = 3;
	State.bFlying = false;
	View = ApexBoard::Shootout(State, Roster, 2);
	TestTrue(TEXT("your turn"), View.bYourTurn);
	TestEqual(TEXT("YOUR TURN"), ApexBoard::ShootoutCall(View), FString(TEXT("YOUR TURN")));

	State.Running = 3;
	State.Done = 2;
	View = ApexBoard::Shootout(State, Roster, 2);
	TestEqual(TEXT("next"), ApexBoard::ShootoutCall(View), FString(TEXT("YOU RUN 4TH · 1 TO GO")));
	State.Running = 0;
	State.Done = 3;
	View = ApexBoard::Shootout(State, Roster, 2);
	TestEqual(TEXT("between runs, next"), ApexBoard::ShootoutCall(View), FString(TEXT("YOU RUN 4TH · YOU ARE NEXT")));
	State.Running = 5;
	State.Done = 4;
	View = ApexBoard::Shootout(State, Roster, 2);
	TestTrue(TEXT("done"), View.bYouDone);
	TestEqual(TEXT("not in the order"), ApexBoard::ShootoutCall(ApexBoard::Shootout(State, Roster, 9)), FString());

	TestEqual(TEXT("1st"), ApexBoard::Ordinal(1), FString(TEXT("1st")));
	TestEqual(TEXT("12th"), ApexBoard::Ordinal(12), FString(TEXT("12th")));
	TestEqual(TEXT("22nd"), ApexBoard::Ordinal(22), FString(TEXT("22nd")));
	TestTrue(TEXT("a plain session's shoot-out"), ApexBoard::IsShootoutRound(EApexQualifyingFormat::Shootout, false, 1, 1));
	TestFalse(TEXT("not Q1 of a weekend"), ApexBoard::IsShootoutRound(EApexQualifyingFormat::Shootout, true, 1, 3));
	TestTrue(TEXT("its final round"), ApexBoard::IsShootoutRound(EApexQualifyingFormat::Shootout, true, 3, 3));
	TestFalse(TEXT("open"), ApexBoard::IsShootoutRound(EApexQualifyingFormat::Open, false, 1, 1));
	return true;
}

#endif
