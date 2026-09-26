#pragma once

#include "CoreMinimal.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "Track/ApexTrackSceneData.h"

class UPackage;
class UWorld;

/**
 * Turns a parsed track export into Unreal assets and a level: the cooked
 * way a circuit reaches the game.
 *
 * The level's contents come from `FApexTrackSceneBuilder`, the same code the
 * game runs to build a track at runtime; this class is the editor's asset
 * factory for it (material instance constants and fully built meshes, each
 * saved in its own package) plus the level world it spawns into and saves.
 * The parent materials are shared assets under `/Game/Materials/Track`,
 * baked first if missing (`ApexTrackMaterialGraphs`).
 *
 * Everything is written under a per-track content folder and regenerated
 * wholesale on the next import, so nothing in there survives hand-editing.
 * Point `-dest` somewhere else if you want to keep a tweaked copy.
 */
class APEXTRACKEDITOR_API FApexTrackAssetBuilder : public IApexTrackAssetFactory
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

	// IApexTrackAssetFactory
	virtual UMaterialInterface* MakeMaterial(
		const FString& Key, UMaterialInterface* Parent, const FApexMaterialParams& Params) override;
	virtual UStaticMesh* MakeMesh(const FString& Name, FMeshDescription& Description, const TArray<FName>& SlotNames,
		const TArray<UMaterialInterface*>& SlotMaterials, bool bTrackSurface, const FBox& Bounds) override;
	virtual bool WantsCollisionComponents() const override { return false; }
	virtual void LabelActor(AActor* Actor, const FString& Label) override;

private:
	/** Remove assets left by a previous import of this track. */
	void PurgeExistingAssets();
	bool SaveTouchedPackages(FString& OutError);

	FString TrackFolder;
	FString TrackName;
	FString LevelPackage;
	/** The generated world, needed when saving its map package. */
	TObjectPtr<UWorld> LevelWorld;
	TArray<TObjectPtr<UPackage>> TouchedPackages;
};
