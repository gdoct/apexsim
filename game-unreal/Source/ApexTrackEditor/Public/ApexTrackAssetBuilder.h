#pragma once

#include "CoreMinimal.h"

struct FApexTrackProp;
#include "ApexTrackSceneData.h"

struct FMeshDescription;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

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
	/**
	 * Generated stand-ins for the prop kinds the level places, one mesh per
	 * kind, plus the start-light gantry sized to this track's line.
	 */
	bool BuildPropMeshes(const FApexTrackScene& Scene, FString& OutError);
	/** The emissive parent material and its start-light instance. */
	bool BuildEmissiveMaterial(FString& OutError);
	/** `M_ApexBrand`: a textured surface for the brand and marker slots of authored props. */
	bool BuildBrandMaterial(FString& OutError);
	/** The `StartLights` actor the race director drives during the countdown. */
	void SpawnStartLights(const FApexTrackScene& Scene, class UWorld* World);

	/**
	 * What a prop resolves to. The authored kit first — `SM_<asset>` under
	 * the props root, then the kind's default asset — else the generated
	 * stand-in for the prop's own kind, else nothing (the placeholder cube).
	 */
	struct FResolvedProp
	{
		FString Kind;
		FString Asset;
		FString Text;
		TObjectPtr<UStaticMesh> Mesh;
		bool bAuthored = false;
		bool bFaceRoad = false;
		bool bInstanced = false;
	};
	FResolvedProp ResolveProp(const FApexTrackProp& Prop);
	/** `SM_<asset>` of the authored kit, or null; cached either way. */
	UStaticMesh* FindAuthoredMesh(const FString& Kind, const FString& Asset);
	/**
	 * A material instance of `M_ApexBrand` showing the texture at `TexturePath`,
	 * made once per key; null when the texture does not exist.
	 */
	UMaterialInterface* TextureMaterialFor(const FString& Key, const FString& TexturePath);
	/** A lit instance of `M_ApexEmissive` for one of the kit's lamp/screen slots. */
	UMaterialInterface* EmissiveMaterialFor(FName Slot);
	/** Brand, marker and emissive slot overrides for an authored mesh on a component. */
	void ApplyAuthoredSlots(UStaticMeshComponent* Component, const UStaticMesh* Mesh, const FString& Text);
	/** A stand laid out of bays and end caps under one actor. */
	void SpawnGrandstand(class UWorld* World, const FApexTrackProp& Prop, const FResolvedProp& Resolved,
		float YawDeg, int32 Index);
	/**
	 * Build and register `SM_<Name>` from a mesh description whose polygon
	 * groups are named after `MaterialKeys`, in slot order.
	 */
	UStaticMesh* CreateStaticMesh(const FString& Name, FMeshDescription& MeshDescription,
		const TArray<FString>& MaterialKeys, bool bSimpleCollision, FString& OutError);
	/** Reject meshes Unreal built badly, e.g. with NaN or empty bounds. */
	bool ValidateMeshes(FString& OutError);
	bool BuildLevel(const FApexTrackScene& Scene, FString& OutError);
	bool SaveTouchedPackages(FString& OutError);

	FString TrackFolder;
	FString TrackName;
	FString LevelPackage;
	/** The scene's season and spectators, for the variant picks. */
	FApexTrackDressing Dressing;
	/** The generated world, needed when saving its map package. */
	TObjectPtr<class UWorld> LevelWorld;

	/** Shared parent material, created once per import run. */
	TObjectPtr<UMaterialInterface> ParentMaterial;
	/** Material key -> generated instance. */
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;
	/** Mesh name -> generated static mesh. */
	TMap<FString, TObjectPtr<UStaticMesh>> Meshes;
	/** Prop kind -> generated stand-in mesh (ground pivot, metres at scale 1). */
	TMap<FString, TObjectPtr<UStaticMesh>> PropMeshes;
	/** Content root of the authored kit (`ApexPropImport`). */
	FString PropsRoot;
	/** Object path -> authored mesh, null for the ones that are not imported. */
	TMap<FString, TObjectPtr<UStaticMesh>> AuthoredMeshes;
	TObjectPtr<UMaterialInterface> EmissiveParent;
	TObjectPtr<UMaterialInterface> BrandParent;
	/** Key -> brand/marker/emissive slot override; a null entry means "tried, no texture". */
	TMap<FString, TObjectPtr<UMaterialInterface>> SlotMaterials;
	/** Texts with no brand or marker texture, logged once each. */
	TSet<FString> UnknownTexts;

	TArray<TObjectPtr<UPackage>> TouchedPackages;
};
