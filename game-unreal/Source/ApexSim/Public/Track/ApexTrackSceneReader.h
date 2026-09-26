#pragma once

#include "CoreMinimal.h"

struct FApexTrackMesh;
struct FApexTrackScene;
struct FApexTrackSceneHeader;

/**
 * Loads the track exports `ats-export` writes.
 *
 * Version 1 is one `.uescene.json` with every vertex buffer inline. Version
 * 2 is that JSON as a manifest (materials, props, grid, centerline, and a
 * header per mesh) plus `<Stem>.uemesh` beside it: a little-endian blob with
 * each mesh's buffers, zlib-compressed per mesh. Both load into the same
 * `FApexTrackScene`. The blob layout is pinned in TRACK_EDITOR.md section 5
 * and by `ApexSim.Track.Reader.*`.
 *
 * Nothing here touches UObjects, so it is safe on a worker thread.
 */
class APEXSIM_API FApexTrackSceneReader
{
public:
	/**
	 * Parse one export (and its mesh blob, for version 2).
	 *
	 * Returns false and fills `OutError` on unreadable files, wrong format
	 * tags, an unsupported version, or geometry whose buffers disagree with
	 * each other. Nothing partial is written to `OutScene` on failure.
	 */
	static bool LoadFromFile(const FString& Path, FApexTrackScene& OutScene, FString& OutError);

	/**
	 * The fields ahead of the manifest's first array, read from the first
	 * few kilobytes of the file only: cheap enough to run over every export
	 * at startup for the catalog. False when the file is not an export.
	 */
	static bool LoadHeader(const FString& Path, FApexTrackSceneHeader& OutHeader, FString& OutError);

	/**
	 * Decode a `.uemesh` blob into meshes, in blob order. Exposed for the
	 * reader tests; `LoadFromFile` is the normal way in.
	 */
	static bool ParseMeshBlob(TConstArrayView<uint8> Blob, TArray<FApexTrackMesh>& OutMeshes, FString& OutError);

	/** `<dir>/Monza.uescene.json` -> `Monza`. */
	static FString StemOf(const FString& ScenePath);

	/** Format tag every export carries. */
	static const TCHAR* FormatTag();

	/** Highest format version this reader understands. */
	static int32 SupportedVersion();

	/** Extension of a manifest, including the compound part: `.uescene.json`. */
	static const TCHAR* SceneExtension();
	/** Extension of a mesh blob: `.uemesh`. */
	static const TCHAR* MeshBlobExtension();
};
