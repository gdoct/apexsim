#include "ApexTrackAssetBuilder.h"

#include "ApexTrackEditorModule.h"
#include "ApexTrackMaterialGraphs.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/**
	 * A package ready to receive a freshly generated asset.
	 *
	 * Re-importing a track after editing it is the normal case, not the
	 * exception, so an existing asset of the same name has to get out of the
	 * way. Worlds are the sharp edge: `UWorld::CreateWorld` into a package
	 * that already holds one fails outright trying to name its
	 * `WorldSettings`, and takes the process down with it.
	 */
	UPackage* MakePackage(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		Package->FullyLoad();

		const FString ObjectName = FPackageName::GetShortName(PackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *ObjectName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}
		return Package;
	}
}	 // namespace

FApexTrackAssetBuilder::FApexTrackAssetBuilder(const FString& DestRoot, const FString& TrackStem)
	: TrackFolder(DestRoot / TrackStem)
	, TrackName(TrackStem)
{
	LevelPackage = TrackFolder / (TEXT("L_") + TrackStem);
}

bool FApexTrackAssetBuilder::Build(const FApexTrackScene& Scene, FString& OutError)
{
	FApexTrackParents Parents;
	if (!ApexTrackMaterialGraphs::EnsureParents(Parents, OutError))
	{
		return false;
	}
	PurgeExistingAssets();

	FApexTrackSceneBuilder Builder(*this, Parents);
	FApexTrackGeometry Geometry;
	FApexTrackSceneBuilder::PrepareGeometry(Scene, Geometry);
	if (!Builder.BuildMaterials(Scene, OutError))
	{
		return false;
	}
	// No frame to spread it over here: all of it now.
	while (!Geometry.IsBuilt())
	{
		if (!Builder.BuildMeshes(Geometry, TNumericLimits<double>::Max(), OutError))
		{
			return false;
		}
	}
	if (!Builder.ValidateMeshes(OutError))
	{
		return false;
	}

	UPackage* Package = MakePackage(LevelPackage);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *LevelPackage);
		return false;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, /*bInformEngineOfWorld*/ false,
		*FPackageName::GetShortName(LevelPackage), Package, /*bAddToRoot*/ false);
	if (!World)
	{
		OutError = FString::Printf(TEXT("could not create world %s"), *LevelPackage);
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	LevelWorld = World;

	TArray<AActor*> Actors;
	Builder.SpawnActors(Scene, World, World->PersistentLevel, Actors);

	World->PostEditChange();
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	UE_LOG(LogApexTrackImport, Display, TEXT("    level %s: %d actor(s)"), *LevelPackage, Actors.Num());

	return SaveTouchedPackages(OutError);
}

void FApexTrackAssetBuilder::PurgeExistingAssets()
{
	// Delete the track's whole content folder before regenerating it.
	//
	// Mesh names are derived from the material keys and section spans in the
	// export, so editing a track can retire names as well as add them.
	// Overwriting in place would leave those orphans behind forever, still
	// referenced by nothing and still cooked into builds. This runs before
	// anything from the folder is loaded, so there is no in-memory state to
	// invalidate — which is also why it only makes sense in the commandlet,
	// on a fresh process.
	const FString Folder = FPackageName::LongPackageNameToFilename(TrackFolder);
	if (IFileManager::Get().DirectoryExists(*Folder))
	{
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		UE_LOG(LogApexTrackImport, Display, TEXT("    cleared previous assets in %s"), *TrackFolder);
	}
}

UMaterialInterface* FApexTrackAssetBuilder::MakeMaterial(
	const FString& Key, UMaterialInterface* Parent, const FApexMaterialParams& Params)
{
	if (!Parent)
	{
		return nullptr;
	}
	const FString PackageName = TrackFolder / (TEXT("MI_") + Key);
	UPackage* Package = MakePackage(PackageName);
	if (!Package)
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    could not create package %s"), *PackageName);
		return nullptr;
	}
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
		Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
	Instance->SetParentEditorOnly(Parent);
	for (const TPair<FName, float>& Pair : Params.Scalars)
	{
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Pair.Key), Pair.Value);
	}
	for (const TPair<FName, FLinearColor>& Pair : Params.Vectors)
	{
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Pair.Key), Pair.Value);
	}
	for (const TPair<FName, UTexture*>& Pair : Params.Textures)
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Pair.Key), Pair.Value);
	}
	Instance->PostEditChange();
	FAssetRegistryModule::AssetCreated(Instance);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	return Instance;
}

UStaticMesh* FApexTrackAssetBuilder::MakeMesh(const FString& Name, FMeshDescription& Description,
	const TArray<FName>& SlotNames, const TArray<UMaterialInterface*>& SlotMaterials, bool bTrackSurface,
	const FBox& Bounds)
{
	const FString PackageName = TrackFolder / (TEXT("SM_") + Name);
	UPackage* Package = MakePackage(PackageName);
	if (!Package)
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    could not create package %s"), *PackageName);
		return nullptr;
	}

	UStaticMesh* Mesh =
		NewObject<UStaticMesh>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
	for (int32 i = 0; i < SlotNames.Num(); ++i)
	{
		// The slot name matches the polygon group's, which is how the build
		// maps sections onto slots.
		Mesh->GetStaticMaterials().Add(
			FStaticMaterial(SlotMaterials.IsValidIndex(i) ? SlotMaterials[i] : nullptr, SlotNames[i]));
	}

	UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
	// Simple collision would be a box around a 250 m ribbon, which is worse
	// than none. The road needs its actual surface, so it uses the render
	// geometry directly (set below). Props are small enough for the box.
	BuildParams.bBuildSimpleCollision = !bTrackSurface;
	// Not the fast path. It leaves some meshes with NaN bounds — three of
	// Monza's ground patches, with perfectly finite vertices — and a mesh
	// with NaN bounds is culled from every view, so the level comes out with
	// holes that nothing in the export explains. The full build costs a
	// second or so per circuit and computes bounds, tangents and distance
	// fields properly; `ValidateMeshes` keeps it honest either way.
	BuildParams.bFastBuild = false;
	BuildParams.bMarkPackageDirty = false;
	BuildParams.bCommitMeshDescription = true;
	Mesh->BuildFromMeshDescriptions({&Description}, BuildParams);

	if (bTrackSurface)
	{
		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		}
	}
	// Nanite is deliberately left off (it is off by default, so there is
	// nothing to set): these meshes are cheap by design, and Nanite would
	// add build time and a memory floor for nothing.

	Mesh->PostEditChange();
	FAssetRegistryModule::AssetCreated(Mesh);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);
	return Mesh;
}

void FApexTrackAssetBuilder::LabelActor(AActor* Actor, const FString& Label)
{
	if (Actor)
	{
		Actor->SetActorLabel(Label);
	}
}

bool FApexTrackAssetBuilder::SaveTouchedPackages(FString& OutError)
{
	for (UPackage* Package : TouchedPackages)
	{
		if (!Package)
		{
			continue;
		}
		const bool bIsMap = Package->GetName() == LevelPackage;
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(),
			bIsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		// A map package has to be saved with its world as the asset, or it
		// lands on disk without one and the editor will not open it.
		UObject* Asset = bIsMap ? LevelWorld.Get() : nullptr;
		if (!UPackage::SavePackage(Package, Asset, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to save %s"), *FileName);
			return false;
		}
	}
	UE_LOG(LogApexTrackImport, Display, TEXT("    saved %d package(s)"), TouchedPackages.Num());
	return true;
}
