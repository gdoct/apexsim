#include "Race/ApexRaceCarActor.h"

#include "Audio/ApexEngineSoundWave.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Race/ApexRaceCoordinate.h"
#include "Sound/SoundAttenuation.h"

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

	// Nothing places the car until the first telemetry frame; until then it
	// would sit at the world origin, which on most circuits is in mid-air or
	// underground next to the grid. Stay hidden until the server says where.
	SetActorHiddenInGame(true);

	EngineAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("EngineAudio"));
	EngineAudio->SetupAttachment(Root);
	EngineAudio->bAutoActivate = false;
	// No SoundAttenuation asset exists for this, so the falloff is set directly
	// on the component: audible up close, fading out well before the next car
	// on a straight would be in earshot.
	EngineAudio->bOverrideAttenuation = true;
	EngineAudio->AttenuationOverrides.bAttenuate = true;
	EngineAudio->AttenuationOverrides.bSpatialize = true;
	EngineAudio->AttenuationOverrides.AttenuationShape = EAttenuationShape::Sphere;
	EngineAudio->AttenuationOverrides.AttenuationShapeExtents = FVector(1500.0f, 0.0f, 0.0f);
	EngineAudio->AttenuationOverrides.FalloffDistance = 4000.0f;
}

void AApexRaceCarActor::BeginPlay()
{
	Super::BeginPlay();

	// Procedural, not an asset: see UApexEngineSoundWave for why. Built here
	// rather than in the constructor so it is never part of the CDO.
	EngineSound = NewObject<UApexEngineSoundWave>(this);
	EngineAudio->SetSound(EngineSound);
}

void AApexRaceCarActor::SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow)
{
	UStaticMesh* Loaded = MeshToShow.IsNull() ? nullptr : MeshToShow.LoadSynchronous();
	CarMesh->SetStaticMesh(Loaded);
	// A different body is a different seat.
	bCockpitLayoutValid = false;
}

void AApexRaceCarActor::SetMeshVisible(bool bVisible)
{
	CarMesh->SetVisibility(bVisible);
}

void AApexRaceCarActor::SetCockpitSpec(const FString& InCarClass, const FApexCockpitOverrides& InOverrides)
{
	CarClass = InCarClass;
	CockpitOverrides = InOverrides;
	bCockpitLayoutValid = false;
}

const FApexCockpitLayout& AApexRaceCarActor::GetCockpitLayout()
{
	if (!bCockpitLayoutValid)
	{
		const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
		const FBox Body = Mesh
			? ApexCockpit::ActorFrameBox(Mesh->GetBounds(), CarMesh->GetRelativeTransform())
			: ApexCockpit::FallbackBox();
		const EApexCockpitStyle Style = ApexCockpit::ResolveStyle(CockpitOverrides.Style, CarClass, Body);
		CockpitLayout = ApexCockpit::DeriveLayout(Body, Style, CockpitOverrides);
		bCockpitLayoutValid = true;
	}
	return CockpitLayout;
}

void AApexRaceCarActor::ApplyTelemetry(const FApexCarTelemetry& Car)
{
	CarIndex = Car.CarIndex;
	SpeedMps = Car.SpeedMps;
	// Kept so a shift request can be expressed relative to it; the protocol
	// wants an absolute target gear, not a delta.
	Gear = Car.Gear;
	EngineRpm = Car.EngineRpm;
	Throttle = Car.Throttle;
	Brake = Car.Brake;
	Steering = Car.Steering;
	CurrentLap = Car.CurrentLap;
	CurrentLapTimeMs = Car.CurrentLapTimeMs;

	TargetLocation = ApexRace::ServerToUnrealPosition(Car.Position);
	TargetRotation = ApexRace::ServerToUnrealRotation(Car.YawRad, Car.PitchRad, Car.RollRad);

	// First frame, or a jump too large to be real motion: go straight there.
	if (!bHasTarget || FVector::Dist(GetActorLocation(), TargetLocation) > TeleportDistanceCm)
	{
		SetActorLocationAndRotation(TargetLocation, TargetRotation);
	}
	if (!bHasTarget)
	{
		SetActorHiddenInGame(false);
		if (EngineAudio)
		{
			EngineAudio->Play();
		}
	}
	bHasTarget = true;

	// Redline is never broadcast, so the pitch range only ever grows to fit
	// what has actually been seen — same trick as the HUD's RPM strip.
	ObservedMaxRpm = FMath::Max(ObservedMaxRpm, EngineRpm);
	if (EngineSound)
	{
		EngineSound->SetRpmRange(800.0f, ObservedMaxRpm);
		EngineSound->SetLive(EngineRpm, Throttle);
	}
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
