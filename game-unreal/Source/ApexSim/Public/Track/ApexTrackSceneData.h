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
 * See `track-editor/TRACK_EDITOR.md` section 5 for the format. Version 2
 * splits it in two: the JSON manifest keeps everything small and names a
 * `<Stem>.uemesh` blob beside it that carries the vertex data, which the
 * reader loads into `Meshes` as if it had been inline.
 *
 * Plain structs with no UObject in them, so a scene can be read and
 * prepared off the game thread (`UApexTrackInstance` does).
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
	/** Brand on a hoarding, distance on a braking marker, number on a post. */
	FString Text;
	/** Grandstands: length along the heading, metres (bays are laid from it). */
	TOptional<float> LengthM;
	/**
	 * Grandstands: signed bend radius at the station, metres — positive on
	 * the outside of the corner, negative inside, unset on a straight.
	 */
	TOptional<float> RadiusM;
	/** Bridges: road width at the station, metres, which the span is scaled to. */
	TOptional<float> SpanM;
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

/** The start/finish line: where the lights gantry goes. */
struct FApexTrackStartFinish
{
	/** Centre of the line on the road surface, UE cm. */
	FVector Location = FVector::ZeroVector;
	/** Direction of travel, same convention as props. */
	float YawDeg = 0.0f;
	float WidthCm = 0.0f;
};

/** Which kit variants the importer picks, scene-wide (`.ats` `dressing`). */
struct FApexTrackDressing
{
	/** `summer` or `autumn`: autumn swaps the broadleaf trees for their `_autumn` meshes. */
	FString Season = TEXT("summer");
	/** Full stands: every grandstand module imports as its `_crowd` variant. */
	bool bSpectators = true;

	bool IsAutumn() const
	{
		return Season == TEXT("autumn");
	}
};

/**
 * What the catalog needs to know about an export without reading its
 * geometry: the fields ahead of the first array in the manifest.
 */
struct FApexTrackSceneHeader
{
	int32 Version = 0;
	FString TrackId;
	/** The real circuit name (source data; often a trademark, never shown). */
	FString TrackName;
	/** What the game shows (`track_display_name`); empty in an older export. */
	FString DisplayName;
	FString SourceTrack;
	/**
	 * CRC-32 of the YAML the export was baked from, carriage returns
	 * dropped (`ApexContent::Compute`); 0 when the export predates the field.
	 */
	int64 SourceCrc = 0;
	bool bClosedLoop = false;
	float LengthCm = 0.0f;

	FString Country;
	FString City;
	FString Category;
	FString EnvironmentType;
	/** `metadata.description`: what the circuit is modelled on, by place. */
	FString Description;
	/** Version 2: the mesh blob's file name, beside the manifest. */
	FString MeshBlob;
};

struct FApexTrackScene
{
	FString TrackId;
	FString TrackName;
	FString SourceTrack;
	/** See `FApexTrackSceneHeader::SourceCrc`. */
	int64 SourceCrc = 0;
	bool bClosedLoop = false;
	float LengthCm = 0.0f;

	FString Country;
	FString City;
	FString Category;
	FString EnvironmentType;

	FApexTrackDressing Dressing;

	TArray<FApexTrackMaterial> Materials;
	TArray<FApexTrackMesh> Meshes;
	TArray<FApexTrackProp> Props;
	TArray<FApexTrackGridSlot> Grid;
	TArray<FApexTrackCenterlinePoint> Centerline;
	TOptional<FApexTrackPitLane> PitLane;
	TOptional<FApexTrackStartFinish> StartFinish;

	const FApexTrackMaterial* FindMaterial(const FString& Key) const
	{
		return Materials.FindByPredicate(
			[&Key](const FApexTrackMaterial& M) { return M.Key == Key; });
	}
};
