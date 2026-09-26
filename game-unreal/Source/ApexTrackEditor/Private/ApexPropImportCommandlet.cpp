#include "ApexPropImportCommandlet.h"

#include "Track/ApexPropLibrary.h"
#include "ApexTrackEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	bool PackageFileExists(const FString& PackageName);

	/**
	 * The engine's glTF pipeline stack, which the override below is built
	 * from. The second turns every material into an instance of the engine's
	 * glTF material library (importing them as materials of our own trips an
	 * engine error on the library's `AlphaMode` input). Those library
	 * parents carry neither the Nanite nor the instanced-mesh usage flag —
	 * fine in the editor, which sets usage on the fly, but a packaged build
	 * draws the default material instead — so every instance is re-parented
	 * onto a copy of its parent under `<dest>/_Parents` that has the flags.
	 */
	const TCHAR* kGltfAssetsPipeline =
		TEXT("/Interchange/Pipelines/DefaultGLTFAssetsPipeline.DefaultGLTFAssetsPipeline");
	const TCHAR* kGltfPipeline = TEXT("/Interchange/Pipelines/DefaultGLTFPipeline.DefaultGLTFPipeline");
	const TCHAR* kParentsFolder = TEXT("_Parents");

	/**
	 * Re-parent an imported instance onto a copy of its engine parent that
	 * carries the Nanite and instanced-mesh usage flags, made once per parent
	 * under `ParentsFolder` and reused across runs. Parameter overrides live
	 * on the instance by name, so they survive the swap.
	 *
	 * The instance's parent is not the material itself but one of the
	 * library's instances of it (`MI_Default_Mask_DS` and the like), and
	 * that middle link is where a glTF `alphaMode` and `doubleSided` live, as
	 * base property overrides. Skipping over it without carrying them across
	 * drew every foliage card opaque: black squares where the texture is
	 * transparent. So the chain's blend mode, sidedness and clip value, and
	 * any parameter a middle link set that the instance does not, are copied
	 * onto the instance first.
	 */
	bool ReparentOntoFlaggedCopy(UMaterialInstanceConstant* Instance, const FString& ParentsFolder,
		TSet<UPackage*>& OutPackages, FString& OutError)
	{
		UMaterial* Parent = Instance->GetMaterial();
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("%s has no parent material"), *Instance->GetName());
			return false;
		}
		if (Parent->GetOutermost()->GetName().StartsWith(ParentsFolder))
		{
			return true;
		}
		const FString CopyPackage = ParentsFolder / Parent->GetName();
		const FString CopyPath = CopyPackage + TEXT(".") + Parent->GetName();
		UMaterial* Copy = FindObject<UMaterial>(nullptr, *CopyPath);
		if (!Copy && PackageFileExists(CopyPackage))
		{
			Copy = LoadObject<UMaterial>(nullptr, *CopyPath);
		}
		if (!Copy)
		{
			UPackage* Package = CreatePackage(*CopyPackage);
			if (!Package)
			{
				OutError = FString::Printf(TEXT("could not create package %s"), *CopyPackage);
				return false;
			}
			Copy = DuplicateObject<UMaterial>(Parent, Package, *Parent->GetName());
			Copy->SetFlags(RF_Public | RF_Standalone);
			Copy->bUsedWithInstancedStaticMeshes = true;
			Copy->bUsedWithNanite = true;
			Copy->PostEditChange();
			FAssetRegistryModule::AssetCreated(Copy);
			Package->MarkPackageDirty();
			OutPackages.Add(Package);
			UE_LOG(LogApexTrackImport, Display, TEXT("    parent %s copied to %s with usage flags"),
				*Parent->GetPathName(), *CopyPackage);
		}
		const EBlendMode BlendMode = Instance->GetBlendMode();
		const bool bTwoSided = Instance->IsTwoSided();
		const float ClipValue = Instance->GetOpacityMaskClipValue();
		for (UMaterialInstance* Link = Cast<UMaterialInstance>(Instance->Parent); Link;
			 Link = Cast<UMaterialInstance>(Link->Parent))
		{
			for (const FScalarParameterValue& Value : Link->ScalarParameterValues)
			{
				if (!Instance->ScalarParameterValues.ContainsByPredicate(
						[&](const FScalarParameterValue& Own) { return Own.ParameterInfo == Value.ParameterInfo; }))
				{
					Instance->SetScalarParameterValueEditorOnly(Value.ParameterInfo, Value.ParameterValue);
				}
			}
			for (const FVectorParameterValue& Value : Link->VectorParameterValues)
			{
				if (!Instance->VectorParameterValues.ContainsByPredicate(
						[&](const FVectorParameterValue& Own) { return Own.ParameterInfo == Value.ParameterInfo; }))
				{
					Instance->SetVectorParameterValueEditorOnly(Value.ParameterInfo, Value.ParameterValue);
				}
			}
			for (const FTextureParameterValue& Value : Link->TextureParameterValues)
			{
				if (!Instance->TextureParameterValues.ContainsByPredicate(
						[&](const FTextureParameterValue& Own) { return Own.ParameterInfo == Value.ParameterInfo; }))
				{
					Instance->SetTextureParameterValueEditorOnly(Value.ParameterInfo, Value.ParameterValue);
				}
			}
		}
		Instance->SetParentEditorOnly(Copy);
		FMaterialInstanceBasePropertyOverrides& Overrides = Instance->BasePropertyOverrides;
		if (BlendMode != Copy->GetBlendMode())
		{
			Overrides.bOverride_BlendMode = true;
			Overrides.BlendMode = BlendMode;
		}
		if (bTwoSided != Copy->IsTwoSided())
		{
			Overrides.bOverride_TwoSided = true;
			Overrides.TwoSided = bTwoSided;
		}
		if (ClipValue != Copy->GetOpacityMaskClipValue())
		{
			Overrides.bOverride_OpacityMaskClipValue = true;
			Overrides.OpacityMaskClipValue = ClipValue;
		}
		Instance->PostEditChange();
		return true;
	}

	/** Folders under the kit that hold tools, previews and loose textures, not props. */
	bool IsKindFolder(const FString& Name)
	{
		return !Name.StartsWith(TEXT("_")) && ApexProps::FindKind(Name) != nullptr;
	}

	/** Whether the package's file is on disk right now (the asset registry may lag a deletion). */
	bool PackageFileExists(const FString& PackageName)
	{
		const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		return IFileManager::Get().FileExists(*File);
	}

	/**
	 * Take an asset out of the way: the same eviction the track builder uses
	 * for an asset whose name a fresh one is about to take.
	 */
	void Discard(UObject* Object)
	{
		if (!Object)
		{
			return;
		}
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->Rename(nullptr, GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
		Object->MarkAsGarbage();
	}

	/**
	 * Move an asset into the package `NewPackageName`, evicting whatever an
	 * earlier run left there. The old package is never saved, so nothing is
	 * left behind on disk.
	 */
	bool Relocate(UObject* Asset, const FString& NewPackageName, FString& OutError)
	{
		if (Asset->GetOutermost()->GetName() == NewPackageName)
		{
			return true;
		}
		UPackage* Package = CreatePackage(*NewPackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *NewPackageName);
			return false;
		}
		// Only a package that is still on disk has anything to evict; the
		// registry may still list one this run deleted, and loading that
		// logs a linker error.
		if (PackageFileExists(NewPackageName))
		{
			Package->FullyLoad();
		}
		const FString ObjectName = FPackageName::GetShortName(NewPackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *ObjectName))
		{
			if (Existing != Asset)
			{
				Discard(Existing);
			}
		}
		if (!Asset->Rename(*ObjectName, Package, REN_DontCreateRedirectors | REN_NonTransactional))
		{
			OutError = FString::Printf(TEXT("could not rename %s to %s"), *Asset->GetPathName(), *NewPackageName);
			return false;
		}
		Asset->SetFlags(RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return true;
	}

	/** An asset of `Class` already at `PackageName`, on disk or in memory. */
	template <typename T>
	T* FindExisting(const FString& PackageName)
	{
		const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		if (T* InMemory = FindObject<T>(nullptr, *ObjectPath))
		{
			return InMemory;
		}
		if (!PackageFileExists(PackageName))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *ObjectPath);
	}

	/** Every texture a material references: its own parameters, or its graph. */
	bool MaterialUsesTexture(UMaterialInterface* Material, UTexture* Texture)
	{
		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
			{
				if (Value.ParameterValue == Texture)
				{
					return true;
				}
			}
			return false;
		}
		if (UMaterial* Graph = Cast<UMaterial>(Material))
		{
			for (UMaterialExpression* Expression : Graph->GetExpressions())
			{
				if (UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression))
				{
					if (Sample->Texture == Texture)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	void ReplaceTexture(UMaterialInterface* Material, UTexture* From, UTexture* To)
	{
		bool bChanged = false;
		if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
		{
			for (FTextureParameterValue& Value : Instance->TextureParameterValues)
			{
				if (Value.ParameterValue == From)
				{
					Value.ParameterValue = To;
					bChanged = true;
				}
			}
		}
		else if (UMaterial* Graph = Cast<UMaterial>(Material))
		{
			for (UMaterialExpression* Expression : Graph->GetExpressions())
			{
				if (UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression))
				{
					if (Sample->Texture == From)
					{
						Sample->Texture = To;
						bChanged = true;
					}
				}
			}
		}
		if (bChanged)
		{
			Material->PostEditChange();
		}
	}

	/**
	 * Foliage cards and fence mesh must be masked and two-sided. The GLBs
	 * say so (`alphaMode: MASK`, `doubleSided`) and the glTF pipeline honours
	 * it; this is the belt to those braces.
	 */
	void EnforceMasked(UMaterialInterface* Material)
	{
		if (Material->GetBlendMode() == BLEND_Masked && Material->IsTwoSided())
		{
			return;
		}
		if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material))
		{
			Instance->BasePropertyOverrides.bOverride_BlendMode = true;
			Instance->BasePropertyOverrides.BlendMode = BLEND_Masked;
			Instance->BasePropertyOverrides.bOverride_TwoSided = true;
			Instance->BasePropertyOverrides.TwoSided = true;
			Instance->PostEditChange();
		}
		else if (UMaterial* Graph = Cast<UMaterial>(Material))
		{
			Graph->BlendMode = BLEND_Masked;
			Graph->TwoSided = true;
			Graph->PostEditChange();
		}
		UE_LOG(LogApexTrackImport, Warning, TEXT("    %s imported %s; forced masked and two-sided"),
			*Material->GetName(),
			Material->GetBlendMode() == BLEND_Masked ? TEXT("single-sided") : TEXT("opaque"));
	}
}	 // namespace

UApexPropImportCommandlet::UApexPropImportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

FString UApexPropImportCommandlet::DefaultSourceDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../content/props")));
}

bool UApexPropImportCommandlet::ParseOptions(const FString& Params, FOptions& Out, FString& OutError)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> Values;
	ParseCommandLine(*Params, Tokens, Switches, Values);

	Out.bAll = Switches.Contains(TEXT("all"));
	Out.bDryRun = Switches.Contains(TEXT("dryrun"));
	Out.SourceDir = Values.Contains(TEXT("source"))
		? FPaths::ConvertRelativePathToFull(Values[TEXT("source")])
		: DefaultSourceDir();
	Out.DestRoot = Values.Contains(TEXT("dest")) ? Values[TEXT("dest")] : FString(ApexProps::DefaultRoot);
	if (!Out.DestRoot.StartsWith(TEXT("/Game")))
	{
		OutError = FString::Printf(TEXT("-dest must be a content path starting with /Game, got \"%s\""), *Out.DestRoot);
		return false;
	}
	Out.DestRoot.RemoveFromEnd(TEXT("/"));

	auto Split = [](const FString& List, TArray<FString>& Into) {
		List.ParseIntoArray(Into, TEXT(","), true);
		for (FString& Item : Into)
		{
			Item.TrimStartAndEndInline();
		}
	};
	if (const FString* Kinds = Values.Find(TEXT("kind")))
	{
		Split(*Kinds, Out.Kinds);
	}
	if (const FString* Assets = Values.Find(TEXT("asset")))
	{
		Split(*Assets, Out.Assets);
	}
	for (const FString& Kind : Out.Kinds)
	{
		if (!ApexProps::FindKind(Kind) && Kind != ApexProps::DecalKind)
		{
			OutError = FString::Printf(TEXT("\"%s\" is not a prop kind (see docs/PROPS.md)"), *Kind);
			return false;
		}
	}
	for (const FString& Asset : Out.Assets)
	{
		FString Kind;
		FString Name;
		if (!Asset.Split(TEXT("/"), &Kind, &Name) || !ApexProps::FindKind(Kind) || Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("-asset wants kind/name, got \"%s\""), *Asset);
			return false;
		}
	}
	if (!Out.bAll && Out.Kinds.IsEmpty() && Out.Assets.IsEmpty())
	{
		OutError = TEXT("nothing to do: pass -all, -kind=NAME or -asset=kind/name");
		return false;
	}
	return true;
}

bool UApexPropImportCommandlet::CollectSources(
	const FOptions& Options, TArray<FSource>& OutSources, TSet<FString>& OutWholeKinds, FString& OutError)
{
	if (!IFileManager::Get().DirectoryExists(*Options.SourceDir))
	{
		OutError = FString::Printf(TEXT("no prop kit at %s"), *Options.SourceDir);
		return false;
	}

	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Options.SourceDir / TEXT("*")), false, true);
	Folders.Sort();
	for (const FString& Folder : Folders)
	{
		if (!IsKindFolder(Folder))
		{
			continue;
		}
		const bool bWhole = Options.bAll || Options.Kinds.Contains(Folder);
		if (bWhole)
		{
			OutWholeKinds.Add(Folder);
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Options.SourceDir / Folder / TEXT("*.glb")), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			const FString Asset = FPaths::GetBaseFilename(File);
			if (bWhole || Options.Assets.Contains(Folder + TEXT("/") + Asset))
			{
				OutSources.Add({Folder, Asset, Options.SourceDir / Folder / File});
			}
		}
	}

	for (const FString& Wanted : Options.Assets)
	{
		const bool bFound = OutSources.ContainsByPredicate(
			[&Wanted](const FSource& S) { return S.Kind + TEXT("/") + S.Asset == Wanted; });
		if (!bFound)
		{
			OutError = FString::Printf(TEXT("no %s.glb under %s"), *Wanted, *Options.SourceDir);
			return false;
		}
	}
	// `-kind=decal` alone imports PNGs and no GLB at all.
	if (OutSources.IsEmpty() && !Options.Kinds.Contains(ApexProps::DecalKind))
	{
		OutError = FString::Printf(TEXT("%s holds no GLBs for the requested kinds"), *Options.SourceDir);
		return false;
	}
	return true;
}

bool UApexPropImportCommandlet::ImportGlb(const FSource& Source, const FOptions& Options, FStats& Stats)
{
	const FString DestPath = Options.DestRoot / Source.Kind;
	const bool bFerris = Source.Kind == ApexProps::FerrisWheelKind && Source.Asset == ApexProps::FerrisWheelAsset;

	// The engine's glTF assets pipeline, adjusted: one mesh per GLB (two
	// for the ferris wheel, each in its own node frame so the rotor keeps
	// its hub pivot), the authored normals kept (the trees carry spherical
	// ones), Nanite on the big one-offs, a box for the camera traces to hit
	// on everything but foliage and the sky, and every material fresh so
	// the sharing below is decided here rather than by a registry search.
	UInterchangeGenericAssetsPipeline* Reference = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, kGltfAssetsPipeline);
	UInterchangeGenericAssetsPipeline* Pipeline = Reference
		? DuplicateObject(Reference, GetTransientPackage())
		: NewObject<UInterchangeGenericAssetsPipeline>(GetTransientPackage());
	if (!Reference)
	{
		UE_LOG(LogApexTrackImport, Warning, TEXT("    %s is missing; importing with a plain assets pipeline"), kGltfAssetsPipeline);
	}
	Pipeline->bUseSourceNameForAsset = true;
	Pipeline->bSceneNameSubFolder = false;
	Pipeline->bAssetTypeSubFolders = false;
	if (Pipeline->CommonMeshesProperties)
	{
		Pipeline->CommonMeshesProperties->bRecomputeNormals = false;
		Pipeline->CommonMeshesProperties->bRecomputeTangents = true;
		Pipeline->CommonMeshesProperties->bUseMikkTSpace = true;
		Pipeline->CommonMeshesProperties->bBakeMeshes = !bFerris;
	}
	if (Pipeline->MeshPipeline)
	{
		Pipeline->MeshPipeline->bImportStaticMeshes = true;
		Pipeline->MeshPipeline->CombineStaticMeshesBehavior = bFerris
			? EInterchangeCombineStaticMeshesBehavior::DoNotCombine
			: EInterchangeCombineStaticMeshesBehavior::All;
		Pipeline->MeshPipeline->bBuildNanite = ApexProps::IsNaniteKind(Source.Kind);
		Pipeline->MeshPipeline->bCollision = Source.Kind != TEXT("tree") && Source.Kind != TEXT("sky");
		Pipeline->MeshPipeline->bImportCollisionAccordingToMeshName = false;
		Pipeline->MeshPipeline->Collision = EInterchangeMeshCollision::Box;
		Pipeline->MeshPipeline->bGenerateLightmapUVs = false;
	}
	if (Pipeline->MaterialPipeline)
	{
		Pipeline->MaterialPipeline->bImportMaterials = true;
		Pipeline->MaterialPipeline->bReuseExistingMaterials = false;
		Pipeline->MaterialPipeline->MaterialImport = EInterchangeMaterialImportOption::ImportAsMaterialInstances;
	}

	FImportAssetParameters Params;
	Params.bIsAutomated = true;
	Params.bReplaceExisting = true;
	Params.OverridePipelines.Add(FSoftObjectPath(Pipeline));
	Params.OverridePipelines.Add(FSoftObjectPath(kGltfPipeline));
	if (!bFerris)
	{
		Params.DestinationName = TEXT("SM_") + Source.Asset;
	}

	// A fresh import every time, never a reimport: reimporting over the
	// existing mesh keeps its material slots and creates no materials at
	// all, and those slots point at the material folder just cleared. The
	// package names are the same afterwards, so the levels stay valid.
	{
		TArray<FString> Targets;
		if (bFerris)
		{
			Targets.Add(ApexProps::MeshPackageName(Options.DestRoot, Source.Kind, ApexProps::FerrisWheelAsset));
			Targets.Add(ApexProps::MeshPackageName(Options.DestRoot, Source.Kind, ApexProps::FerrisRotorAsset));
		}
		else
		{
			Targets.Add(ApexProps::MeshPackageName(Options.DestRoot, Source.Kind, Source.Asset));
		}
		TArray<FString> Deleted;
		for (const FString& Target : Targets)
		{
			const FString File = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*File))
			{
				IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true);
				Deleted.Add(File);
			}
		}
		// Interchange asks the registry whether the destination exists and
		// would try to load a file that is gone.
		if (!Deleted.IsEmpty())
		{
			IAssetRegistry::Get()->ScanModifiedAssetFiles(Deleted);
		}
	}

	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UE::Interchange::FScopedSourceData ScopedSourceData(Source.Path);
	if (!Manager.CanTranslateSourceData(ScopedSourceData.GetSourceData()))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    Interchange has no translator for %s"), *Source.Path);
		return false;
	}
	TArray<UObject*> Imported;
	if (!Manager.ImportAsset(DestPath, ScopedSourceData.GetSourceData(), Params, Imported))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    Interchange failed on %s"), *Source.Path);
		return false;
	}

	TArray<UStaticMesh*> Meshes;
	TArray<UMaterialInterface*> Materials;
	TArray<UTexture*> Textures;
	for (UObject* Object : Imported)
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			Meshes.Add(Mesh);
		}
		else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Object))
		{
			Materials.Add(Material);
		}
		else if (UTexture* Texture = Cast<UTexture>(Object))
		{
			Textures.Add(Texture);
		}
	}
	if (Meshes.IsEmpty())
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    %s produced no static mesh"), *Source.Path);
		return false;
	}

	// Names. A single-mesh GLB is already `SM_<asset>` through the
	// destination name; the ferris wheel's two come out as their node
	// names and are moved.
	FString Error;
	TSet<UPackage*> Packages;
	if (bFerris)
	{
		if (Meshes.Num() != 2)
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s should import as the wheel and its rotor, got %d mesh(es)"),
				*Source.Path, Meshes.Num());
			return false;
		}
		for (UStaticMesh* Mesh : Meshes)
		{
			const bool bRotor = Mesh->GetName().Contains(TEXT("rotor"));
			const FString Target = ApexProps::MeshPackageName(Options.DestRoot, Source.Kind,
				bRotor ? ApexProps::FerrisRotorAsset : ApexProps::FerrisWheelAsset);
			if (!Relocate(Mesh, Target, Error))
			{
				UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
				return false;
			}
		}
	}
	else
	{
		if (Meshes.Num() != 1)
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s imported as %d meshes; expected one combined mesh"),
				*Source.Path, Meshes.Num());
			return false;
		}
		const FString Target = ApexProps::MeshPackageName(Options.DestRoot, Source.Kind, Source.Asset);
		if (!Relocate(Meshes[0], Target, Error))
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
			return false;
		}
	}
	for (UStaticMesh* Mesh : Meshes)
	{
		Packages.Add(Mesh->GetOutermost());
	}

	if (!SettleMaterials(ApexProps::MaterialsFolder(Options.DestRoot, Source.Kind), Options.DestRoot / kParentsFolder,
			Meshes, Materials, Textures, Stats, Packages, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
		return false;
	}

	// What Unreal built, not what it was handed: an empty or unbounded mesh
	// is invisible in every level and nothing about the GLB would say why.
	for (UStaticMesh* Mesh : Meshes)
	{
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		const int32 Triangles = RenderData && !RenderData->LODResources.IsEmpty() ? Mesh->GetNumTriangles(0) : 0;
		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		const FVector Size = Bounds.BoxExtent * 2.0;
		if (Triangles <= 0 || Bounds.BoxExtent.ContainsNaN() || Bounds.SphereRadius <= 0.0f)
		{
			UE_LOG(LogApexTrackImport, Error, TEXT("    %s built empty (%d triangles)"), *Mesh->GetName(), Triangles);
			return false;
		}
		TArray<FString> Slots;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			Slots.Add(Slot.MaterialSlotName.ToString());
		}
		UE_LOG(LogApexTrackImport, Display, TEXT("    %s: %d tris, %.0f x %.0f x %.0f cm%s, slots %s"),
			*Mesh->GetName(), Triangles, Size.X, Size.Y, Size.Z,
			Mesh->IsNaniteEnabled() ? TEXT(", Nanite") : TEXT(""), *FString::Join(Slots, TEXT(" ")));
		++Stats.Meshes;
	}

	if (!SavePackages(Packages, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
		return false;
	}
	return true;
}

bool UApexPropImportCommandlet::SettleMaterials(const FString& MaterialsFolder, const FString& ParentsFolder,
	TArray<UStaticMesh*>& Meshes, TArray<UMaterialInterface*>& Materials, TArray<UTexture*>& Textures,
	FStats& Stats, TSet<UPackage*>& OutPackages, FString& OutError)
{
	// Materials are shared by name across a kind (`armco_galv` is on every
	// armco module): the first GLB to bring one in owns it, later ones are
	// pointed at it. Textures follow the materials that survive.
	TMap<UMaterialInterface*, UMaterialInterface*> Remap;
	TArray<UMaterialInterface*> Kept;
	for (UMaterialInterface* Material : Materials)
	{
		const FString Target = MaterialsFolder / Material->GetName();
		if (UMaterialInterface* Existing = FindExisting<UMaterialInterface>(Target))
		{
			if (Existing != Material)
			{
				Remap.Add(Material, Existing);
				Discard(Material);
				++Stats.MaterialsReused;
				continue;
			}
		}
		if (!Relocate(Material, Target, OutError))
		{
			return false;
		}
		Kept.Add(Material);
		++Stats.MaterialsImported;
	}
	for (UStaticMesh* Mesh : Meshes)
	{
		bool bChanged = false;
		for (FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			if (UMaterialInterface* const* Replacement = Remap.Find(Slot.MaterialInterface))
			{
				Slot.MaterialInterface = *Replacement;
				bChanged = true;
			}
		}
		if (bChanged)
		{
			Mesh->PostEditChange();
		}
	}

	for (UTexture* Texture : Textures)
	{
		TArray<UMaterialInterface*> Users;
		for (UMaterialInterface* Material : Kept)
		{
			if (MaterialUsesTexture(Material, Texture))
			{
				Users.Add(Material);
			}
		}
		if (Users.IsEmpty())
		{
			// Only a discarded duplicate material wanted it.
			Discard(Texture);
			continue;
		}
		const FString Target = MaterialsFolder / Texture->GetName();
		if (UTexture* Existing = FindExisting<UTexture>(Target))
		{
			if (Existing != Texture)
			{
				for (UMaterialInterface* Material : Users)
				{
					ReplaceTexture(Material, Texture, Existing);
				}
				Discard(Texture);
				++Stats.TexturesReused;
				continue;
			}
		}
		if (!Relocate(Texture, Target, OutError))
		{
			return false;
		}
		OutPackages.Add(Texture->GetOutermost());
		++Stats.TexturesImported;
	}

	for (UMaterialInterface* Material : Kept)
	{
		// The kit is drawn through instanced components and Nanite meshes;
		// a material without the matching usage flag is swapped for the
		// default material in a cooked build, silently.
		if (UMaterial* Graph = Cast<UMaterial>(Material))
		{
			Graph->bUsedWithInstancedStaticMeshes = true;
			Graph->bUsedWithNanite = true;
			Graph->PostEditChange();
		}
		else if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material))
		{
			if (!ReparentOntoFlaggedCopy(Instance, ParentsFolder, OutPackages, OutError))
			{
				return false;
			}
		}
		// After the re-parent, which is what once lost the mask.
		if (ApexProps::IsMaskedSlot(FName(*Material->GetName())))
		{
			EnforceMasked(Material);
		}
		OutPackages.Add(Material->GetOutermost());
	}
	return true;
}

UTexture2D* UApexPropImportCommandlet::ImportPng(const FString& PackageName, const FString& PngPath, FString& OutError)
{
	FImage Image;
	if (!FImageUtils::LoadImage(*PngPath, Image))
	{
		OutError = FString::Printf(TEXT("could not read %s"), *PngPath);
		return nullptr;
	}
	Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
		return nullptr;
	}
	Package->FullyLoad();
	const FString AssetName = FPackageName::GetShortName(PackageName);
	if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *AssetName))
	{
		Discard(Existing);
	}

	// World textures, unlike the catalog previews: mipmapped, streamed and
	// compressed like any other surface.
	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	Texture->Source.Init(Image);
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->MipGenSettings = TMGS_FromTextureGroup;
	Texture->UpdateResource();
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();
	return Texture;
}

bool UApexPropImportCommandlet::ImportLooseTextures(
	const FOptions& Options, FStats& Stats, const TSet<FString>& WholeKinds)
{
	struct FSet
	{
		/** The kind whose import brings the set along. */
		const TCHAR* Kind;
		const TCHAR* Folder;
		FString Dest;
		const TCHAR* Prefix;
	};
	TArray<FSet> Sets = {
		{TEXT("board"), TEXT("board/brands"), ApexProps::BrandsFolder(Options.DestRoot), TEXT("T_brand_")},
		{TEXT("board"), TEXT("board/markers"), ApexProps::MarkersFolder(Options.DestRoot), TEXT("T_marker_")},
		{TEXT("sign"), TEXT("sign/flags"), ApexProps::FlagsFolder(Options.DestRoot), TEXT("T_flag_")},
	};
	// Road decals: PNGs only, no GLB, so `decal` is not a kind folder and
	// is only ever asked for by name (`-kind=decal`) or by `-all`.
	TArray<FString> DecalFolders;
	TArray<FString> DecalPrefixes;
	for (const FString& Set : ApexProps::DecalSets())
	{
		DecalFolders.Add(FString(ApexProps::DecalKind) / Set);
		DecalPrefixes.Add(FString::Printf(TEXT("T_%s_"), *Set));
	}
	for (int32 i = 0; i < DecalFolders.Num(); ++i)
	{
		Sets.Add({ApexProps::DecalKind, *DecalFolders[i],
			ApexProps::DecalFolder(Options.DestRoot, ApexProps::DecalSets()[i]), *DecalPrefixes[i]});
	}
	bool bOk = true;
	TSet<UPackage*> Packages;
	for (const FSet& Set : Sets)
	{
		if (!Options.bAll && !WholeKinds.Contains(Set.Kind) && !Options.Kinds.Contains(Set.Kind))
		{
			continue;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Options.SourceDir / Set.Folder / TEXT("*.png")), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			const FString PackageName = Set.Dest / (Set.Prefix + FPaths::GetBaseFilename(File));
			UE_LOG(LogApexTrackImport, Display, TEXT("  %s <- %s/%s"), *PackageName, Set.Folder, *File);
			if (Options.bDryRun)
			{
				continue;
			}
			FString Error;
			if (UTexture2D* Texture = ImportPng(PackageName, Options.SourceDir / Set.Folder / File, Error))
			{
				Packages.Add(Texture->GetOutermost());
				++Stats.TexturesImported;
			}
			else
			{
				UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
				++Stats.Failures;
				bOk = false;
			}
		}
	}
	FString Error;
	if (!SavePackages(Packages, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("    %s"), *Error);
		return false;
	}
	return bOk;
}

bool UApexPropImportCommandlet::SavePackages(const TSet<UPackage*>& Packages, FString& OutError)
{
	for (UPackage* Package : Packages)
	{
		if (!Package)
		{
			continue;
		}
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			OutError = FString::Printf(TEXT("failed to save %s"), *FileName);
			return false;
		}
	}
	return true;
}

int32 UApexPropImportCommandlet::Main(const FString& Params)
{
	FOptions Options;
	FString Error;
	if (!ParseOptions(Params, Options, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}

	TArray<FSource> Sources;
	TSet<FString> WholeKinds;
	if (!CollectSources(Options, Sources, WholeKinds, Error))
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("%s"), *Error);
		return 1;
	}
	if (!Options.bDryRun && !UInterchangeManager::IsInterchangeImportEnabled())
	{
		UE_LOG(LogApexTrackImport, Error, TEXT("Interchange import is disabled (Interchange.FeatureFlags.Import.Enable)"));
		return 1;
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("Importing %d prop(s) from %s into %s%s"), Sources.Num(),
		*Options.SourceDir, *Options.DestRoot, Options.bDryRun ? TEXT(" (dry run)") : TEXT(""));

	// A kind imported wholesale starts from a clean material folder, so a
	// surface the GLBs no longer use is not carried along forever. Files
	// only: nothing from the folder has been loaded yet in this process.
	if (!Options.bDryRun)
	{
		for (const FString& Kind : WholeKinds)
		{
			const FString Folder = FPackageName::LongPackageNameToFilename(
				ApexProps::MaterialsFolder(Options.DestRoot, Kind));
			if (IFileManager::Get().DirectoryExists(*Folder))
			{
				IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
				UE_LOG(LogApexTrackImport, Display, TEXT("  cleared %s"), *ApexProps::MaterialsFolder(Options.DestRoot, Kind));
			}
		}
	}

	FStats Stats;
	for (const FSource& Source : Sources)
	{
		UE_LOG(LogApexTrackImport, Display, TEXT("  %s/%s <- %s"), *Source.Kind, *Source.Asset, *Source.Path);
		if (Options.bDryRun)
		{
			continue;
		}
		if (!ImportGlb(Source, Options, Stats))
		{
			++Stats.Failures;
		}
	}

	if (Options.bAll || WholeKinds.Contains(TEXT("board")) || WholeKinds.Contains(TEXT("sign"))
		|| Options.Kinds.Contains(ApexProps::DecalKind))
	{
		ImportLooseTextures(Options, Stats, WholeKinds);
	}

	UE_LOG(LogApexTrackImport, Display,
		TEXT("Props: %d mesh(es), %d material(s) imported and %d shared, %d texture(s) imported and %d shared, %d failure(s)"),
		Stats.Meshes, Stats.MaterialsImported, Stats.MaterialsReused, Stats.TexturesImported, Stats.TexturesReused,
		Stats.Failures);
	return Stats.Failures > 0 ? 1 : 0;
}
