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

// A puddle lies where the slice says across the road: per-bin water fills
// each bin's A; a slice without spreads the cell's depth across.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRoadStateMapPuddleTest, "ApexSim.Race.RoadStateMap.Puddle", ApexTestFlags)

bool FApexRoadStateMapPuddleTest::RunTest(const FString& Parameters)
{
	using namespace ApexRoadStateMapTest;
	FApexRoadStateMap Map;
	FApexRoadState Wet = Slice(4, 2);
	Wet.WaterRows.SetNumZeroed(2 * 32);
	for (int32 b = 0; b < 32; ++b)
	{
		Wet.WaterRows[b] = b >= 20 && b <= 24 ? 160 : 40;
	}
	TestTrue(TEXT("a slice with water rows applies"), Map.Apply(Wet));
	TestEqual(TEXT("in the puddle"), (int32)Map.StateAt(4, 22).A, 160);
	TestEqual(TEXT("the flat beside it"), (int32)Map.StateAt(4, 10).A, 40);
	TestEqual(TEXT("cell 5 dry"), (int32)Map.StateAt(5, 22).A, 0);
	TestEqual(TEXT("the rest of the texel as before"), (int32)Map.StateAt(4, 22).R, 22 * 8);
	// The puddle over the cell's flat share, in the snow map's A: what the
	// material draws a puddle from, whatever the rain.
	TestEqual(TEXT("the puddle stands 120 over the flat"), (int32)Map.GetSnow()[22 * Map.NumCells() + 4].A, 120);
	TestEqual(TEXT("none on the flat"), (int32)Map.GetSnow()[10 * Map.NumCells() + 4].A, 0);
	TestEqual(TEXT("none in a dry cell"), (int32)Map.GetSnow()[22 * Map.NumCells() + 5].A, 0);
	TestTrue(TEXT("a slice without water rows applies"), Map.Apply(Slice(4, 2)));
	TestEqual(TEXT("the cell's depth across"), (int32)Map.StateAt(4, 10).A, 40);
	TestEqual(TEXT("and in the puddle's place"), (int32)Map.StateAt(4, 22).A, 40);
	TestEqual(TEXT("no puddle once the slice says none"), (int32)Map.GetSnow()[22 * Map.NumCells() + 4].A, 0);
	return true;
}

// The snow rides along: a slice with snow rows fills the snow map (cover and
// ice per bin, the verge in B), one without says there is none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FApexRoadStateMapSnowTest, "ApexSim.Race.RoadStateMap.Snow", ApexTestFlags)

bool FApexRoadStateMapSnowTest::RunTest(const FString& Parameters)
{
	using namespace ApexRoadStateMapTest;
	FApexRoadStateMap Map;
	FApexRoadState Snowy = Slice(2, 2);
	// A snow row is the verge, then cover and ice per bin: cell 2 white with a
	// frozen bin 5, cell 3 bare.
	const int32 RowBytes = Snowy.SnowRowBytes();
	TestEqual(TEXT("a snow row is 65 bytes at 32 bins"), RowBytes, 65);
	Snowy.SnowRows.SetNumZeroed(2 * RowBytes);
	Snowy.SnowRows[0] = 255;
	for (int32 b = 0; b < 32; ++b)
	{
		Snowy.SnowRows[1 + 2 * b] = 200;
	}
	Snowy.SnowRows[1 + 2 * 5 + 1] = 90;
	TestTrue(TEXT("a slice with snow applies"), Map.Apply(Snowy));
	TestTrue(TEXT("the map has snow"), Map.HasSnow());
	const FColor Cover = Map.GetSnow()[5 * Map.NumCells() + 2];
	TestEqual(TEXT("cover in R"), (int32)Cover.R, 200);
	TestEqual(TEXT("ice in G"), (int32)Cover.G, 90);
	TestEqual(TEXT("the verge in B"), (int32)Cover.B, 255);
	TestEqual(TEXT("no puddle in A"), (int32)Cover.A, 0);
	TestEqual(TEXT("cell 3 is bare"), Map.GetSnow()[5 * Map.NumCells() + 3], FColor(0, 0, 0, 0));

	// The server leaves the field off once the road is bare: the same cells
	// without snow rows clear it.
	TestTrue(TEXT("the slice without snow applies"), Map.Apply(Slice(2, 2)));
	TestFalse(TEXT("the snow is gone"), Map.HasSnow());
	TestEqual(TEXT("cell 2 is bare again"), Map.GetSnow()[5 * Map.NumCells() + 2], FColor(0, 0, 0, 0));
	return true;
}

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

	// What the tyres pick up: the depth of the cell under the car, unknown
	// (negative) where no slice has reached yet or before any slice.
	TestEqual(TEXT("cell 9's water under a car"), Map.WaterAt(95.0f, FVector2D(RightOfCentre.X, RightOfCentre.Y)), 90.0f);
	TestTrue(TEXT("a cell not sent has no water figure"), Map.WaterAt(35.0f, FVector2D(RightOfCentre.X, RightOfCentre.Y)) < 0.0f);
	TestTrue(TEXT("nor an empty map"), FApexRoadStateMap().WaterAt(95.0f, FVector2D::ZeroVector) < 0.0f);

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
