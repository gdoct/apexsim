#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"

/**
 * The session's road state as the road material reads it: the lap's cells
 * kept whole on the client from the server's `RoadState` slices (a burst on
 * join, then a slice every couple of seconds), packed the way the two
 * textures of `M_ApexTrackRoad` hold them (docs/server/conditions.md,
 * "The road on screen").
 *
 *  - The state: a column per cell, a row per bin across the road, one
 *    FColor each: R rubber, G marbles, B dry, A water depth (as the wire's
 *    byte: 100 is heavy rain on the flat, so the material reads A x 2.55).
 *  - The geometry: one texel per cell, where the server measures that
 *    cell's bins from, in Unreal's frame: X, Y the centerline point in world
 *    cm, Z, W the unit vector to its left in world axes. The material's
 *    lateral is -dot(world - point, left) / 100, positive right, which is
 *    `RoadState::lateral_of` in the server's frame.
 *
 * A cell no slice has reached yet holds the neutral state (rubber at the
 * calibrated half, dry, no marbles), so it draws as built whatever bin a
 * pixel lands in. Pure data: no world, no textures (the race director owns
 * those and uploads from here).
 */
class APEXSIM_API FApexRoadStateMap
{
public:
	/** The neutral texel: rubber 0.5, no marbles, not dry, no water. */
	static FColor NeutralTexel() { return FColor(128, 0, 0, 0); }

	/**
	 * Take one slice. A slice of another layout (another lap, cell length or
	 * bin count) starts the map again at that layout. False when the slice
	 * is unusable (no cells, rows short of their geometry).
	 */
	bool Apply(const FApexRoadState& Slice);

	/** Forget everything (a session left, another backdrop). */
	void Reset();

	bool IsValid() const { return Cells > 0 && Bins > 0; }
	/** Texture width: cells round the lap. */
	int32 NumCells() const { return Cells; }
	/** Texture height: bins across the road. */
	int32 NumBins() const { return Bins; }
	float GetCellM() const { return CellM; }
	float GetLapM() const { return LapM; }
	float GetHalfSpanM() const { return HalfSpanM; }
	/** How many cells a slice has filled since the map began. */
	int32 NumKnownCells() const { return KnownCells; }

	/** The state, row by row (bin), a texel per cell; NumCells x NumBins. */
	const TArray<FColor>& GetState() const { return State; }
	/** The geometry, a texel per cell, Unreal frame (see the class note). */
	const TArray<FLinearColor>& GetGeometry() const { return Geometry; }
	/** The debris the last slice reported, server frame, metres. */
	const TArray<FVector2D>& GetDebris() const { return Debris; }

	/** Set by Apply; cleared by the caller once it has uploaded. */
	bool TakeDirty() { const bool b = bDirty; bDirty = false; return b; }
	/** Set when the size changed (the textures must be made again). */
	bool TakeResized() { const bool b = bResized; bResized = false; return b; }
	/** Set when the debris list changed. */
	bool TakeDebrisChanged() { const bool b = bDebrisChanged; bDebrisChanged = false; return b; }

	/** One texel of the state. */
	FColor StateAt(int32 Cell, int32 Bin) const;

	/**
	 * What the material computes, on the CPU for tests: the texture `u` of
	 * a station (metres along the lap), and the `v` of a world position
	 * (cm, Unreal frame) in the cell at that station.
	 */
	float TexU(float StationM) const;
	float TexV(float StationM, const FVector2D& WorldCm) const;
	/**
	 * The water at a station and world position (cm, Unreal frame),
	 * percent of heavy rain on the flat (the texel's A); negative when the
	 * map is empty or no slice has reached that cell yet.
	 */
	float WaterAt(float StationM, const FVector2D& WorldCm) const;
	/** `RoadStateU`: 1 / (cell length x cells), station metres to `u`. */
	float UPerMetre() const { return Cells > 0 && CellM > 0.0f ? 1.0f / (CellM * Cells) : 0.0f; }

private:
	int32 Cells = 0;
	int32 Bins = 0;
	float CellM = 0.0f;
	float LapM = 0.0f;
	float HalfSpanM = 0.0f;
	int32 KnownCells = 0;
	TArray<FColor> State;
	TArray<FLinearColor> Geometry;
	TBitArray<> Known;
	TArray<FVector2D> Debris;
	bool bDirty = false;
	bool bResized = false;
	bool bDebrisChanged = false;
};
