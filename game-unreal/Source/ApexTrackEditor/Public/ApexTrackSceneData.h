#pragma once

#include "CoreMinimal.h"

/**
 * In-memory form of a `.uescene.json` baked by the ApexSim track editor.
 *
 * The editor resolves everything before writing: station spans are already
 * triangles, and every value is in Unreal's frame (centimeters, degrees,
 * left-handed, winding matched to Unreal's front-face convention). Nothing
 * here converts coordinates — if you find yourself negating a Y or flipping
 * an index order, the bug is on the Rust side.
 *
 * See `track-editor/TRACK_EDITOR.md` section 5 for the format.
 */

/** One material key the meshes reference. */
struct FApexTrackMaterial
{
	FString Key;
	/** road, curb, surface, marking or pit_lane. */
	FString Family;
	FLinearColor BaseColor = FLinearColor::White;
};

/** One bakeable static mesh. */
struct FApexTrackMesh
{
	FString Name;
	FString MaterialKey;
	/** UE centimeters. */
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	/** u along the track, v across it, both in meters. */
	TArray<FVector2f> UVs;
	TArray<uint32> Indices;

	int32 NumTriangles() const { return Indices.Num() / 3; }
};

struct FApexTrackProp
{
	FString Kind;
	FString Asset;
	FVector Location = FVector::ZeroVector;
	float YawDeg = 0.0f;
	float Scale = 1.0f;
	FString Text;
};

struct FApexTrackGridSlot
{
	/** 1-based; slot 1 is pole. */
	int32 Position = 0;
	FVector Location = FVector::ZeroVector;
	float YawDeg = 0.0f;
};

struct FApexTrackCenterlinePoint
{
	float StationCm = 0.0f;
	FVector Location = FVector::ZeroVector;
	float YawDeg = 0.0f;
	float HalfLeftCm = 0.0f;
	float HalfRightCm = 0.0f;
};

struct FApexTrackPitLane
{
	float WidthCm = 0.0f;
	int32 BoxCount = 0;
	float SpeedLimitKph = 0.0f;
};

struct FApexTrackScene
{
	FString TrackId;
	FString TrackName;
	FString SourceTrack;
	bool bClosedLoop = false;
	float LengthCm = 0.0f;

	FString Country;
	FString City;
	FString Category;
	FString EnvironmentType;

	TArray<FApexTrackMaterial> Materials;
	TArray<FApexTrackMesh> Meshes;
	TArray<FApexTrackProp> Props;
	TArray<FApexTrackGridSlot> Grid;
	TArray<FApexTrackCenterlinePoint> Centerline;
	TOptional<FApexTrackPitLane> PitLane;

	const FApexTrackMaterial* FindMaterial(const FString& Key) const
	{
		return Materials.FindByPredicate(
			[&Key](const FApexTrackMaterial& M) { return M.Key == Key; });
	}
};
