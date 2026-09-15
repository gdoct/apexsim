#include "Race/ApexRaceCarActor.h"

#include "Audio/ApexEngineSoundWave.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Engine.h"
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

	/**
	 * How far behind the newest telemetry frame a car is drawn, in frames.
	 * Two frames (33 ms at 60Hz) rides out the usual lumping of arrivals;
	 * lower is closer to live but runs off the end of the data more often.
	 */
	TAutoConsoleVariable<float> CVarInterpDelayFrames(
		TEXT("apexsim.car.InterpDelayFrames"),
		2.0f,
		TEXT("Telemetry frames a car is drawn behind the newest sample (blend room; lower is closer to live)"),
		ECVF_Default);

	/** How long a car keeps moving on its last velocity once telemetry stops. */
	TAutoConsoleVariable<float> CVarInterpMaxExtrapolationMs(
		TEXT("apexsim.car.InterpMaxExtrapolationMs"),
		250.0f,
		TEXT("Longest a car is dead-reckoned past its newest telemetry sample before it holds still"),
		ECVF_Default);

	/** On-screen readout of every car's motion buffer. */
	TAutoConsoleVariable<int32> CVarInterpDebug(
		TEXT("apexsim.car.InterpDebug"),
		0,
		TEXT("1: show each car's motion buffer (tick rate estimate, lag, extrapolation) on screen"),
		ECVF_Default);

	/**
	 * Most a wheel is drawn turning in one frame, degrees. Real road speed
	 * is 40 turns a second on an F1 car: drawn true, the motion blur smears
	 * the wheel into a disc. 12 stays under half the F1 wheel's 30° spoke
	 * pitch, so the spokes never appear to run backwards.
	 */
	TAutoConsoleVariable<float> CVarWheelMaxDegPerFrame(
		TEXT("apexsim.car.WheelMaxDegPerFrame"),
		12.0f,
		TEXT("Most a car's wheel is drawn turning per frame, in degrees (0: true road speed, blurred)"),
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

	// The wheels ride the body mesh, so they share its frame: nose +Y, left +X.
	Wheels.CreateComponents(*this, CarMesh);

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

void AApexRaceCarActor::SetWheels(const FApexWheelSpec& Spec)
{
	if (Spec == Wheels.GetSpec() && Wheels.HasWheels() == Spec.IsUsable())
	{
		return;
	}
	Wheels.SetSpec(Spec);
	// The layout box includes the wheels.
	bCockpitLayoutValid = false;
}

FBoxSphereBounds AApexRaceCarActor::BodyBounds() const
{
	const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	if (!Mesh)
	{
		return FBoxSphereBounds(FVector::ZeroVector, FVector::ZeroVector, 0.0f);
	}
	FBox Box = Mesh->GetBounds().GetBox();
	if (Wheels.HasWheels())
	{
		Box += ApexWheels::WheelsBox(Wheels.GetSpec());
	}
	return FBoxSphereBounds(Box);
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
	// Not propagated to children: the wheels are set explicitly so they go
	// with the bodywork they belong to.
	CarMesh->SetVisibility(bVisible);
	Wheels.SetVisible(bVisible);
}

void AApexRaceCarActor::SetCockpitSpec(const FString& InCarClass, const FApexCockpitOverrides& InOverrides)
{
	CarClass = InCarClass;
	CockpitOverrides = InOverrides;
	bCockpitLayoutValid = false;
}

FBox AApexRaceCarActor::GetBodyBox() const
{
	return CarMesh->GetStaticMesh()
		? ApexCockpit::ActorFrameBox(BodyBounds(), CarMesh->GetRelativeTransform())
		: ApexCockpit::FallbackBox();
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
		// Wheels included: the layout was tuned on bodies that still had them.
		const FBox Body = GetBodyBox();
		const EApexCockpitStyle Style = ApexCockpit::ResolveStyle(CockpitOverrides.Style, CarClass, Body);
		CockpitLayout = ApexCockpit::DeriveLayout(Body, Style, CockpitOverrides);
		bCockpitLayoutValid = true;
	}
	return CockpitLayout;
}

ApexMotion::FSettings AApexRaceCarActor::MotionSettings() const
{
	ApexMotion::FSettings Settings;
	Settings.DelayFrames = CVarInterpDelayFrames.GetValueOnGameThread();
	Settings.MaxExtrapolationSeconds = FMath::Max(CVarInterpMaxExtrapolationMs.GetValueOnGameThread(), 0.0f) / 1000.0f;
	Settings.TeleportDistance = TeleportDistanceCm;
	return Settings;
}

void AApexRaceCarActor::ApplyTelemetry(const FApexCarTelemetry& Car, int64 ServerTick)
{
	CarIndex = Car.CarIndex;
	// Kept so a shift request can be expressed relative to it; the protocol
	// wants an absolute target gear, not a delta.
	Gear = Car.Gear;
	Throttle = Car.Throttle;
	Brake = Car.Brake;
	UpdateBrakeLights();
	CurrentLap = Car.CurrentLap;
	CurrentLapTimeMs = Car.CurrentLapTimeMs;

	ApexMotion::FSnapshot Snapshot;
	Snapshot.Tick = ServerTick;
	Snapshot.Location = ApexRace::ServerToUnrealPosition(Car.Position);
	Snapshot.Rotation = ApexRace::ServerToUnrealRotation(Car.YawRad, Car.PitchRad, Car.RollRad).Quaternion();
	Snapshot.Steering = Car.Steering;
	Snapshot.SpeedMps = Car.SpeedMps;
	Snapshot.EngineRpm = Car.EngineRpm;
	const ApexMotion::EPushResult Pushed = Motion.Push(Snapshot, FPlatformTime::Seconds(), MotionSettings());

	// First frame, or a jump too large to be real motion: go straight there.
	// Tick reads the same pose back until a second sample gives it motion.
	if (Pushed == ApexMotion::EPushResult::Restarted)
	{
		SetActorLocationAndRotation(Snapshot.Location, Snapshot.Rotation);
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
		ObservedIdleRpm = FMath::Min(ObservedIdleRpm, Car.EngineRpm);
		ObservedMaxRpm = FMath::Max(ObservedMaxRpm, Car.EngineRpm);
	}
	else
	{
		ObservedIdleRpm = Car.EngineRpm;
		ObservedMaxRpm = Car.EngineRpm;
		bHasEngineRange = true;
	}
	if (EngineSound)
	{
		// The synth smooths its own inputs, and the note should follow the
		// revs as soon as they are known rather than two frames later.
		EngineSound->SetRpmRange(ObservedIdleRpm, ObservedMaxRpm);
		EngineSound->SetLive(Car.EngineRpm, Throttle, Gear);
	}
	if (Motion.Num() < 2)
	{
		// Until the buffer can blend, the dials show the sample itself.
		SpeedMps = Car.SpeedMps;
		EngineRpm = Car.EngineRpm;
		Steering = Car.Steering;
	}
}

void AApexRaceCarActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bHasTarget)
	{
		return;
	}

	ApexMotion::FPose Pose;
	if (!Motion.Sample(DeltaSeconds, MotionSettings(), Pose))
	{
		return;
	}
	SetActorLocationAndRotation(Pose.Location, Pose.Rotation);
	// Nothing else sets a velocity on a puppet; the audio's doppler and
	// anything asking the actor read this.
	Root->ComponentVelocity = Pose.Velocity;
	SpeedMps = Pose.SpeedMps;
	EngineRpm = Pose.EngineRpm;
	Steering = Pose.Steering;
	// Speed is a magnitude on the wire; reverse gear is the only way back.
	Wheels.Update(Steering, Gear < 0 ? -SpeedMps : SpeedMps, DeltaSeconds,
		FMath::DegreesToRadians(CVarWheelMaxDegPerFrame.GetValueOnGameThread()));

	if (CVarInterpDebug.GetValueOnGameThread() != 0 && GEngine)
	{
		const double Rate = Motion.GetTicksPerSecond();
		const double LagMs = Rate > 0.0 ? Motion.GetLagTicks() / Rate * 1000.0 : 0.0;
		GEngine->AddOnScreenDebugMessage(
			static_cast<uint64>(0x4D4F00) + static_cast<uint64>(FMath::Max(CarIndex, 0)), 0.5f,
			Pose.bExtrapolated ? FColor::Orange : FColor::Green,
			FString::Printf(TEXT("car %d %s: %d buffered, tick rate %.0f/s, spacing %lld, lag %.1f ms%s"),
				CarIndex, *DisplayName, Motion.Num(), Rate, Motion.GetFrameSpacingTicks(), LagMs,
				Pose.bExtrapolated ? TEXT(" EXTRAPOLATING") : TEXT("")));
	}
}
