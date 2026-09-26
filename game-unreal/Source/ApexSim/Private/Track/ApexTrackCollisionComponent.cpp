#include "Track/ApexTrackCollisionComponent.h"

#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/BodySetup.h"
#include "Track/ApexTrackSceneBuilder.h"

UApexTrackCollisionComponent::UApexTrackCollisionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	CastShadow = false;
	bHiddenInGame = true;
}

void UApexTrackCollisionComponent::SetTriangles(const TArray<FVector3f>& Positions, const TArray<uint32>& Indices)
{
	Vertices = Positions;
	Triangles = Indices;
	Triangles.SetNum(Triangles.Num() - Triangles.Num() % 3);
	LocalBounds = FBox(ForceInit);
	for (const FVector3f& P : Vertices)
	{
		LocalBounds += FVector(P);
	}
	UpdateBounds();

	if (Triangles.IsEmpty())
	{
		bCooking = false;
		return;
	}

	// A fresh body setup per cook, as the procedural mesh does: one that is
	// already in use by the physics scene must not be re-cooked under it.
	UBodySetup* Setup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
	Setup->BodySetupGuid = FGuid::NewGuid();
	Setup->bGenerateMirroredCollision = false;
	Setup->bDoubleSidedGeometry = true;
	Setup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	bCooking = true;
	PendingBodySetup = Setup;
	Setup->CreatePhysicsMeshesAsync(
		FOnAsyncPhysicsCookFinished::CreateUObject(this, &UApexTrackCollisionComponent::FinishCook, Setup));
}

void UApexTrackCollisionComponent::FinishCook(bool bSuccess, UBodySetup* Cooked)
{
	if (Cooked != PendingBodySetup)
	{
		// A cook superseded by a later SetTriangles.
		return;
	}
	bCooking = false;
	PendingBodySetup = nullptr;
	if (!Cooked)
	{
		return;
	}
	if (!bSuccess)
	{
		UE_LOG(LogApexTrack, Warning, TEXT("Collision cook failed for %s; traces will pass through it"),
			*GetPathNameSafe(GetOwner()));
	}
	BodySetup = Cooked;
	// The triangles live in the cooked mesh now.
	Vertices.Empty();
	Triangles.Empty();
	RecreatePhysicsState();
}

bool UApexTrackCollisionComponent::GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool InUseAllTriData)
{
	if (Triangles.IsEmpty())
	{
		return false;
	}
	CollisionData->Vertices = Vertices;
	CollisionData->Indices.Reserve(Triangles.Num() / 3);
	CollisionData->MaterialIndices.Reserve(Triangles.Num() / 3);
	for (int32 i = 0; i + 2 < Triangles.Num(); i += 3)
	{
		FTriIndices Triangle;
		Triangle.v0 = static_cast<int32>(Triangles[i]);
		Triangle.v1 = static_cast<int32>(Triangles[i + 1]);
		Triangle.v2 = static_cast<int32>(Triangles[i + 2]);
		CollisionData->Indices.Add(Triangle);
		CollisionData->MaterialIndices.Add(0);
	}
	// The bake winds for Unreal's clockwise front face; the cooker wants
	// the other hand. The body is double-sided anyway, so this only keeps
	// hit normals pointing out of the surface.
	CollisionData->bFlipNormals = true;
	CollisionData->bDeformableMesh = false;
	CollisionData->bFastCook = true;
	return true;
}

bool UApexTrackCollisionComponent::ContainsPhysicsTriMeshData(bool InUseAllTriData) const
{
	return !Triangles.IsEmpty();
}

UBodySetup* UApexTrackCollisionComponent::GetBodySetup()
{
	return BodySetup;
}

FBoxSphereBounds UApexTrackCollisionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (!LocalBounds.IsValid)
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
	}
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
}
