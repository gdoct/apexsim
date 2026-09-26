#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Interfaces/Interface_CollisionDataProvider.h"

#include "ApexTrackCollisionComponent.generated.h"

class UBodySetup;

/**
 * Complex collision for a track surface built at runtime.
 *
 * A static mesh built in a cooked game has no cooked collision and cannot
 * cook its own (its body setup belongs to an asset, not to anything in a
 * game world). This component can: it owns its body setup, hands Chaos its
 * triangles through `IInterface_CollisionDataProvider` and has them cooked
 * on a worker thread — the way `UProceduralMeshComponent` works, without
 * the plugin. It draws nothing; the static mesh beside it does that.
 *
 * Its triangles only answer traces (`ECC_WorldStatic`): the racing line's
 * ground snap and the cameras' ground and line-of-sight tests. Cars are
 * telemetry puppets and collide with nothing.
 */
UCLASS(ClassGroup = (ApexSim), meta = (BlueprintSpawnableComponent))
class APEXSIM_API UApexTrackCollisionComponent : public UPrimitiveComponent, public IInterface_CollisionDataProvider
{
	GENERATED_BODY()

public:
	UApexTrackCollisionComponent(const FObjectInitializer& ObjectInitializer);

	/**
	 * The triangles to collide with, in the component's space; starts the
	 * cook. The arrays are copied and released once the cook is done.
	 */
	void SetTriangles(const TArray<FVector3f>& Positions, const TArray<uint32>& Indices);

	/** True once the cook has finished (or there was nothing to cook). */
	bool IsCooked() const { return !bCooking; }

	// IInterface_CollisionDataProvider
	virtual bool GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool InUseAllTriData) override;
	virtual bool ContainsPhysicsTriMeshData(bool InUseAllTriData) const override;
	virtual bool WantsNegXTriMesh() override { return false; }

	// UPrimitiveComponent
	virtual UBodySetup* GetBodySetup() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	void FinishCook(bool bSuccess, UBodySetup* Cooked);

	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> BodySetup;

	/** The body setup being cooked, held here so nothing collects it meanwhile. */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> PendingBodySetup;

	TArray<FVector3f> Vertices;
	TArray<uint32> Triangles;
	FBox LocalBounds = FBox(ForceInit);
	bool bCooking = false;
};
