#pragma once

#include "CoreMinimal.h"

struct FApexTrackScene;
class UMaterialInterface;
class UStaticMesh;

/**
 * Turns a parsed track export into Unreal assets and a level.
 *
 * Everything is written under a per-track content folder and regenerated
 * wholesale on the next import, so nothing in there survives hand-editing.
 * Point `-dest` somewhere else if you want to keep a tweaked copy.
 */
class APEXTRACKEDITOR_API FApexTrackAssetBuilder
{
public:
	/**
	 * @param DestRoot   content path to build under, e.g. `/Game/Tracks`
	 * @param TrackStem  export stem, e.g. `Monza` — names the subfolder and the level
	 */
	FApexTrackAssetBuilder(const FString& DestRoot, const FString& TrackStem);

	/**
	 * Generate materials, static meshes and the level, then save every
	 * package. Returns false with `OutError` set on the first failure; some
	 * packages may already have been written.
	 */
	bool Build(const FApexTrackScene& Scene, FString& OutError);

	/** `/Game/Tracks/Monza/L_Monza` once `Build` has run. */
	const FString& LevelPackageName() const { return LevelPackage; }

private:
	/** Remove assets left by a previous import of this track. */
	void PurgeExistingAssets();

	bool BuildMaterials(const FApexTrackScene& Scene, FString& OutError);
	bool BuildMeshes(const FApexTrackScene& Scene, FString& OutError);
	/** Reject meshes Unreal built badly, e.g. with NaN or empty bounds. */
	bool ValidateMeshes(FString& OutError);
	bool BuildLevel(const FApexTrackScene& Scene, FString& OutError);
	bool SaveTouchedPackages(FString& OutError);

	FString TrackFolder;
	FString TrackName;
	FString LevelPackage;
	/** The generated world, needed when saving its map package. */
	TObjectPtr<class UWorld> LevelWorld;

	/** Shared parent material, created once per import run. */
	TObjectPtr<UMaterialInterface> ParentMaterial;
	/** Material key -> generated instance. */
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;
	/** Mesh name -> generated static mesh. */
	TMap<FString, TObjectPtr<UStaticMesh>> Meshes;

	TArray<TObjectPtr<UPackage>> TouchedPackages;
};
