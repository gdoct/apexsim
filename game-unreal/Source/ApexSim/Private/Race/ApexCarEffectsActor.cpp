#include "Race/ApexCarEffectsActor.h"

#include "ApexSim.h"
#include "Camera/PlayerCameraManager.h"
#include "Cars/ApexCarMaterials.h"
#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** The engine's sphere is 100 cm across, its cube 100 cm a side. */
	constexpr float SphereRadiusCm = 50.0f;
	constexpr float CubeSizeCm = 100.0f;
	/** A puff closer to the camera than this is hidden: a grey wall over the lens is not smoke. */
	constexpr float NearCameraCm = 80.0f;
	/** How long a shutter sees a spark for: sets its streak's length from its speed. */
	constexpr float SparkShutterSeconds = 1.0f / 50.0f;

	UInstancedStaticMeshComponent* MakeRing(AActor& Owner, const TCHAR* Name, USceneComponent* Root)
	{
		UInstancedStaticMeshComponent* Component = Owner.CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
		Component->SetupAttachment(Root);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCastShadow(false);
		Component->bAffectDistanceFieldLighting = false;
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetUsingAbsoluteLocation(true);
		Component->SetUsingAbsoluteRotation(true);
		Component->SetUsingAbsoluteScale(true);
		Component->SetVisibility(false);
		Component->NumCustomDataFloats = ApexCarMaterials::SmokeCustomFloats;
		return Component;
	}
}

AApexCarEffectsActor::AApexCarEffectsActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the cars have moved and emitted, and after the cameras.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Smoke = MakeRing(*this, TEXT("Smoke"), Root);
	Sparks = MakeRing(*this, TEXT("Sparks"), Root);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (SphereFinder.Succeeded())
	{
		Smoke->SetStaticMesh(SphereFinder.Object);
	}
	if (CubeFinder.Succeeded())
	{
		Sparks->SetStaticMesh(CubeFinder.Object);
	}
}

AApexCarEffectsActor* AApexCarEffectsActor::Get(UWorld* World, bool bSpawn)
{
	if (!World || !World->IsGameWorld())
	{
		return nullptr;
	}
	for (TActorIterator<AApexCarEffectsActor> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			return *It;
		}
	}
	if (!bSpawn)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AApexCarEffectsActor* Spawned =
		World->SpawnActor<AApexCarEffectsActor>(AApexCarEffectsActor::StaticClass(), FTransform::Identity, Params);
	if (Spawned)
	{
		Spawned->EnsureInstances();
	}
	return Spawned;
}

void AApexCarEffectsActor::EnsureInstances()
{
	if (bInstancesMade)
	{
		return;
	}
	bInstancesMade = true;
	const FString Package = ApexCarMaterials::PackageName(ApexCarMaterials::SmokeName);
	UMaterialInterface* Material = FPackageName::DoesPackageExist(Package)
		? LoadObject<UMaterialInterface>(nullptr, *ApexCarMaterials::ObjectPath(ApexCarMaterials::SmokeName))
		: nullptr;
	bHaveMaterial = Material != nullptr;
	if (!bHaveMaterial)
	{
		UE_LOG(LogApexSim, Warning,
			TEXT("Car effects: %s is missing (run -run=ApexMaterialBake); damaged cars draw no smoke, steam or sparks"),
			*Package);
		return;
	}
	auto Fill = [Material](UInstancedStaticMeshComponent& Component, int32 Count, TArray<ApexDamage::FPuff>& Ring,
					TArray<FTransform>& Transforms, TArray<float>& Data) {
		Component.SetMaterial(0, Material);
		Ring.SetNum(Count);
		for (ApexDamage::FPuff& Puff : Ring)
		{
			Puff.Age = Puff.Life;
		}
		Transforms.Init(FTransform(FQuat::Identity, FVector::ZeroVector, FVector(0.001f)), Count);
		Data.Init(0.0f, Count * ApexCarMaterials::SmokeCustomFloats);
		Component.AddInstances(Transforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	};
	Fill(*Smoke, MaxSmoke, SmokeRing, SmokeTransforms, SmokeData);
	Fill(*Sparks, MaxSparks, SparkRing, SparkTransforms, SparkData);
}

void AApexCarEffectsActor::Emit(const ApexDamage::FPuff& Puff)
{
	EnsureInstances();
	if (!bHaveMaterial)
	{
		return;
	}
	if (Puff.bSpark)
	{
		SparkRing[NextSpark] = Puff;
		NextSpark = (NextSpark + 1) % MaxSparks;
		bSparksLive = true;
	}
	else
	{
		SmokeRing[NextSmoke] = Puff;
		NextSmoke = (NextSmoke + 1) % MaxSmoke;
		bSmokeLive = true;
	}
}

void AApexCarEffectsActor::AddDebris(AApexCarDebrisActor* Piece)
{
	Debris.RemoveAll([](const TWeakObjectPtr<AApexCarDebrisActor>& Weak) { return !Weak.IsValid(); });
	while (Debris.Num() >= MaxDebris)
	{
		if (AApexCarDebrisActor* Oldest = Debris[0].Get())
		{
			Oldest->Destroy();
		}
		Debris.RemoveAt(0);
	}
	Debris.Add(Piece);
}

void AApexCarEffectsActor::Clear()
{
	for (ApexDamage::FPuff& Puff : SmokeRing)
	{
		Puff.Age = Puff.Life;
	}
	for (ApexDamage::FPuff& Puff : SparkRing)
	{
		Puff.Age = Puff.Life;
	}
	for (const TWeakObjectPtr<AApexCarDebrisActor>& Weak : Debris)
	{
		if (AApexCarDebrisActor* Piece = Weak.Get())
		{
			Piece->Destroy();
		}
	}
	Debris.Reset();
}

bool AApexCarEffectsActor::WriteRing(UInstancedStaticMeshComponent& Component, TArray<ApexDamage::FPuff>& Ring,
	TArray<FTransform>& Transforms, TArray<float>& Data, float DeltaSeconds, const FVector& CameraLocation)
{
	constexpr int32 Floats = ApexCarMaterials::SmokeCustomFloats;
	bool bAnyAlive = false;
	for (int32 i = 0; i < Ring.Num(); ++i)
	{
		ApexDamage::FPuff& Puff = Ring[i];
		float* Out = Data.GetData() + i * Floats;
		if (!ApexDamage::StepPuff(Puff, DeltaSeconds)
			|| FVector::DistSquared(Puff.Location, CameraLocation) < NearCameraCm * NearCameraCm)
		{
			Transforms[i] = FTransform(FQuat::Identity, Puff.Location, FVector(0.001f));
			Out[0] = 0.0f;
			Out[2] = 0.0f;
			bAnyAlive |= Puff.IsAlive();
			continue;
		}
		bAnyAlive = true;
		if (Puff.bSpark)
		{
			// A streak along the spark's path, as long as a shutter sees it move.
			const float Speed = Puff.Velocity.Size();
			const float Length = FMath::Clamp(Speed * SparkShutterSeconds, 3.0f, 45.0f);
			const FQuat Aim = Speed > 1.0f ? Puff.Velocity.ToOrientationQuat() : FQuat::Identity;
			const float Thick = Puff.StartSize / CubeSizeCm;
			Transforms[i] = FTransform(Aim, Puff.Location, FVector(Length / CubeSizeCm, Thick, Thick));
			Out[0] = 1.0f;
			Out[1] = 1.0f;
			Out[2] = ApexDamage::PuffGlow(Puff);
			Out[3] = 0.0f;
		}
		else
		{
			Transforms[i] = FTransform(FQuat::Identity, Puff.Location, FVector(ApexDamage::PuffRadius(Puff) / SphereRadiusCm));
			Out[0] = ApexDamage::PuffOpacity(Puff);
			Out[1] = Puff.Shade;
			Out[2] = 0.0f;
			Out[3] = 1.0f;
		}
	}
	Component.BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
	for (int32 i = 0; i < Ring.Num(); ++i)
	{
		Component.SetCustomData(i, TArrayView<const float>(Data.GetData() + i * Floats, Floats), /*bMarkRenderStateDirty*/ false);
	}
	Component.MarkRenderStateDirty();
	Component.SetVisibility(bAnyAlive);
	return bAnyAlive;
}

void AApexCarEffectsActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bHaveMaterial || (!bSmokeLive && !bSparksLive))
	{
		return;
	}
	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
	const FVector CameraLocation = Camera ? Camera->GetCameraLocation() : FVector(1.0e9f);
	if (bSmokeLive)
	{
		bSmokeLive = WriteRing(*Smoke, SmokeRing, SmokeTransforms, SmokeData, DeltaSeconds, CameraLocation);
	}
	if (bSparksLive)
	{
		bSparksLive = WriteRing(*Sparks, SparkRing, SparkTransforms, SparkData, DeltaSeconds, CameraLocation);
	}
}

AApexCarDebrisActor::AApexCarDebrisActor()
{
	PrimaryActorTick.bCanEverTick = true;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetMobility(EComponentMobility::Movable);
}

void AApexCarDebrisActor::Launch(const UStaticMeshComponent& Source, const FVector& Velocity, const FVector& Spin, float FallbackGroundZ)
{
	UStaticMesh* Part = Source.GetStaticMesh();
	if (!Part)
	{
		return;
	}
	Mesh->SetStaticMesh(Part);
	for (int32 i = 0; i < Source.GetNumMaterials(); ++i)
	{
		Mesh->SetMaterial(i, Source.GetMaterial(i));
	}
	// The dents it had on the car, in the same body frame.
	const TArray<float>& Dents = Source.GetCustomPrimitiveData().Data;
	for (int32 i = 0; i < Dents.Num(); ++i)
	{
		Mesh->SetCustomPrimitiveDataFloat(i, Dents[i]);
	}
	Mesh->SetRelativeScale3D(Source.GetComponentScale());

	// The part's mesh is in the body's frame, its origin the car's: the
	// actor sits at the part's own middle, so it tumbles about itself.
	const FTransform World = Source.GetComponentTransform();
	const FVector LocalCentre = Part->GetBounds().Origin;
	Mesh->SetRelativeLocation(-LocalCentre * Source.GetComponentScale());
	Body.Location = World.TransformPosition(LocalCentre);
	Body.Rotation = World.GetRotation();
	Body.Velocity = Velocity;
	Body.Spin = Spin;
	FallbackGround = FallbackGroundZ;
	// It comes to rest on its flattest side: its middle that far up.
	const FVector Half = Part->GetBounds().BoxExtent * Source.GetComponentScale();
	RestHeight = static_cast<float>(FMath::Min3(Half.X, Half.Y, Half.Z));
	Body.GroundZ = GroundUnder(Body.Location) + RestHeight;
	SetActorLocationAndRotation(Body.Location, Body.Rotation);
}

float AApexCarDebrisActor::GroundUnder(const FVector& At) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return FallbackGround;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexDebrisGround), /*bTraceComplex*/ true, this);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, At + FVector(0.0f, 0.0f, 300.0f), At - FVector(0.0f, 0.0f, 3000.0f), ECC_Visibility, Params))
	{
		return static_cast<float>(Hit.ImpactPoint.Z);
	}
	return FallbackGround;
}

void AApexCarDebrisActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	if (Age > LifeSeconds)
	{
		Destroy();
		return;
	}
	if (Body.bResting)
	{
		return;
	}
	NextTrace -= DeltaSeconds;
	if (NextTrace <= 0.0f)
	{
		// The ground under it, a few times a second: it slides over kerbs and verges.
		NextTrace = 0.15f;
		Body.GroundZ = GroundUnder(Body.Location) + RestHeight;
	}
	ApexDamage::StepDebris(Body, DeltaSeconds);
	SetActorLocationAndRotation(Body.Location, Body.Rotation);
}
