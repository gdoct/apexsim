#include "Race/ApexRaceCarActor.h"

#include "Audio/ApexEngineSoundWave.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexRaceCoordinate.h"
#include "Sound/SoundAttenuation.h"

namespace
{
	/**
	 * Emissive strength of a lit brake light. The race is exposed for a 50 klux
	 * sun, where the GLB's own strength of 1 is invisible; the start lights
	 * read at 4000. A knob so it can be tuned against a screenshot.
	 */
	TAutoConsoleVariable<float> CVarBrakeLightNits(
		TEXT("apexsim.car.BrakeLightNits"),
		3000.0f,
		TEXT("Brightness of a car's brake lights while braking, as an emissive multiplier"),
		ECVF_Default);

	/** The material slot every car GLB gives its brake lights (docs/CAR_MODELS.md). */
	const FName BrakeLightSlot(TEXT("car_brakelight"));

	/** Parameter on the Interchange glTF parent that scales `EmissiveFactor`. */
	const FName EmissiveStrengthParam(TEXT("EmissiveStrength"));

	/**
	 * Pedal travel that turns the lights on. A real brake-light switch is
	 * on/off at the first bit of pedal, not dimmed by how hard it is pressed.
	 */
	constexpr float BrakeLightThreshold = 0.02f;
}

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
	// Overrides are per slot index, and the new body's slots need not line up
	// with the old one's.
	CarMesh->EmptyOverrideMaterials();
	CarMesh->SetStaticMesh(Loaded);
	// A different body is a different seat.
	bCockpitLayoutValid = false;

	BrakeLightMaterial = nullptr;
	const int32 BrakeSlot = Loaded ? CarMesh->GetMaterialIndex(BrakeLightSlot) : INDEX_NONE;
	if (BrakeSlot != INDEX_NONE)
	{
		BrakeLightMaterial = CarMesh->CreateDynamicMaterialInstance(BrakeSlot);
	}
	// Force the next update to write the parameter: the imported material ships lit.
	bBrakeLightsOn = true;
	UpdateBrakeLights();
}

void AApexRaceCarActor::UpdateBrakeLights()
{
	const bool bOn = Brake > BrakeLightThreshold;
	if (bOn == bBrakeLightsOn || !BrakeLightMaterial)
	{
		bBrakeLightsOn = bOn;
		return;
	}
	bBrakeLightsOn = bOn;
	BrakeLightMaterial->SetScalarParameterValue(EmissiveStrengthParam, bOn ? CVarBrakeLightNits.GetValueOnGameThread() : 0.0f);
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

FBox AApexRaceCarActor::GetBodyBox() const
{
	const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	return Mesh ? ApexCockpit::ActorFrameBox(Mesh->GetBounds(), CarMesh->GetRelativeTransform()) : ApexCockpit::FallbackBox();
}

void AApexRaceCarActor::SetEngineVolume(float Scale)
{
	if (EngineAudio)
	{
		EngineAudio->SetVolumeMultiplier(FMath::Max(Scale, 0.0f));
	}
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
	UpdateBrakeLights();
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

	// Neither end of the rev range is broadcast, so both grow to fit what has
	// actually been seen — same trick as the HUD's RPM strip, but from both
	// ends: idle is 900rpm in the GT3 and 4500 in the F1, and assuming the
	// lower left the F1 sounding a quarter opened up while stood on the grid.
	// The car's first frame is the grid, so the first reading *is* idle.
	if (bHasEngineRange)
	{
		ObservedIdleRpm = FMath::Min(ObservedIdleRpm, EngineRpm);
		ObservedMaxRpm = FMath::Max(ObservedMaxRpm, EngineRpm);
	}
	else
	{
		ObservedIdleRpm = EngineRpm;
		ObservedMaxRpm = EngineRpm;
		bHasEngineRange = true;
	}
	if (EngineSound)
	{
		EngineSound->SetRpmRange(ObservedIdleRpm, ObservedMaxRpm);
		EngineSound->SetLive(EngineRpm, Throttle, Gear);
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
