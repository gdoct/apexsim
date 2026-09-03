#include "ApexTrackAssetBuilder.h"

#include "ApexTrackEditorModule.h"
#include "ApexTrackSceneData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "StaticMeshResources.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "GameFramework/PlayerStart.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/** Engine placeholder used for props until real assets exist. */
	const TCHAR* kPlaceholderPropMesh = TEXT("/Engine/BasicShapes/Cube.Cube");

	/**
	 * Rough blockout size per prop kind, in centimeters.
	 *
	 * The export names an asset key per prop but the project has no art for
	 * any of them yet, so each becomes a scaled engine cube. A tree-sized
	 * box in the right place is worth a great deal when you are checking
	 * whether a circuit feels right; a uniform cube everywhere is not.
	 */
	FVector PlaceholderPropSize(const FString& Kind)
	{
		if (Kind == TEXT("tree"))
		{
			return FVector(150.0f, 150.0f, 800.0f);
		}
		if (Kind == TEXT("barrier") || Kind == TEXT("tire_wall"))
		{
			return FVector(400.0f, 60.0f, 100.0f);
		}
		if (Kind == TEXT("grandstand"))
		{
			return FVector(3000.0f, 1200.0f, 900.0f);
		}
		if (Kind == TEXT("building"))
		{
			return FVector(1500.0f, 1000.0f, 800.0f);
		}
		if (Kind == TEXT("sign"))
		{
			return FVector(30.0f, 200.0f, 250.0f);
		}
		if (Kind == TEXT("light"))
		{
			return FVector(40.0f, 40.0f, 1200.0f);
		}
		if (Kind == TEXT("cone"))
		{
			return FVector(40.0f, 40.0f, 60.0f);
		}
		return FVector(100.0f, 100.0f, 100.0f);
	}

	/** Cubes are 100 cm; placeholder sizes are absolute, so convert. */
	FVector PlaceholderPropScale(const FString& Kind, float PropScale)
	{
		return PlaceholderPropSize(Kind) * (PropScale / 100.0f);
	}

	/** Object name from an asset path, i.e. the part after the last slash. */
	FString ObjectNameOf(const FString& PackageName)
	{
		FString Left;
		FString Right;
		return PackageName.Split(TEXT("/"), &Left, &Right, ESearchCase::CaseSensitive,
				   ESearchDir::FromEnd)
			? Right
			: PackageName;
	}

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

		const FString ObjectName = ObjectNameOf(PackageName);
		if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *ObjectName))
		{
			Existing->ClearFlags(RF_Public | RF_Standalone);
			Existing->Rename(nullptr, GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}
		return Package;
	}

	/** Create a material expression and register it with the material. */
	template <typename T>
	T* AddExpr(UMaterial* Material)
	{
		T* Expression = NewObject<T>(Material);
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/**
	 * Generated material for one prop kind, keyed `prop_<kind>`.
	 *
	 * Without these the placeholder cubes render in the engine's default
	 * checker grey, which is most of why a circuit lined with thousands of
	 * them reads as a paste-up rather than a place.
	 */
	struct FPropMaterialSpec
	{
		const TCHAR* Key;
		FLinearColor Color;
		float Roughness;
		float NoiseAmount;
		float NoiseScale;
		/** 0 disables the stripe. In cube-face UV units (0..1), not meters. */
		float StripePeriod;
		FLinearColor Secondary;
	};

	const FPropMaterialSpec kPropMaterials[] = {
		// Tire walls and barriers as red/white safety blocks: four stripes
		// across each cube face, which on a 12 m wall segment is a 3 m block.
		{TEXT("prop_tire_wall"), FLinearColor(0.42f, 0.05f, 0.04f), 0.85f, 0.06f, 0.001f, 0.26f,
			FLinearColor(0.80f, 0.78f, 0.74f)},
		{TEXT("prop_barrier"), FLinearColor(0.42f, 0.05f, 0.04f), 0.85f, 0.06f, 0.001f, 0.26f,
			FLinearColor(0.80f, 0.78f, 0.74f)},
		{TEXT("prop_grandstand"), FLinearColor(0.15f, 0.16f, 0.20f), 0.55f, 0.15f, 0.002f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_building"), FLinearColor(0.42f, 0.39f, 0.34f), 0.80f, 0.20f, 0.002f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_tree"), FLinearColor(0.06f, 0.18f, 0.05f), 1.00f, 0.50f, 0.010f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_sign"), FLinearColor(0.80f, 0.80f, 0.84f), 0.40f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_light"), FLinearColor(0.30f, 0.30f, 0.32f), 0.50f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_cone"), FLinearColor(0.85f, 0.30f, 0.03f), 0.60f, 0.00f, 0.002f, 0.0f,
			FLinearColor::Black},
		{TEXT("prop_default"), FLinearColor(0.45f, 0.45f, 0.47f), 0.70f, 0.10f, 0.002f, 0.0f,
			FLinearColor::Black},
	};

}	 // namespace

FApexTrackAssetBuilder::FApexTrackAssetBuilder(const FString& DestRoot, const FString& TrackStem)
	: TrackFolder(DestRoot / TrackStem)
	, TrackName(TrackStem)
{
	LevelPackage = TrackFolder / (TEXT("L_") + TrackStem);
}

bool FApexTrackAssetBuilder::Build(const FApexTrackScene& Scene, FString& OutError)
{
	PurgeExistingAssets();
	return BuildMaterials(Scene, OutError) && BuildMeshes(Scene, OutError)
		&& BuildLevel(Scene, OutError) && SaveTouchedPackages(OutError);
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

bool FApexTrackAssetBuilder::BuildMaterials(const FApexTrackScene& Scene, FString& OutError)
{
	// One parent material, then an instance per key. The project has no
	// authored track materials yet, so the parent carries enough procedural
	// detail to keep a circuit from reading as poster paint: an optional
	// stripe over the `u` coordinate (curbs, tire walls), and world-space
	// noise breaking up albedo and roughness. Swap `ParentMaterial` for a
	// hand-authored asset later and the instances keep working, as long as
	// it exposes the same parameters.
	const FString ParentPackageName = TrackFolder / TEXT("M_ApexTrackBase");
	UPackage* ParentPackage = MakePackage(ParentPackageName);
	if (!ParentPackage)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *ParentPackageName);
		return false;
	}

	UMaterial* Parent = NewObject<UMaterial>(
		ParentPackage, *ObjectNameOf(ParentPackageName), RF_Public | RF_Standalone);
	if (!Parent)
	{
		OutError = TEXT("could not create the parent material");
		return false;
	}

	UMaterialExpressionVectorParameter* ColorParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	ColorParam->ParameterName = TEXT("BaseColor");
	ColorParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

	UMaterialExpressionVectorParameter* SecondaryParam =
		AddExpr<UMaterialExpressionVectorParameter>(Parent);
	SecondaryParam->ParameterName = TEXT("SecondaryColor");
	SecondaryParam->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);

	UMaterialExpressionScalarParameter* RoughnessParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessParam->ParameterName = TEXT("Roughness");
	RoughnessParam->DefaultValue = 0.85f;

	// A period `u` never reaches, so stripes are off unless an instance
	// asks for them (0 would divide by zero in the graph below).
	UMaterialExpressionScalarParameter* StripeParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	StripeParam->ParameterName = TEXT("StripePeriod");
	StripeParam->DefaultValue = 1.0e6f;

	UMaterialExpressionScalarParameter* NoiseAmountParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	NoiseAmountParam->ParameterName = TEXT("NoiseAmount");
	NoiseAmountParam->DefaultValue = 0.0f;

	// Cycles per centimeter of world space.
	UMaterialExpressionScalarParameter* NoiseScaleParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	NoiseScaleParam->ParameterName = TEXT("NoiseScale");
	NoiseScaleParam->DefaultValue = 0.002f;

	UMaterialExpressionScalarParameter* RoughnessNoiseParam =
		AddExpr<UMaterialExpressionScalarParameter>(Parent);
	RoughnessNoiseParam->ParameterName = TEXT("RoughnessNoise");
	RoughnessNoiseParam->DefaultValue = 0.0f;

	// Stripe mask: fmod(floor(u / period), 2) alternates 0/1 along `u`,
	// which the bake emits as meters of station for track strips and which
	// is plain 0..1 face UVs on the placeholder prop cubes.
	UMaterialExpressionTextureCoordinate* TexCoord =
		AddExpr<UMaterialExpressionTextureCoordinate>(Parent);
	UMaterialExpressionComponentMask* MaskU = AddExpr<UMaterialExpressionComponentMask>(Parent);
	MaskU->Input.Expression = TexCoord;
	MaskU->R = 1;
	MaskU->G = 0;
	MaskU->B = 0;
	MaskU->A = 0;
	UMaterialExpressionDivide* StripeU = AddExpr<UMaterialExpressionDivide>(Parent);
	StripeU->A.Expression = MaskU;
	StripeU->B.Expression = StripeParam;
	UMaterialExpressionFloor* StripeIndex = AddExpr<UMaterialExpressionFloor>(Parent);
	StripeIndex->Input.Expression = StripeU;
	UMaterialExpressionConstant* Two = AddExpr<UMaterialExpressionConstant>(Parent);
	Two->R = 2.0f;
	UMaterialExpressionFmod* Stripe = AddExpr<UMaterialExpressionFmod>(Parent);
	Stripe->A.Expression = StripeIndex;
	Stripe->B.Expression = Two;
	UMaterialExpressionLinearInterpolate* Paint =
		AddExpr<UMaterialExpressionLinearInterpolate>(Parent);
	Paint->A.Expression = ColorParam;
	Paint->B.Expression = SecondaryParam;
	Paint->Alpha.Expression = Stripe;

	// World-space turbulence, recentred to ±0.5 so instances can scale it.
	UMaterialExpressionWorldPosition* WorldPos = AddExpr<UMaterialExpressionWorldPosition>(Parent);
	UMaterialExpressionMultiply* NoisePos = AddExpr<UMaterialExpressionMultiply>(Parent);
	NoisePos->A.Expression = WorldPos;
	NoisePos->B.Expression = NoiseScaleParam;
	UMaterialExpressionNoise* NoiseExpr = AddExpr<UMaterialExpressionNoise>(Parent);
	NoiseExpr->Position.Expression = NoisePos;
	NoiseExpr->Scale = 1.0f;
	NoiseExpr->bTurbulence = true;
	NoiseExpr->Levels = 3;
	NoiseExpr->OutputMin = 0.0f;
	NoiseExpr->OutputMax = 1.0f;
	UMaterialExpressionAdd* NoiseCentered = AddExpr<UMaterialExpressionAdd>(Parent);
	NoiseCentered->A.Expression = NoiseExpr;
	NoiseCentered->ConstB = -0.5f;

	// Albedo: the striped paint, brightened or darkened by the noise.
	UMaterialExpressionMultiply* Mottle = AddExpr<UMaterialExpressionMultiply>(Parent);
	Mottle->A.Expression = NoiseCentered;
	Mottle->B.Expression = NoiseAmountParam;
	UMaterialExpressionAdd* Brightness = AddExpr<UMaterialExpressionAdd>(Parent);
	Brightness->A.Expression = Mottle;
	Brightness->ConstB = 1.0f;
	UMaterialExpressionMultiply* Albedo = AddExpr<UMaterialExpressionMultiply>(Parent);
	Albedo->A.Expression = Paint;
	Albedo->B.Expression = Brightness;

	// Roughness: the base value, wobbled by the same noise so sheen varies
	// with the mottling instead of against it.
	UMaterialExpressionMultiply* RoughWobble = AddExpr<UMaterialExpressionMultiply>(Parent);
	RoughWobble->A.Expression = NoiseCentered;
	RoughWobble->B.Expression = RoughnessNoiseParam;
	UMaterialExpressionAdd* RoughSum = AddExpr<UMaterialExpressionAdd>(Parent);
	RoughSum->A.Expression = RoughnessParam;
	RoughSum->B.Expression = RoughWobble;
	UMaterialExpressionClamp* RoughOut = AddExpr<UMaterialExpressionClamp>(Parent);
	RoughOut->Input.Expression = RoughSum;
	RoughOut->MinDefault = 0.05f;
	RoughOut->MaxDefault = 1.0f;

	UMaterialEditorOnlyData* EditorOnly = Parent->GetEditorOnlyData();
	EditorOnly->BaseColor.Expression = Albedo;
	EditorOnly->Roughness.Expression = RoughOut;
	Parent->PostEditChange();

	FAssetRegistryModule::AssetCreated(Parent);
	ParentPackage->MarkPackageDirty();
	TouchedPackages.Add(ParentPackage);
	ParentMaterial = Parent;

	auto MakeInstance = [&](const FString& Key) -> UMaterialInstanceConstant* {
		const FString PackageName = TrackFolder / (TEXT("MI_") + Key);
		UPackage* Package = MakePackage(PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
			return nullptr;
		}
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(
			Package, *ObjectNameOf(PackageName), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(Parent);
		return Instance;
	};
	auto FinishInstance = [&](UMaterialInstanceConstant* Instance, const FString& Key) {
		Instance->PostEditChange();
		FAssetRegistryModule::AssetCreated(Instance);
		UPackage* Package = Instance->GetOutermost();
		Package->MarkPackageDirty();
		TouchedPackages.Add(Package);
		Materials.Add(Key, Instance);
	};
	auto SetScalar = [](UMaterialInstanceConstant* Instance, const TCHAR* Name, float Value) {
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
	};
	auto SetVector = [](
						 UMaterialInstanceConstant* Instance, const TCHAR* Name, FLinearColor Value) {
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(Name), Value);
	};

	for (const FApexTrackMaterial& Source : Scene.Materials)
	{
		UMaterialInstanceConstant* Instance = MakeInstance(Source.Key);
		if (!Instance)
		{
			return false;
		}

		// The exporter's colors are editor-viewport colors: bright, because
		// the viewport is flat-lit. Under a physically lit sun they render
		// as pastel — real asphalt reflects ~10% of what hits it, grass not
		// much more — so each family scales down toward plausible albedo.
		FLinearColor Base = Source.BaseColor;

		if (Source.Family == TEXT("road") || Source.Family == TEXT("pit_lane"))
		{
			// Tarmac: patchy aggregate and uneven sheen, not one flat slab.
			Base *= 0.42f;
			SetScalar(Instance, TEXT("Roughness"), 0.9f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.35f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.0012f);
			SetScalar(Instance, TEXT("RoughnessNoise"), 0.2f);
		}
		else if (Source.Family == TEXT("curb"))
		{
			// The exporter's solid slab becomes 2 m stripes. Style keys name
			// their pair (`curb_red_white`, `curb_yellow_black`): the base
			// color is the first, so the alternate is white unless the key
			// says black.
			Base *= 0.7f;
			SetScalar(Instance, TEXT("StripePeriod"), 2.0f);
			const FLinearColor Alt = Source.Key.EndsWith(TEXT("black"))
				? FLinearColor(0.04f, 0.04f, 0.04f)
				: FLinearColor(0.62f, 0.60f, 0.57f);
			SetVector(Instance, TEXT("SecondaryColor"), Alt);
			SetScalar(Instance, TEXT("Roughness"), 0.6f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.08f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.004f);
		}
		else if (Source.Family == TEXT("surface"))
		{
			// Grass, gravel, sand and the terrain itself: desaturated a
			// touch, darkened a lot, and mottled in ~10 m patches — the
			// large scale is what still reads a hundred meters out.
			const float Luma = Base.GetLuminance();
			Base = FMath::Lerp(Base, FLinearColor(Luma, Luma, Luma), 0.15f) * 0.33f;
			SetScalar(Instance, TEXT("Roughness"), 0.95f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.5f);
			SetScalar(Instance, TEXT("NoiseScale"), 0.0008f);
			SetScalar(Instance, TEXT("RoughnessNoise"), 0.08f);
		}
		else if (Source.Family == TEXT("marking"))
		{
			// Painted lines read as paint, not asphalt — worn paint, so a
			// touch of the same mottling the road gets.
			Base *= 0.85f;
			SetScalar(Instance, TEXT("Roughness"), 0.45f);
			SetScalar(Instance, TEXT("NoiseAmount"), 0.08f);
		}
		Base.A = 1.0f;
		SetVector(Instance, TEXT("BaseColor"), Base);
		FinishInstance(Instance, Source.Key);
	}

	for (const FPropMaterialSpec& Spec : kPropMaterials)
	{
		UMaterialInstanceConstant* Instance = MakeInstance(Spec.Key);
		if (!Instance)
		{
			return false;
		}
		SetVector(Instance, TEXT("BaseColor"), Spec.Color);
		SetScalar(Instance, TEXT("Roughness"), Spec.Roughness);
		SetScalar(Instance, TEXT("NoiseAmount"), Spec.NoiseAmount);
		SetScalar(Instance, TEXT("NoiseScale"), Spec.NoiseScale);
		if (Spec.StripePeriod > 0.0f)
		{
			SetScalar(Instance, TEXT("StripePeriod"), Spec.StripePeriod);
			SetVector(Instance, TEXT("SecondaryColor"), Spec.Secondary);
		}
		FinishInstance(Instance, Spec.Key);
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("    generated %d material instance(s)"),
		Materials.Num());
	return true;
}

bool FApexTrackAssetBuilder::BuildMeshes(const FApexTrackScene& Scene, FString& OutError)
{
	for (const FApexTrackMesh& Source : Scene.Meshes)
	{
		const FString PackageName = TrackFolder / (TEXT("SM_") + Source.Name);
		UPackage* Package = MakePackage(PackageName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("could not create package %s"), *PackageName);
			return false;
		}

		FMeshDescription MeshDescription;
		FStaticMeshAttributes Attributes(MeshDescription);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

		const int32 VertexCount = Source.Positions.Num();
		MeshDescription.ReserveNewVertices(VertexCount);
		MeshDescription.ReserveNewVertexInstances(Source.Indices.Num());
		MeshDescription.ReserveNewTriangles(Source.NumTriangles());
		MeshDescription.ReserveNewPolygons(Source.NumTriangles());

		const FPolygonGroupID PolygonGroup = MeshDescription.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[PolygonGroup] = FName(*Source.MaterialKey);

		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(VertexCount);
		for (int32 i = 0; i < VertexCount; ++i)
		{
			const FVertexID VertexID = MeshDescription.CreateVertex();
			Positions[VertexID] = Source.Positions[i];
			VertexIDs.Add(VertexID);
		}

		// Vertex order is taken exactly as exported. The bake already wound
		// every triangle to Unreal's front-face convention and checks it in
		// its own test suite, so reversing anything here would undo that.
		TArray<FVertexInstanceID> Corners;
		Corners.SetNum(3);
		for (int32 Tri = 0; Tri < Source.Indices.Num(); Tri += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Index = Source.Indices[Tri + Corner];
				const FVertexInstanceID InstanceID =
					MeshDescription.CreateVertexInstance(VertexIDs[Index]);
				Normals[InstanceID] = Source.Normals[Index];
				UVs.Set(InstanceID, 0, Source.UVs[Index]);
				Corners[Corner] = InstanceID;
			}
			MeshDescription.CreatePolygon(PolygonGroup, Corners);
		}

		UStaticMesh* Mesh = NewObject<UStaticMesh>(
			Package, *ObjectNameOf(PackageName), RF_Public | RF_Standalone);
		Mesh->GetStaticMaterials().Add(
			FStaticMaterial(Materials.FindRef(Source.MaterialKey), FName(*Source.MaterialKey)));

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		// Simple collision would be a box around a 250 m ribbon, which is
		// worse than none. The road needs its actual surface, so it uses the
		// render geometry directly (set below).
		BuildParams.bBuildSimpleCollision = false;
		// Not the fast path. It leaves some meshes with NaN bounds — three
		// of Monza's ground patches, with perfectly finite vertices — and a
		// mesh with NaN bounds is culled from every view, so the level comes
		// out with holes that nothing in the export explains. The full build
		// costs a second or so per circuit and computes bounds and tangents
		// properly. `ValidateMeshes` keeps it honest either way.
		BuildParams.bFastBuild = false;
		BuildParams.bMarkPackageDirty = false;
		BuildParams.bCommitMeshDescription = true;
		Mesh->BuildFromMeshDescriptions({&MeshDescription}, BuildParams);

		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		}
		// Nanite is deliberately left off (it is off by default, so there is
		// nothing to set). A whole circuit is around 35k triangles — the
		// entire point of these meshes is that they are cheap — and Nanite
		// would add build time and a memory floor for nothing. Revisit when
		// the ribbons carry real displaced detail.

		Mesh->PostEditChange();
		FAssetRegistryModule::AssetCreated(Mesh);
		Package->MarkPackageDirty();
		TouchedPackages.Add(Package);
		Meshes.Add(Source.Name, Mesh);
	}

	UE_LOG(LogApexTrackImport, Display, TEXT("    generated %d static mesh(es)"), Meshes.Num());
	return ValidateMeshes(OutError);
}

bool FApexTrackAssetBuilder::ValidateMeshes(FString& OutError)
{
	// Check what Unreal actually built, not what was handed to it. A mesh
	// with NaN or empty bounds is invisible at runtime — it fails every
	// frustum test — and nothing about the export would tell you why. Better
	// to fail the import than to ship a circuit with holes in it.
	for (const TPair<FString, TObjectPtr<UStaticMesh>>& Entry : Meshes)
	{
		const UStaticMesh* Mesh = Entry.Value;
		// Note `LODResources`, not `IsInitialized()`: the latter means the
		// RHI resources are live, which never happens in a commandlet.
		const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
		if (!RenderData || RenderData->LODResources.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s built no render data"), *Entry.Key);
			return false;
		}

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		if (Bounds.Origin.ContainsNaN() || Bounds.BoxExtent.ContainsNaN()
			|| !FMath::IsFinite(Bounds.SphereRadius))
		{
			OutError = FString::Printf(TEXT("%s built with non-finite bounds"), *Entry.Key);
			return false;
		}
		if (Bounds.SphereRadius <= 0.0f)
		{
			OutError = FString::Printf(TEXT("%s built with empty bounds"), *Entry.Key);
			return false;
		}

		const int32 Triangles = Mesh->GetNumTriangles(0);
		if (Triangles <= 0)
		{
			OutError = FString::Printf(TEXT("%s built with no triangles"), *Entry.Key);
			return false;
		}
	}
	return true;
}

bool FApexTrackAssetBuilder::BuildLevel(const FApexTrackScene& Scene, FString& OutError)
{
	UPackage* Package = MakePackage(LevelPackage);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("could not create package %s"), *LevelPackage);
		return false;
	}

	UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, /*bInformEngineOfWorld*/ false,
		*ObjectNameOf(LevelPackage), Package, /*bAddToRoot*/ false);
	if (!World)
	{
		OutError = FString::Printf(TEXT("could not create world %s"), *LevelPackage);
		return false;
	}
	World->SetFlags(RF_Public | RF_Standalone);
	LevelWorld = World;

	FActorSpawnParameters SpawnParams;
	SpawnParams.OverrideLevel = World->PersistentLevel;
	SpawnParams.ObjectFlags = RF_Transactional;

	// Track geometry: one static actor per baked mesh.
	int32 MeshActors = 0;
	for (const FApexTrackMesh& Source : Scene.Meshes)
	{
		UStaticMesh* Mesh = Meshes.FindRef(Source.Name);
		if (!Mesh)
		{
			continue;
		}
		SpawnParams.Name = MakeUniqueObjectName(
			World->PersistentLevel, AStaticMeshActor::StaticClass(), FName(*Source.Name));
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!Actor)
		{
			continue;
		}
		Actor->SetActorLabel(Source.Name);
		// Positions are baked in world space, so the actor stays at the
		// origin and the mesh carries the layout.
		// Movable, despite never moving. Static geometry wants baked lighting,
		// and nobody is going to run a lighting build on 26 regenerated
		// circuits — the level just loads complaining about hundreds of
		// unbuilt objects. Movable puts it on dynamic lighting instead, which
		// is what a procedurally generated level can actually rely on.
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		// Movable is a lighting decision, not a promise to move, and virtual
		// shadow maps read it as one: a movable primitive is cached as
		// dynamic, so a whole circuit — sixty-odd 384 m ground patches plus
		// the grass and gravel — re-marks its shadow pages every frame and
		// overflows the non-Nanite marking job queue (the "[VSM] Non-Nanite
		// Marking Job Queue overflow" warning on screen). Saying the shape
		// never moves puts it back in the static page cache.
		Actor->GetStaticMeshComponent()->ShadowCacheInvalidationBehavior =
			EShadowCacheInvalidationBehavior::Static;
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		++MeshActors;
	}

	// Props, as scaled placeholder boxes.
	UStaticMesh* PlaceholderMesh =
		LoadObject<UStaticMesh>(nullptr, kPlaceholderPropMesh);
	int32 PropActors = 0;
	if (PlaceholderMesh)
	{
		for (int32 i = 0; i < Scene.Props.Num(); ++i)
		{
			const FApexTrackProp& Prop = Scene.Props[i];
			SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
				AStaticMeshActor::StaticClass(), FName(*FString::Printf(TEXT("Prop_%d"), i)));
			const FVector Scale = PlaceholderPropScale(Prop.Kind, Prop.Scale);
			// The box pivot is centred; lift it so props sit on the ground
			// rather than half-buried.
			const FVector Location = Prop.Location + FVector(0.0f, 0.0f, Scale.Z * 0.5f);
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
				Location, FRotator(0.0f, Prop.YawDeg, 0.0f), SpawnParams);
			if (!Actor)
			{
				continue;
			}
			Actor->SetActorLabel(FString::Printf(TEXT("%s_%s"), *Prop.Kind, *Prop.Asset));
			Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			// Props are scenery bolted to the ground; same shadow-cache
			// reasoning as the track meshes above.
			Actor->GetStaticMeshComponent()->ShadowCacheInvalidationBehavior =
				EShadowCacheInvalidationBehavior::Static;
			Actor->GetStaticMeshComponent()->SetStaticMesh(PlaceholderMesh);
			UMaterialInterface* PropMaterial = Materials.FindRef(TEXT("prop_") + Prop.Kind);
			if (!PropMaterial)
			{
				PropMaterial = Materials.FindRef(TEXT("prop_default"));
			}
			if (PropMaterial)
			{
				Actor->GetStaticMeshComponent()->SetMaterial(0, PropMaterial);
			}
			Actor->SetActorScale3D(Scale / 100.0f);
			++PropActors;
		}
	}
	else
	{
		UE_LOG(LogApexTrackImport, Warning,
			TEXT("    %s is missing — props were skipped"), kPlaceholderPropMesh);
	}

	// Starting grid. These match the slots the server computes, so a car
	// spawned here sits where the simulation thinks it is.
	for (const FApexTrackGridSlot& Slot : Scene.Grid)
	{
		SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
			APlayerStart::StaticClass(), FName(*FString::Printf(TEXT("Grid_%d"), Slot.Position)));
		APlayerStart* Start = World->SpawnActor<APlayerStart>(
			Slot.Location, FRotator(0.0f, Slot.YawDeg, 0.0f), SpawnParams);
		if (Start)
		{
			Start->SetActorLabel(FString::Printf(TEXT("GridSlot_%02d"), Slot.Position));
		}
	}

	// Deliberately no sun and no sky light.
	//
	// A track level is streamed into the menu world rather than travelled to,
	// so it inherits that world's lighting. Adding its own gives two
	// directional lights in one scene, which Unreal resolves by picking one
	// and warning about the rest. The race director re-tunes that world's
	// sun instead. If these levels ever get opened standalone they will need
	// lighting of their own, but not from here.
	//
	// Fog and post-processing are different: the menu world has neither, and
	// both are what give a five-kilometer circuit any sense of depth.
	SpawnParams.Name = MakeUniqueObjectName(
		World->PersistentLevel, AExponentialHeightFog::StaticClass(), FName(TEXT("TrackFog")));
	if (AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		Fog->SetActorLabel(TEXT("TrackFog"));
		UExponentialHeightFogComponent* FogComponent = Fog->GetComponent();
		// Thin: enough that the far end of a straight sits in haze rather
		// than pin-sharp against the sky, not enough to milk out the
		// mid-field (0.0045 did, and with it every color on screen).
		FogComponent->FogDensity = 0.0015f;
		FogComponent->FogHeightFalloff = 0.2f;
		// Keep the first ~30 m crisp; the haze belongs to the distance.
		FogComponent->StartDistance = 3000.0f;
	}

	SpawnParams.Name = MakeUniqueObjectName(World->PersistentLevel,
		APostProcessVolume::StaticClass(), FName(TEXT("TrackPostProcess")));
	if (APostProcessVolume* PostProcess = World->SpawnActor<APostProcessVolume>(
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams))
	{
		PostProcess->SetActorLabel(TEXT("TrackPostProcess"));
		PostProcess->bUnbound = true;
		FPostProcessSettings& Settings = PostProcess->Settings;
		Settings.bOverride_BloomIntensity = true;
		Settings.BloomIntensity = 0.3f;
		// A touch under-exposed: the neutral metering renders daylight as
		// high-key pastel; racing footage sits deeper.
		Settings.bOverride_AutoExposureBias = true;
		Settings.AutoExposureBias = -0.4f;
		// Quick adaptation: the view flips between open sky and
		// self-shadowed cockpit constantly.
		Settings.bOverride_AutoExposureSpeedUp = true;
		Settings.AutoExposureSpeedUp = 5.0f;
		Settings.bOverride_AutoExposureSpeedDown = true;
		Settings.AutoExposureSpeedDown = 2.0f;
		// Clamped to daylight (EV100): the race director drives the sun at
		// real sunlight intensities, and without a floor the auto-exposure
		// would normalize any scene to mid-grey — which is exactly the
		// washed-out look this whole volume exists to prevent.
		Settings.bOverride_AutoExposureMinBrightness = true;
		Settings.AutoExposureMinBrightness = 10.0f;
		Settings.bOverride_AutoExposureMaxBrightness = true;
		Settings.AutoExposureMaxBrightness = 15.0f;
		Settings.bOverride_VignetteIntensity = true;
		Settings.VignetteIntensity = 0.3f;
	}

	World->PostEditChange();
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	TouchedPackages.Add(Package);

	UE_LOG(LogApexTrackImport, Display,
		TEXT("    level %s: %d track mesh actor(s), %d prop(s), %d grid slot(s)"), *LevelPackage,
		MeshActors, PropActors, Scene.Grid.Num());
	return true;
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
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), bIsMap ? FPackageName::GetMapPackageExtension()
									   : FPackageName::GetAssetPackageExtension());

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
