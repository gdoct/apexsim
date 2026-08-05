#include "Race/ApexRaceCarActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Race/ApexRaceCoordinate.h"

AApexRaceCarActor::AApexRaceCarActor()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	CarMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CarMesh"));
	CarMesh->SetupAttachment(Root);
	CarMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// The imported meshes have their long axis on Y; the same -90 yaw the car
	// preview applies puts the nose on +X, which is what the server's heading
	// means.
	CarMesh->SetRelativeRotation(FRotator(0.0f, -90.0f, 0.0f));
}

void AApexRaceCarActor::SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow)
{
	UStaticMesh* Loaded = MeshToShow.IsNull() ? nullptr : MeshToShow.LoadSynchronous();
	CarMesh->SetStaticMesh(Loaded);
}

void AApexRaceCarActor::ApplyTelemetry(const FApexCarTelemetry& Car)
{
	CarIndex = Car.CarIndex;
	SpeedMps = Car.SpeedMps;

	TargetLocation = ApexRace::ServerToUnrealPosition(Car.Position);
	TargetRotation = ApexRace::ServerToUnrealRotation(Car.YawRad, Car.PitchRad, Car.RollRad);

	// First frame, or a jump too large to be real motion: go straight there.
	if (!bHasTarget || FVector::Dist(GetActorLocation(), TargetLocation) > TeleportDistanceCm)
	{
		SetActorLocationAndRotation(TargetLocation, TargetRotation);
	}
	bHasTarget = true;
}

void AApexRaceCarActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bHasTarget)
	{
		return;
	}

	// Telemetry lands at 60Hz but the client renders faster, so ease towards the
	// latest sample rather than snapping to it.
	const FVector NewLocation =
		FMath::VInterpTo(GetActorLocation(), TargetLocation, DeltaSeconds, InterpolationSpeed);
	const FRotator NewRotation =
		FMath::RInterpTo(GetActorRotation(), TargetRotation, DeltaSeconds, InterpolationSpeed);

	SetActorLocationAndRotation(NewLocation, NewRotation);
}
