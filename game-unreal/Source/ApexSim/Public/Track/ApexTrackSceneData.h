#pragma once

#include "CoreMinimal.h"
#include "Track/ApexDdsReader.h"

/**
 * In-memory form of a `.uescene.json` baked by the ApexSim track editor.
 *
 * The editor resolves everything before writing: station spans are already
 * triangles, and every value is in Unreal's frame (centimeters, degrees,
 * left-handed, winding matched to Unreal's front-face convention). Nothing
 * here converts coordinates — if you find yourself negating a Y or flipping
 * an index order, the bug is on the Rust side.
 *
 * See `docs/content/track-format.md` for the format. Version 2
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
	/** road, curb, surface, marking, pit_lane, decal or (version 3) scenery. */
	FString Family;
	FLinearColor BaseColor = FLinearColor::White;
	/**
	 * Version 3, `surface` family: which of the kit's ground texture sets
	 * the key samples (`grass`, `gravel`...) when the key itself does not
	 * say; empty for the generated keys, which `ApexGround::LookFor` reads.
	 */
	FString GroundSet;
	/**
	 * Version 3, `scenery` family: a DDS relative to the manifest's folder,
	 * parsed into `FApexTrackScene::Textures`; empty for a flat colour.
	 */
	FString Texture;
	/** Version 3, `scenery`: `opaque`, `masked` or `translucent`. */
	FString Blend;
	bool bTwoSided = false;
	/** Version 3, `scenery`: roughness, or negative for the parent's. */
	float Roughness = -1.0f;
	/** Version 3, `scenery` masked: the alpha cutoff, or negative for the parent's. */
	float AlphaCutoff = -1.0f;

	/** An imported, textured material drawn on the car parents rather than the track base. */
	bool IsScenery() const
	{
		return Family == TEXT("scenery");
	}

	/**
	 * Whether a surface of this material lies flat on the ground: the road,
	 * the curbs, the paint, the pit lane, the decals and the exporter's
	 * `surface_*` bands (grass apron, gravel, run-off, astroturf). They
	 * receive shadows but cast none worth drawing (a 5 cm curb's shadow is
	 * under a shadow texel at most clipmap levels), while each 250 m section
	 * is one of the large instances that fill virtual shadow maps'
	 * non-Nanite marking queue (docs/proposals/nanite-shadows.md). The
	 * terrain (`ground`, the `horizon`), structures, walls and an imported
	 * circuit's own ground (`ac_*`, which may be a hillside) are not flat.
	 */
	bool IsFlatSurface() const
	{
		return Family == TEXT("road") || Family == TEXT("curb") || Family == TEXT("marking")
			|| Family == TEXT("pit_lane") || Family == TEXT("decal")
			|| (Family == TEXT("surface") && Key.StartsWith(TEXT("surface_")));
	}
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
	/** Version 3: how far the mesh is drawn, UE cm; 0 for always. */
	float DrawDistanceCm = 0.0f;
	/**
	 * Version 3: whether the mesh is a traceable track surface (a collision
	 * component for the racing line and the cameras). Every version 2 mesh
	 * is; an imported circuit's scenery is not.
	 */
	bool bCollision = true;

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
	/**
	 * `metadata.latitude_deg` (degrees north) and `metadata.north_yaw_deg`
	 * (true north's yaw in the track frame, degrees counter-clockwise from
	 * +X): where the sun stands over the real circuit. Unset in an export
	 * from before the keys (the sky then keeps 50° N, north down +X).
	 */
	TOptional<float> LatitudeDeg;
	TOptional<float> NorthYawDeg;
	/** Version 2: the mesh blob's file name, beside the manifest. */
	FString MeshBlob;
	/** Version 3: which importer wrote the export whole (`ac`); empty for a generated circuit. */
	FString Imported;
};

struct FApexTrackScene
{
	FString TrackId;
	FString TrackName;
	FString SourceTrack;
	/** The manifest's folder, which the materials' texture paths are relative to. */
	FString BaseDir;
	/** See `FApexTrackSceneHeader::Imported`. */
	FString Imported;
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
	/**
	 * Version 3: every texture a scenery material names, keyed by the path
	 * the material carries, read and parsed with the scene (off the game
	 * thread, like the meshes). A material whose texture failed to read
	 * has no entry here and draws its base colour.
	 */
	TMap<FString, FApexTrackTexture> Textures;
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
