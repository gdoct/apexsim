#include "ApexTestCommon.h"
#include "Race/ApexRoadStateMap.h"
#include "Race/ApexRaceCoordinate.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ApexRoadStateMapTest
{
	/**
	 * A 100 m lap of 10 cells, 32 bins over +-16 m, along the server's +X
	 * (left is +Y). `First` and `Count` pick the cells; each cell's bin `b`
	 * carries rubber `b * 8`, marbles `Cell`, dry 200, and the cell's depth
	 * is `Cell * 10`.
	 */
	FApexRoadState Slice(int32 First, int32 Count)
	{
		FApexRoadState S;
		S.SessionId = TEXT("s");
		S.LapM = 100.0f;
		S.CellM = 10.0f;
		S.Bins = 32;
		S.HalfSpanM = 16.0f;
		S.FirstCell = First;
		for (int32 k = 0; k < Count; ++k)
		{
			const int32 Cell = (First + k) % 10;
			S.Rows.Add(static_cast<uint8>(Cell * 10));
			for (int32 b = 0; b < 32; ++b)
			{
				S.Rows.Add(static_cast<uint8>(b * 8));
				S.Rows.Add(static_cast<uint8>(Cell));
				S.Rows.Add(200);
			}
			S.Geometry.Add(FVector4f(Cell * 10.0f + 5.0f, 0.0f, 0.0f, 1.0f));
		}
		return S;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRoadStateMapTest, "ApexSim.Race.RoadStateMap", ApexTestFlags)

bool FApexRoadStateMapTest::RunTest(const FString& Parameters)
{
	using namespace ApexRoadStateMapTest;
	FApexRoadStateMap Map;
	TestFalse(TEXT("empty until a slice"), Map.IsValid());

	// A slice that wraps the lap's end: cells 8, 9, 0.
	TestTrue(TEXT("a slice applies"), Map.Apply(Slice(8, 3)));
	TestEqual(TEXT("ten cells round the lap"), Map.NumCells(), 10);
	TestEqual(TEXT("32 bins"), Map.NumBins(), 32);
	TestEqual(TEXT("three known"), Map.NumKnownCells(), 3);
	TestTrue(TEXT("resized"), Map.TakeResized());
	TestTrue(TEXT("dirty"), Map.TakeDirty());

	const FColor C9 = Map.StateAt(9, 4);
	TestEqual(TEXT("rubber in R"), (int32)C9.R, 32);
	TestEqual(TEXT("marbles in G"), (int32)C9.G, 9);
	TestEqual(TEXT("dry in B"), (int32)C9.B, 200);
	TestEqual(TEXT("depth in A"), (int32)C9.A, 90);
	TestEqual(TEXT("wrapped onto cell 0"), (int32)Map.StateAt(0, 0).G, 0);
	TestEqual(TEXT("a cell not sent is neutral"), Map.StateAt(3, 5), FApexRoadStateMap::NeutralTexel());

	// The material's maths: `u` picks the cell, `v` the bin, as the server's
	// lateral_of does (positive right). Cell 9's centre is (95, 0) m; the
	// server's +Y is left, so 2 m right is server y -2, Unreal y +200 cm.
	TestTrue(TEXT("u of a station"), FMath::IsNearlyEqual(Map.TexU(95.0f), 0.95f, 1e-5f));
	const FVector RightOfCentre = ApexRace::ServerToUnrealPosition(FVector(97.0, -2.0, 0.0));
	const float V = Map.TexV(95.0f, FVector2D(RightOfCentre.X, RightOfCentre.Y));
	TestTrue(TEXT("2 m right is (2 + 16) / 32 across"), FMath::IsNearlyEqual(V, 18.0f / 32.0f, 1e-4f));
	const FVector LeftEdge = ApexRace::ServerToUnrealPosition(FVector(95.0, 15.5, 0.0));
	TestTrue(TEXT("15.5 m left is the first bin"),
		FMath::FloorToInt(Map.TexV(95.0f, FVector2D(LeftEdge.X, LeftEdge.Y)) * 32.0f) == 0);

	// Another layout starts again; the same one fills in.
	TestTrue(TEXT("more of the lap"), Map.Apply(Slice(1, 2)));
	TestEqual(TEXT("five known"), Map.NumKnownCells(), 5);
	TestFalse(TEXT("not resized by the same layout"), Map.TakeResized());
	FApexRoadState Longer = Slice(0, 1);
	Longer.LapM = 200.0f;
	TestTrue(TEXT("a longer lap applies"), Map.Apply(Longer));
	TestEqual(TEXT("twenty cells"), Map.NumCells(), 20);
	TestEqual(TEXT("one known after the reset"), Map.NumKnownCells(), 1);

	// A slice short of its geometry is refused.
	FApexRoadState NoGeometry = Slice(0, 2);
	NoGeometry.Geometry.SetNum(1);
	TestFalse(TEXT("rows without geometry are refused"), Map.Apply(NoGeometry));

	// Debris is reported once per change.
	FApexRoadState WithDebris = Slice(0, 1);
	WithDebris.LapM = 200.0f;
	WithDebris.Debris.Add(FVector2D(12.0, 3.0));
	Map.TakeDebrisChanged();
	Map.Apply(WithDebris);
	TestTrue(TEXT("debris changed"), Map.TakeDebrisChanged());
	Map.Apply(WithDebris);
	TestFalse(TEXT("the same debris is no change"), Map.TakeDebrisChanged());
	return true;
}

#endif
