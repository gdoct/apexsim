#include "Track/ApexTrackInstance.h"

#include "Async/Async.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "Track/ApexTrackCollisionComponent.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "Track/ApexTrackSceneReader.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	TAutoConsoleVariable<float> CVarTrackBuildBudgetMs(TEXT("apexsim.track.BuildBudgetMs"), 20.0f,
		TEXT("Milliseconds of each frame a runtime track may spend building meshes while it loads."));

	/** What a runtime track falls back to for the track parent when it was never baked. */
	const TCHAR* kFallbackMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	const TCHAR* kPlaceholderCube = TEXT("/Engine/BasicShapes/Cube.Cube");
}	 // namespace

/**
 * Transient assets for a track built in the running game: dynamic material
 * instances and fast-built static meshes, owned by the instance. The meshes
 * get no collision of their own (a mesh built in a cooked game cannot cook
 * it); the builder gives each track surface a collision component instead.
 */
class FApexRuntimeTrackFactory : public IApexTrackAssetFactory
{
public:
	explicit FApexRuntimeTrackFactory(UApexTrackInstance& InOwner)
		: Owner(InOwner)
	{
	}

	virtual UMaterialInterface* MakeMaterial(
		const FString& Key, UMaterialInterface* Parent, const FApexMaterialParams& Params) override
	{
		if (!Parent)
		{
			return nullptr;
		}
		const FName Name = MakeUniqueObjectName(&Owner, UMaterialInstanceDynamic::StaticClass(), FName(*(TEXT("MI_") + Key)));
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, &Owner, Name);
		if (!Material)
		{
			return nullptr;
		}
		TSharedPtr<FApexMaterialParams> Kept = MakeShared<FApexMaterialParams>(Params);
		Owner.ApplyMaterialParams(Material, *Kept);
		Owner.Created.Add(Material);
		Owner.MaterialDefaults.Emplace(Material, Kept);
		return Material;
	}

	virtual UStaticMesh* MakeMesh(const FString& Name, FMeshDescription& Description, const TArray<FName>& SlotNames,
		const TArray<UMaterialInterface*>& SlotMaterials, bool bTrackSurface, const FBox& Bounds) override
	{
		const FName ObjectName = MakeUniqueObjectName(&Owner, UStaticMesh::StaticClass(), FName(*(TEXT("SM_") + Name)));
		UStaticMesh* Mesh = NewObject<UStaticMesh>(&Owner, ObjectName, RF_Transient);
		for (int32 i = 0; i < SlotNames.Num(); ++i)
		{
			// The slot name matches the polygon group's, which is how the
			// build maps sections onto slots.
			FStaticMaterial& Slot = Mesh->GetStaticMaterials().Add_GetRef(
				FStaticMaterial(SlotMaterials.IsValidIndex(i) ? SlotMaterials[i] : nullptr, SlotNames[i]));
			// The editor's build measures how much world a UV unit covers,
			// which the texture streamer needs to pick mips; the fast build
			// does not. Track UVs are metres (100 cm a unit) and the stand-ins'
			// face UVs are about that too.
			Slot.UVChannelData.bInitialized = true;
			for (float& Density : Slot.UVChannelData.LocalUVDensities)
			{
				Density = 100.0f;
			}
		}
		// Nothing to cook on the mesh itself: a track surface's triangles are
		// cooked by the collision component beside it, and a stand-in keeps
		// the simple box the build adds, which needs no cooking. Say so before
		// the build makes a body setup and tries.
		Mesh->CreateBodySetup();
		if (UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			BodySetup->bNeverNeedsCookedCollisionData = true;
		}

		UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
		BuildParams.bFastBuild = true;
		BuildParams.bBuildSimpleCollision = !bTrackSurface;
		BuildParams.bCommitMeshDescription = false;
		BuildParams.bMarkPackageDirty = false;
		if (!Mesh->BuildFromMeshDescriptions({&Description}, BuildParams))
		{
			return nullptr;
		}
		// The fast build takes its bounds from the description, which the
		// editor found NaN on a few of Monza's ground patches with perfectly
		// finite vertices; a NaN-bounded mesh is culled from every view. The
		// vertices' own box is what the mesh spans.
		if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData(); RenderData && Bounds.IsValid)
		{
			RenderData->Bounds = FBoxSphereBounds(Bounds);
			Mesh->CalculateExtendedBounds();
		}
		if (!bTrackSurface)
		{
			// The box answers the cameras' complex traces too, as a cooked
			// stand-in's does.
			if (UBodySetup* BodySetup = Mesh->GetBodySetup())
			{
				BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			}
		}
		Owner.Created.Add(Mesh);
		return Mesh;
	}

	virtual bool WantsCollisionComponents() const override { return true; }

private:
	UApexTrackInstance& Owner;
};

UApexTrackInstance::UApexTrackInstance()
{
	// Referenced from the class default object so the cooker carries them
	// into a packaged build, where a runtime track may need them.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(kPlaceholderCube);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(kFallbackMaterial);
	PlaceholderCube = CubeFinder.Object;
	FallbackMaterial = MaterialFinder.Object;
}

UApexTrackInstance::~UApexTrackInstance() = default;

UWorld* UApexTrackInstance::GetWorld() const
{
	return World.Get();
}

void UApexTrackInstance::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UApexTrackInstance* This = CastChecked<UApexTrackInstance>(InThis);
	if (This->Builder)
	{
		This->Builder->AddReferencedObjects(Collector);
	}
	Super::AddReferencedObjects(InThis, Collector);
}

bool UApexTrackInstance::Start(UWorld* InWorld, const FApexRuntimeTrackFiles& Files)
{
	World = InWorld;
	Stem = Files.Stem;
	StartedAt = FPlatformTime::Seconds();
	bVisible = true;
	if (!InWorld)
	{
		Fail(TEXT("no world"));
		return false;
	}

	// Read and prepare off the game thread: the manifest and blob are tens
	// of megabytes and the mesh descriptions a million triangles. The task
	// owns everything it touches; the result is picked up in Tick.
	ScenePath = Files.ScenePath;
	Phase = EPhase::Parsing;
	UE_LOG(LogApexTrack, Log, TEXT("Track %s: building from %s"), *Stem, *ScenePath);
	const FString Path = ScenePath;
	Parsing = Async(EAsyncExecution::ThreadPool, [Path]() -> TSharedPtr<FParsed> {
		const double Began = FPlatformTime::Seconds();
		TSharedPtr<FParsed> Result = MakeShared<FParsed>();
		Result->Scene = MakeShared<FApexTrackScene>();
		if (!FApexTrackSceneReader::LoadFromFile(Path, *Result->Scene, Result->Error))
		{
			return Result;
		}
		Result->Geometry = MakeShared<FApexTrackGeometry>();
		FApexTrackSceneBuilder::PrepareGeometry(*Result->Scene, *Result->Geometry);
		Result->Seconds = FPlatformTime::Seconds() - Began;
		Result->bOk = true;
		return Result;
	});
	return true;
}

bool UApexTrackInstance::IsTickable() const
{
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return false;
	}
	return Phase == EPhase::Parsing || Phase == EPhase::Materials || Phase == EPhase::Meshes
		|| Phase == EPhase::Actors || Phase == EPhase::Cooking;
}

TStatId UApexTrackInstance::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UApexTrackInstance, STATGROUP_Tickables);
}

void UApexTrackInstance::Fail(const FString& Why)
{
	Phase = EPhase::Failed;
	UE_LOG(LogApexTrack, Error, TEXT("Track %s could not be loaded: %s"), *Stem, *Why);
	Builder.Reset();
	Factory.Reset();
	Scene.Reset();
	Geometry.Reset();
}

void UApexTrackInstance::Tick(float DeltaTime)
{
	UWorld* TrackWorld = World.Get();
	if (!TrackWorld)
	{
		Fail(TEXT("its world went away while it loaded"));
		return;
	}

	if (Phase == EPhase::Parsing)
	{
		if (!Parsing.IsReady())
		{
			return;
		}
		TSharedPtr<FParsed> Parsed = Parsing.Get();
		Parsing = TFuture<TSharedPtr<FParsed>>();
		if (!Parsed || !Parsed->bOk)
		{
			Fail(Parsed ? Parsed->Error : FString(TEXT("the reader returned nothing")));
			return;
		}
		ParseSeconds = Parsed->Seconds;
		Scene = Parsed->Scene;
		Geometry = Parsed->Geometry;
		Phase = EPhase::Materials;
		// Fall through: the materials are quick.
	}

	if (Phase == EPhase::Materials)
	{
		FApexTrackParents Parents = ApexTrackMaterials::LoadParents();
		if (!Parents.Base)
		{
			// A fresh clone before `ApexMaterialBake`, or a package cooked
			// without `/Game/Materials`: flat colours rather than no track.
			UE_LOG(LogApexTrack, Error,
				TEXT("Track %s: no %s — run `-run=ApexMaterialBake` in the editor and re-cook; drawing flat colours"),
				*Stem, *ApexTrackMaterials::ObjectPath(ApexTrackMaterials::BaseName));
			Parents.Base = FallbackMaterial;
		}
		Factory = MakeShared<FApexRuntimeTrackFactory>(*this);
		Builder = MakeShared<FApexTrackSceneBuilder>(*Factory, Parents);
		FString Error;
		if (!Builder->BuildMaterials(*Scene, Error))
		{
			Fail(Error);
			return;
		}
		Phase = EPhase::Meshes;
		return;
	}

	if (Phase == EPhase::Meshes)
	{
		const double Deadline = FPlatformTime::Seconds() + FMath::Max(1.0f, CVarTrackBuildBudgetMs.GetValueOnGameThread()) / 1000.0;
		FString Error;
		if (!Builder->BuildMeshes(*Geometry, Deadline, Error))
		{
			Fail(Error);
			return;
		}
		if (Geometry->IsBuilt())
		{
			if (!Builder->ValidateMeshes(Error))
			{
				Fail(Error);
				return;
			}
			Geometry.Reset();
			Phase = EPhase::Actors;
		}
		return;
	}

	if (Phase == EPhase::Actors)
	{
		TArray<AActor*> Spawned;
		Builder->SpawnActors(*Scene, TrackWorld, TrackWorld->PersistentLevel, Spawned);
		Actors.Reset(Spawned.Num());
		for (AActor* Actor : Spawned)
		{
			Actors.Add(Actor);
		}
		// Everything is referenced by the actors and `Created` now.
		Builder.Reset();
		Factory.Reset();
		Scene.Reset();
		ApplyVisibility();
		Phase = EPhase::Cooking;
		return;
	}

	if (Phase == EPhase::Cooking)
	{
		// Loaded means traceable: the racing line snaps to the road and the
		// cameras find the ground the moment the director sees it loaded.
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (!Actor)
			{
				continue;
			}
			TArray<UApexTrackCollisionComponent*> Collisions;
			Actor->GetComponents<UApexTrackCollisionComponent>(Collisions);
			for (const UApexTrackCollisionComponent* Collision : Collisions)
			{
				if (!Collision->IsCooked())
				{
					return;
				}
			}
		}
		Phase = EPhase::Ready;
		UE_LOG(LogApexTrack, Log, TEXT("Track %s built in %.2f s (%.2f s reading and preparing), %d actor(s), %d asset(s)"),
			*Stem, FPlatformTime::Seconds() - StartedAt, ParseSeconds, Actors.Num(), Created.Num());
	}
}

bool UApexTrackInstance::IsLoaded() const
{
	return Phase == EPhase::Ready;
}

bool UApexTrackInstance::IsVisible() const
{
	return Phase == EPhase::Ready && bVisible;
}

bool UApexTrackInstance::HasFailed() const
{
	return Phase == EPhase::Failed;
}

void UApexTrackInstance::SetVisible(bool bInVisible)
{
	bVisible = bInVisible;
	ApplyVisibility();
}

void UApexTrackInstance::ApplyVisibility()
{
	for (const TObjectPtr<AActor>& Actor : Actors)
	{
		if (!Actor)
		{
			continue;
		}
		// Hidden is not enough for the two actors that act on the whole
		// view: an unbound volume grades the menu behind the car-select
		// screen whether or not it is "hidden", and so does the fog.
		Actor->SetActorHiddenInGame(!bVisible);
		Actor->SetActorEnableCollision(bVisible);
		if (bVisible)
		{
			// A cook that finished while the track was hidden made no body
			// (collision was off), and turning collision back on does not
			// make one: ask for it.
			TArray<UApexTrackCollisionComponent*> Collisions;
			Actor->GetComponents<UApexTrackCollisionComponent>(Collisions);
			for (UApexTrackCollisionComponent* Collision : Collisions)
			{
				if (Collision->IsCooked() && !Collision->IsPhysicsStateCreated())
				{
					Collision->RecreatePhysicsState();
				}
			}
		}
		if (APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
		{
			Volume->bEnabled = bVisible;
		}
		else if (AExponentialHeightFog* Fog = Cast<AExponentialHeightFog>(Actor))
		{
			Fog->GetComponent()->SetVisibility(bVisible);
		}
	}
}

void UApexTrackInstance::Unload()
{
	for (const TObjectPtr<AActor>& Actor : Actors)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	Actors.Reset();
	Created.Reset();
	MaterialDefaults.Reset();
	// A parse still running finishes into a future nobody reads.
	Parsing = TFuture<TSharedPtr<FParsed>>();
	Builder.Reset();
	Factory.Reset();
	Scene.Reset();
	Geometry.Reset();
	Phase = EPhase::Idle;
}

void UApexTrackInstance::ApplyMaterialParams(UMaterialInstanceDynamic* Material, const FApexMaterialParams& Params) const
{
	// The fallback parent calls its colour `Color`; everything else is
	// named as the track parents name it.
	const bool bFallback = Material->Parent == FallbackMaterial && FallbackMaterial != nullptr;
	static const FName BaseColor(TEXT("BaseColor"));
	static const FName Color(TEXT("Color"));
	for (const TPair<FName, float>& Pair : Params.Scalars)
	{
		Material->SetScalarParameterValue(Pair.Key, Pair.Value);
	}
	for (const TPair<FName, FLinearColor>& Pair : Params.Vectors)
	{
		Material->SetVectorParameterValue(bFallback && Pair.Key == BaseColor ? Color : Pair.Key, Pair.Value);
	}
	for (const TPair<FName, UTexture*>& Pair : Params.Textures)
	{
		Material->SetTextureParameterValue(Pair.Key, Pair.Value);
	}
}

void UApexTrackInstance::ResetForReuse()
{
	for (const TPair<TWeakObjectPtr<UMaterialInstanceDynamic>, TSharedPtr<FApexMaterialParams>>& Pair : MaterialDefaults)
	{
		if (UMaterialInstanceDynamic* Material = Pair.Key.Get(); Material && Pair.Value)
		{
			ApplyMaterialParams(Material, *Pair.Value);
		}
	}
	SetVisible(true);
	UE_LOG(LogApexTrack, Log, TEXT("Track %s: reusing the track built earlier"), *Stem);
}

void UApexTrackInstance::GetActors(TArray<AActor*>& OutActors) const
{
	OutActors.Reset();
	for (const TObjectPtr<AActor>& Actor : Actors)
	{
		if (IsValid(Actor))
		{
			OutActors.Add(Actor);
		}
	}
}
