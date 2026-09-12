#include "Race/ApexPropActors.h"

#include "Components/StaticMeshComponent.h"

namespace
{
	UStaticMeshComponent* MakeMesh(AActor* Owner, const TCHAR* Name)
	{
		UStaticMeshComponent* Component = Owner->CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetGenerateOverlapEvents(false);
		Component->SetCanEverAffectNavigation(false);
		return Component;
	}
}	 // namespace

AApexSkyDriftActor::AApexSkyDriftActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = MakeMesh(this, TEXT("Mesh"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetRootComponent(Mesh);
}

void AApexSkyDriftActor::BeginPlay()
{
	Super::BeginPlay();
	Home = GetActorTransform();
}

void AApexSkyDriftActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;

	// A sine both ways: it eases into each turn, and its peak speed is the
	// drift speed when the period is 2π·range/speed.
	const float Range = FMath::Max(DriftRangeCm, 1.0f);
	const float Omega = FMath::Max(DriftSpeedCmPerS, 0.0f) / Range;
	const float Along = Range * FMath::Sin(Omega * Elapsed);

	const float SwayAmplitude = FMath::Max(SwayAmplitudeDeg, 0.01f);
	const float SwayOmega = FMath::DegreesToRadians(FMath::Max(SwayDegPerS, 0.0f)) / FMath::DegreesToRadians(SwayAmplitude);
	const float Sway = SwayAmplitude * FMath::Sin(SwayOmega * Elapsed);

	const FVector Location = Home.GetLocation() + Home.GetRotation().GetForwardVector() * Along;
	FRotator Rotation = Home.Rotator();
	Rotation.Yaw += Sway;
	SetActorLocationAndRotation(Location, Rotation);
}

AApexRotorActor::AApexRotorActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = MakeMesh(this, TEXT("Mesh"));
	SetRootComponent(Mesh);
	Rotor = MakeMesh(this, TEXT("Rotor"));
	Rotor->SetupAttachment(Mesh);
	Rotor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AApexRotorActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	AngleDeg = FMath::Fmod(AngleDeg + RevolutionsPerMinute * 6.0f * DeltaSeconds, 360.0f);
	if (Rotor)
	{
		// The wheel plane faces the road (local ±Y), so the axle is local Y:
		// a pitch in the component's own frame.
		Rotor->SetRelativeRotation(FRotator(AngleDeg, 0.0f, 0.0f));
	}
}
