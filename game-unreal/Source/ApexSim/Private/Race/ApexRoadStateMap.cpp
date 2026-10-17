#include "Race/ApexRoadStateMap.h"

#include "Race/ApexRaceCoordinate.h"

bool FApexRoadStateMap::Apply(const FApexRoadState& Slice)
{
	if (!Slice.IsValid())
	{
		return false;
	}
	const int32 SliceCells = Slice.NumCells();
	if (Slice.Geometry.Num() < SliceCells)
	{
		return false;
	}
	const int32 LapCells = Slice.LapCells();
	if (LapCells != Cells || Slice.Bins != Bins || Slice.CellM != CellM || Slice.HalfSpanM != HalfSpanM)
	{
		Cells = LapCells;
		Bins = Slice.Bins;
		CellM = Slice.CellM;
		HalfSpanM = Slice.HalfSpanM;
		State.Init(NeutralTexel(), Cells * Bins);
		Snow.Init(FColor(0, 0, 0, 0), Cells * Bins);
		Snowy.Init(false, Cells);
		SnowyCells = 0;
		Geometry.Init(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), Cells);
		Known.Init(false, Cells);
		KnownCells = 0;
		bResized = true;
	}
	LapM = Slice.LapM;

	const int32 RowBytes = Slice.RowBytes();
	// The water per bin when the slice says where its puddles lie; else the
	// cell's depth across the whole road.
	const bool bWaterRows = Slice.HasWaterRows();
	for (int32 k = 0; k < SliceCells; ++k)
	{
		const int32 Cell = (Slice.FirstCell + k) % Cells;
		const uint8* Row = Slice.Rows.GetData() + k * RowBytes;
		const uint8 Depth = Row[0];
		const uint8* Water = bWaterRows ? Slice.WaterRows.GetData() + k * Slice.Bins : nullptr;
		// The cell's flat share: its shallowest bin. A puddle is what stands
		// over it, in rain of any strength (the depth itself only passes
		// heavy rain's in a dip under heavy rain).
		uint8 FlatShare = Depth;
		if (Water)
		{
			for (int32 Bin = 0; Bin < Bins; ++Bin)
			{
				FlatShare = FMath::Min(FlatShare, Water[Bin]);
			}
		}
		for (int32 Bin = 0; Bin < Bins; ++Bin)
		{
			const uint8* B = Row + 1 + 3 * Bin;
			State[Bin * Cells + Cell] = FColor(B[0], B[1], B[2], Water ? Water[Bin] : Depth);
		}
		// The cell's snow, if the slice carries any; a slice without says there
		// is none (the server leaves the field off once the road is bare).
		bool bCellSnowy = false;
		const int32 SnowRowBytes = Slice.SnowRowBytes();
		const bool bSliceSnow = Slice.HasSnow() && Slice.SnowRows.Num() >= (k + 1) * SnowRowBytes;
		for (int32 Bin = 0; Bin < Bins; ++Bin)
		{
			const uint8 Puddle = Water ? static_cast<uint8>(Water[Bin] - FlatShare) : 0;
			FColor Texel(0, 0, 0, Puddle);
			if (bSliceSnow)
			{
				const uint8* S = Slice.SnowRows.GetData() + k * SnowRowBytes;
				Texel = FColor(S[1 + 2 * Bin], S[2 + 2 * Bin], S[0], Puddle);
				bCellSnowy |= Texel.R > 0 || Texel.G > 0 || Texel.B > 0;
			}
			Snow[Bin * Cells + Cell] = Texel;
		}
		if (Snowy[Cell] != bCellSnowy)
		{
			Snowy[Cell] = bCellSnowy;
			SnowyCells += bCellSnowy ? 1 : -1;
		}
		// Server frame (metres, +Y left) into Unreal's (cm, +Y right): the
		// point moves, the left vector only flips its Y.
		const FVector4f& G = Slice.Geometry[k];
		const FVector Point = ApexRace::ServerToUnrealPosition(FVector(G.X, G.Y, 0.0));
		Geometry[Cell] = FLinearColor(static_cast<float>(Point.X), static_cast<float>(Point.Y), G.Z, -G.W);
		if (!Known[Cell])
		{
			Known[Cell] = true;
			++KnownCells;
		}
	}
	if (Slice.Debris != Debris)
	{
		Debris = Slice.Debris;
		bDebrisChanged = true;
	}
	bDirty = true;
	return true;
}

void FApexRoadStateMap::Reset()
{
	const bool bHadDebris = Debris.Num() > 0;
	*this = FApexRoadStateMap();
	bResized = true;
	bDirty = true;
	bDebrisChanged = bHadDebris;
}

FColor FApexRoadStateMap::StateAt(int32 Cell, int32 Bin) const
{
	if (Cell < 0 || Cell >= Cells || Bin < 0 || Bin >= Bins)
	{
		return NeutralTexel();
	}
	return State[Bin * Cells + Cell];
}

float FApexRoadStateMap::WaterAt(float StationM, const FVector2D& WorldCm) const
{
	if (!IsValid())
	{
		return -1.0f;
	}
	const int32 Cell = FMath::Clamp(FMath::FloorToInt(TexU(StationM) * Cells), 0, Cells - 1);
	if (!Known.IsValidIndex(Cell) || !Known[Cell])
	{
		return -1.0f;
	}
	const int32 Bin = FMath::Clamp(FMath::FloorToInt(TexV(StationM, WorldCm) * Bins), 0, Bins - 1);
	return static_cast<float>(StateAt(Cell, Bin).A);
}

float FApexRoadStateMap::PuddleAt(float StationM, const FVector2D& WorldCm) const
{
	if (!IsValid())
	{
		return -1.0f;
	}
	const int32 Cell = FMath::Clamp(FMath::FloorToInt(TexU(StationM) * Cells), 0, Cells - 1);
	if (!Known.IsValidIndex(Cell) || !Known[Cell])
	{
		return -1.0f;
	}
	const int32 Bin = FMath::Clamp(FMath::FloorToInt(TexV(StationM, WorldCm) * Bins), 0, Bins - 1);
	return static_cast<float>(Snow[Bin * Cells + Cell].A);
}

float FApexRoadStateMap::TexU(float StationM) const
{
	return StationM * UPerMetre();
}

float FApexRoadStateMap::TexV(float StationM, const FVector2D& WorldCm) const
{
	if (!IsValid() || HalfSpanM <= 0.0f)
	{
		return 0.5f;
	}
	const int32 Cell = FMath::Clamp(FMath::FloorToInt(TexU(StationM) * Cells), 0, Cells - 1);
	const FLinearColor& G = Geometry[Cell];
	const double LeftCm = (WorldCm.X - G.R) * G.B + (WorldCm.Y - G.G) * G.A;
	const double RightM = -LeftCm / ApexRace::MetresToCentimetres;
	return static_cast<float>((RightM + HalfSpanM) / (2.0 * HalfSpanM));
}
