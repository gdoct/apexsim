#include "Race/ApexRainActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AApexRainActor::AApexRainActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the cameras have moved, so the box is centred on this frame's view.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Streaks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Streaks"));
	Streaks->SetupAttachment(Root);
	Streaks->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Streaks->SetCastShadow(false);
	Streaks->bAffectDistanceFieldLighting = false;
	Streaks->SetMobility(EComponentMobility::Movable);
	Streaks->SetUsingAbsoluteLocation(true);
	Streaks->SetUsingAbsoluteRotation(true);
	Streaks->SetUsingAbsoluteScale(true);
	Streaks->SetVisibility(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (CubeFinder.Succeeded())
	{
		Streaks->SetStaticMesh(CubeFinder.Object);
	}
	if (MaterialFinder.Succeeded())
	{
		Streaks->SetMaterial(0, MaterialFinder.Object);
	}

	Random.Initialize(1337);
}

void AApexRainActor::SetIntensity(float Intensity)
{
	RainIntensity = FMath::Clamp(Intensity, 0.0f, 1.0f);
	const int32 Wanted = FMath::RoundToInt(RainIntensity * MaxStreaks);
	EnsureStreaks(Wanted);
	// Streaks past the new count stay in the component but shrink to nothing.
	for (int32 Index = Wanted; Index < Transforms.Num(); ++Index)
	{
		Transforms[Index].SetScale3D(FVector(0.001f));
	}
	if (Wanted < ActiveStreaks && Transforms.Num() > 0)
	{
		Streaks->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ true, /*bTeleport*/ true);
	}
	ActiveStreaks = Wanted;
	Streaks->SetVisibility(ActiveStreaks > 0);
	SetActorTickEnabled(ActiveStreaks > 0);
}

FVector AApexRainActor::BoxCentre(const FVector& CameraLocation, const FVector& InCameraVelocity) const
{
	// Ahead of a moving camera, so the drops it is about to meet exist.
	return CameraLocation + InCameraVelocity * 0.35f + FVector(0.0f, 0.0f, HalfHeightCm * 0.25f);
}

void AApexRainActor::EnsureStreaks(int32 Count)
{
	if (Count <= Positions.Num())
	{
		return;
	}
	const FVector Centre = bHaveCamera ? BoxCentre(LastCameraLocation, CameraVelocity) : FVector::ZeroVector;
	const int32 First = Positions.Num();
	Positions.SetNum(Count);
	Transforms.SetNum(Count);
	for (int32 Index = First; Index < Count; ++Index)
	{
		Positions[Index] = Centre + FVector(
			Random.FRandRange(-HalfWidthCm, HalfWidthCm),
			Random.FRandRange(-HalfWidthCm, HalfWidthCm),
			Random.FRandRange(-HalfHeightCm, HalfHeightCm));
		Transforms[Index] = FTransform(FRotator::ZeroRotator, Positions[Index], FVector(0.01f));
	}
	if (Streaks->GetInstanceCount() < Count)
	{
		TArray<FTransform> Fresh(Transforms.GetData() + Streaks->GetInstanceCount(), Count - Streaks->GetInstanceCount());
		Streaks->AddInstances(Fresh, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	}
}

void AApexRainActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (ActiveStreaks <= 0 || DeltaSeconds <= 0.0f)
	{
		return;
	}

	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (!Camera)
	{
		return;
	}
	const FVector CameraLocation = Camera->GetCameraLocation();
	if (bHaveCamera)
	{
		const FVector Raw = (CameraLocation - LastCameraLocation) / DeltaSeconds;
		// A cut (a new shot, a respawn) is not a velocity.
		if (Raw.Size() < 20000.0f)
		{
			CameraVelocity = FMath::VInterpTo(CameraVelocity, Raw, DeltaSeconds, 8.0f);
		}
		else
		{
			CameraVelocity = FVector::ZeroVector;
		}
	}
	LastCameraLocation = CameraLocation;
	bHaveCamera = true;

	const FVector Centre = BoxCentre(CameraLocation, CameraVelocity);
	// A little wind so the fall is never dead vertical.
	const FVector Fall(60.0f, 25.0f, -FallSpeedCmPerS);
	// What the camera sees: the drop's motion less its own.
	const FVector Apparent = Fall - CameraVelocity;
	const float ApparentSpeed = FMath::Max(Apparent.Size(), 1.0f);
	const float Length = FMath::Clamp(ApparentSpeed * ShutterSeconds, 12.0f, 160.0f);
	const FRotator Aim = Apparent.Rotation();
	// The cube is 100 cm; the streak lies along its local X, which `Rotation`
	// points down the apparent velocity.
	const FVector Scale(Length / 100.0f, StreakThicknessCm / 100.0f, StreakThicknessCm / 100.0f);

	for (int32 Index = 0; Index < ActiveStreaks; ++Index)
	{
		FVector& P = Positions[Index];
		P += Fall * DeltaSeconds;

		// Wrap on each axis of the box; a drop that leaves the bottom comes
		// back in at the top somewhere new.
		FVector Rel = P - Centre;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (Rel[Axis] > HalfWidthCm)       { Rel[Axis] -= 2.0f * HalfWidthCm; }
			else if (Rel[Axis] < -HalfWidthCm) { Rel[Axis] += 2.0f * HalfWidthCm; }
		}
		if (Rel.Z < -HalfHeightCm)
		{
			Rel.Z += 2.0f * HalfHeightCm;
			Rel.X = Random.FRandRange(-HalfWidthCm, HalfWidthCm);
			Rel.Y = Random.FRandRange(-HalfWidthCm, HalfWidthCm);
		}
		else if (Rel.Z > HalfHeightCm)
		{
			Rel.Z -= 2.0f * HalfHeightCm;
		}
		P = Centre + Rel;

		// Nothing right at the lens: a streak through the near plane is a
		// grey bar across the screen.
		const bool bTooClose = FVector::DistSquared(P, CameraLocation) < 40.0f * 40.0f;
		Transforms[Index] = FTransform(Aim, P, bTooClose ? FVector(0.001f) : Scale);
	}

	Streaks->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ true, /*bTeleport*/ true);
}
