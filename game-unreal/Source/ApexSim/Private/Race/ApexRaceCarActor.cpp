#include "Race/ApexRaceCarActor.h"

#include "Audio/ApexEngineSoundWave.h"
#include "Audio/ApexRoadSoundWave.h"
#include "Components/AudioComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Engine.h"
#include "Race/ApexCarLivery.h"
#include "Race/ApexRaceCoordinate.h"
#include "Sound/SoundAttenuation.h"

namespace
{
	/**
	 * Emissive brightness of a lit brake light, as a multiple of the slot's
	 * authored colour. The race is exposed for a 50 klux sun, where the GLB's
	 * own emission is invisible; the start lights read at 4000. A knob so it
	 * can be tuned against a screenshot.
	 */
	TAutoConsoleVariable<float> CVarBrakeLightNits(
		TEXT("apexsim.car.BrakeLightNits"),
		3000.0f,
		TEXT("Brightness of a car's brake lights while braking, as an emissive multiplier"),
		ECVF_Default);

	/**
	 * Emissive brightness of the running (tail) lights in daylight. A race car
	 * runs its tail lights all session; left at the GLB's own emission the
	 * `car_taillight` slot was invisible under the 50 klux exposure and only
	 * the brake lights ever showed. Dimmer than a brake light so braking still
	 * reads; with the headlights on the tails drop to the running-light share
	 * of the brake glow, since the night exposure lifts them by itself.
	 */
	TAutoConsoleVariable<float> CVarTailLightNits(
		TEXT("apexsim.car.TailLightNits"),
		700.0f,
		TEXT("Brightness of a car's tail (running) lights in daylight, as an emissive multiplier"),
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
	/** ... and its running lights (always on while racing). */
	const FName TailLightSlot(TEXT("car_taillight"));

	/**
	 * The Interchange glTF parent's emissive colour. Its `EmissiveStrength`
	 * parameter is imported but does not reach the shader (setting it to 1e5
	 * changed nothing on screen), so the lights scale the colour itself.
	 */
	const FName EmissiveFactorParam(TEXT("EmissiveFactor"));

	/**
	 * Pedal travel that turns the lights on. A real brake-light switch is
	 * on/off at the first bit of pedal, not dimmed by how hard it is pressed.
	 */
	constexpr float BrakeLightThreshold = 0.02f;
	/** Share of the brake glow the tail lights hold while the headlights are on. */
	constexpr float RunningLightShare = 0.12f;

	TAutoConsoleVariable<float> CVarHeadlightLumens(
		TEXT("apexsim.car.HeadlightLumens"),
		2500.0f,
		TEXT("Luminous flux of each headlight, lumens."));
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
	DrsFlap.CreateComponent(*this, CarMesh);

	// Nothing places the car until the first telemetry frame; until then it
	// would sit at the world origin, which on most circuits is in mid-air or
	// underground next to the grid. Stay hidden until the server says where.
	SetActorHiddenInGame(true);

	EngineAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("EngineAudio"));
	EngineAudio->SetupAttachment(Root);
	EngineAudio->bAutoActivate = false;
	// No SoundAttenuation asset exists for this, so the falloff is set directly
	// on the component. Somebody else's car: full level only alongside (4 m),
	// then falling steadily in dB to nothing at 150 m, and losing its top end
	// to the air on the way, so a car up the road is a dull drone and the one
	// being passed is a roar. (It used to be at full level within 15 m and
	// silent at 55: a whole grid of engines as loud as the player's own.)
	FSoundAttenuationSettings& Falloff = EngineAudio->AttenuationOverrides;
	EngineAudio->bOverrideAttenuation = true;
	Falloff.bAttenuate = true;
	Falloff.bSpatialize = true;
	Falloff.AttenuationShape = EAttenuationShape::Sphere;
	Falloff.AttenuationShapeExtents = FVector(400.0f, 0.0f, 0.0f);
	Falloff.FalloffDistance = 15000.0f;
	Falloff.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
	Falloff.dBAttenuationAtMax = -50.0f;
	Falloff.bAttenuateWithLPF = true;
	Falloff.LPFRadiusMin = 1000.0f;
	Falloff.LPFRadiusMax = 12000.0f;
	Falloff.LPFFrequencyAtMin = 20000.0f;
	Falloff.LPFFrequencyAtMax = 1500.0f;

	// The player's own car is not a point in the world: see SetListenerSeat.
	OwnEngineAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("OwnEngineAudio"));
	OwnEngineAudio->SetupAttachment(Root);
	OwnEngineAudio->bAutoActivate = false;
	OwnEngineAudio->bAllowSpatialization = false;

	// Only ever played for the car being driven, whose driver sits in it: no
	// attenuation and no panning, which at a listener a metre from the source
	// would swing from ear to ear with every look to the side.
	RoadAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("RoadAudio"));
	RoadAudio->SetupAttachment(Root);
	RoadAudio->bAutoActivate = false;
	RoadAudio->bAllowSpatialization = false;
}

void AApexRaceCarActor::BeginPlay()
{
	Super::BeginPlay();

	// Procedural, not an asset: see UApexEngineSoundWave for why. Built here
	// rather than in the constructor so it is never part of the CDO.
	EngineSound = NewObject<UApexEngineSoundWave>(this);
	EngineAudio->SetSound(EngineSound);
	OwnEngineSound = UApexEngineSoundWave::MakeOwnCar(this);
	OwnEngineAudio->SetSound(OwnEngineSound);
	RoadSound = NewObject<UApexRoadSoundWave>(this);
	RoadAudio->SetSound(RoadSound);
}

void AApexRaceCarActor::SetCarMesh(const TSoftObjectPtr<UStaticMesh>& MeshToShow)
{
	UStaticMesh* Loaded = MeshToShow.IsNull() ? nullptr : MeshToShow.LoadSynchronous();
	// Overrides are per slot index, and the new body's slots need not line up
	// with the old one's.
	CarMesh->EmptyOverrideMaterials();
	bLiveryApplied = false;
	CarMesh->SetStaticMesh(Loaded);
	// A different body is a different seat.
	bCockpitLayoutValid = false;

	// Normalised authored colour, so the cvars alone set how bright the lights are.
	auto LightSlot = [this, Loaded](FName Slot, FLinearColor& OutColor) -> UMaterialInstanceDynamic*
	{
		OutColor = FLinearColor::Red;
		const int32 Index = Loaded ? CarMesh->GetMaterialIndex(Slot) : INDEX_NONE;
		UMaterialInstanceDynamic* Instance = Index != INDEX_NONE ? CarMesh->CreateDynamicMaterialInstance(Index) : nullptr;
		FLinearColor Authored;
		if (Instance
			&& Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(EmissiveFactorParam), Authored)
			&& Authored.GetMax() > UE_KINDA_SMALL_NUMBER)
		{
			OutColor = Authored / Authored.GetMax();
			OutColor.A = 1.0f;
		}
		return Instance;
	};
	BrakeLightMaterial = LightSlot(BrakeLightSlot, BrakeLightColor);
	TailLightMaterial = LightSlot(TailLightSlot, TailLightColor);
	// Force the next update to write the parameter: the imported material ships lit.
	bBrakeLightsOn = true;
	TailLightState = -1;
	UpdateBrakeLights();
	PlaceHeadlights();
}

void AApexRaceCarActor::SetLivery(const FApexCarLivery* Livery)
{
	// Livery 0 on a fresh body has nothing to undo; the ghost relies on that,
	// since it puts its own materials on after the mesh.
	if (Livery || bLiveryApplied)
	{
		ApexLivery::Apply(CarMesh, Livery);
		// The flap is painted like the wing it was cut from.
		if (UStaticMeshComponent* Flap = GetDrsFlapComponent())
		{
			ApexLivery::Apply(Flap, Livery);
		}
	}
	bLiveryApplied = Livery != nullptr;
}

void AApexRaceCarActor::SetDrsFlap(const FApexDrsFlapSpec& Spec)
{
	if (Spec == DrsFlap.GetSpec() && DrsFlap.HasFlap() == Spec.IsUsable())
	{
		return;
	}
	DrsFlap.SetSpec(Spec);
	bDrsOpen = false;
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
	bBrakeLightsOn = bOn;
	const int32 Wanted = bOn ? 2 : (bHeadlightsOn ? 1 : 0);
	if (Wanted == TailLightState || (!BrakeLightMaterial && !TailLightMaterial))
	{
		return;
	}
	TailLightState = Wanted;
	const float Nits = CVarBrakeLightNits.GetValueOnGameThread();
	const float Running = bHeadlightsOn ? Nits * RunningLightShare : CVarTailLightNits.GetValueOnGameThread();
	if (BrakeLightMaterial)
	{
		const FLinearColor Glow = Wanted == 2 ? BrakeLightColor * Nits
			: Wanted == 1 ? BrakeLightColor * (Nits * RunningLightShare)
			: FLinearColor::Black;
		BrakeLightMaterial->SetVectorParameterValue(EmissiveFactorParam, Glow);
	}
	if (TailLightMaterial)
	{
		// Running lights stay on whatever the pedal does; only the level changes with the sky.
		TailLightMaterial->SetVectorParameterValue(EmissiveFactorParam, TailLightColor * Running);
	}
}

void AApexRaceCarActor::SetHeadlights(bool bOn)
{
	if (bOn == bHeadlightsOn && (!bOn || HeadlightLeft))
	{
		return;
	}
	bHeadlightsOn = bOn;
	if (bOn && !HeadlightLeft)
	{
		auto MakeLamp = [this](const TCHAR* Name) {
			USpotLightComponent* Lamp = NewObject<USpotLightComponent>(this, Name);
			Lamp->SetMobility(EComponentMobility::Movable);
			Lamp->SetIntensityUnits(ELightUnits::Lumens);
			Lamp->SetIntensity(CVarHeadlightLumens.GetValueOnGameThread());
			Lamp->SetLightColor(FLinearColor(1.0f, 0.96f, 0.88f));
			Lamp->SetAttenuationRadius(9000.0f);
			Lamp->SetInnerConeAngle(16.0f);
			Lamp->SetOuterConeAngle(34.0f);
			// Twenty cars, forty lamps: shadows would be the whole frame budget.
			Lamp->SetCastShadows(false);
			Lamp->bAffectsWorld = true;
			Lamp->SetupAttachment(Root);
			Lamp->RegisterComponent();
			return Lamp;
		};
		HeadlightLeft = MakeLamp(TEXT("HeadlightLeft"));
		HeadlightRight = MakeLamp(TEXT("HeadlightRight"));
		PlaceHeadlights();
	}
	if (HeadlightLeft)  { HeadlightLeft->SetVisibility(bOn); }
	if (HeadlightRight) { HeadlightRight->SetVisibility(bOn); }
	// The tail lights follow: dim running lights with the headlights on.
	TailLightState = -1;
	UpdateBrakeLights();
}

void AApexRaceCarActor::PlaceHeadlights()
{
	if (!HeadlightLeft || !HeadlightRight)
	{
		return;
	}
	// The body box is in the car's frame: nose +X, left +Y, up +Z. Lamps sit
	// just inside the nose, out at the corners, a little under half height,
	// and aim a touch down onto the road.
	const FBox Box = GetBodyBox();
	const FVector Size = Box.GetSize();
	if (Size.X < 1.0f)
	{
		return;
	}
	const float X = Box.Max.X - Size.X * 0.04f;
	const float Y = Size.Y * 0.36f;
	const float Z = Box.Min.Z + Size.Z * 0.42f;
	const FRotator Aim(-3.0f, 0.0f, 0.0f);
	HeadlightLeft->SetRelativeLocationAndRotation(FVector(X, Y, Z), Aim);
	HeadlightRight->SetRelativeLocationAndRotation(FVector(X, -Y, Z), Aim);
}

void AApexRaceCarActor::SetPuppetState(
	float InSpeedMps, float InEngineRpm, int32 InGear, float InSteering, float InThrottle, float InBrake)
{
	SpeedMps = InSpeedMps;
	EngineRpm = InEngineRpm;
	Gear = InGear;
	Steering = InSteering;
	Throttle = InThrottle;
	Brake = InBrake;
	UpdateBrakeLights();
}

void AApexRaceCarActor::SetMeshVisible(bool bVisible)
{
	// Not propagated to children: the wheels are set explicitly so they go
	// with the bodywork they belong to.
	CarMesh->SetVisibility(bVisible);
	Wheels.SetVisible(bVisible);
	DrsFlap.SetVisible(bVisible);
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

void AApexRaceCarActor::SetEngineSound(const FApexEngineSoundSpec& Spec, const FString& InCarClass)
{
	const ApexEngineSynth::FEngineSpec Engine = ApexEngineAudio::MakeSpec(Spec, InCarClass);
	for (UApexEngineSoundWave* Wave : {EngineSound.Get(), OwnEngineSound.Get()})
	{
		if (Wave)
		{
			Wave->SetSpec(Engine);
		}
	}
}

void AApexRaceCarActor::SetEngineVolume(float Scale)
{
	VolumeScale = FMath::Max(Scale, 0.0f);
	ApplyVolumes();
}

void AApexRaceCarActor::SetMixVolumes(float Engine, float OtherCars, float Road)
{
	EngineMix = FMath::Clamp(Engine, 0.0f, 1.0f);
	OtherCarsMix = FMath::Clamp(OtherCars, 0.0f, 1.0f);
	RoadMix = FMath::Clamp(Road, 0.0f, 1.0f);
	ApplyVolumes();
}

void AApexRaceCarActor::ApplyVolumes()
{
	if (EngineAudio)
	{
		EngineAudio->SetVolumeMultiplier(VolumeScale * EngineMix * OtherCarsMix);
	}
	if (OwnEngineAudio)
	{
		OwnEngineAudio->SetVolumeMultiplier(VolumeScale * EngineMix);
	}
	if (RoadAudio)
	{
		RoadAudio->SetVolumeMultiplier(VolumeScale * RoadMix);
	}
}

void AApexRaceCarActor::UpdateRoadSound(const ApexRoadSynth::FInputs& Levels, float BumpMps, float ImpactMps)
{
	if (!RoadSound || !RoadAudio)
	{
		return;
	}
	RoadSound->SetLive(Levels);
	if (BumpMps > 0.0f || ImpactMps > 0.0f)
	{
		RoadSound->Hit(BumpMps, ImpactMps);
	}
	if (!RoadAudio->IsPlaying())
	{
		RoadAudio->Play();
	}
}

void AApexRaceCarActor::StopRoadSound()
{
	if (RoadAudio && RoadAudio->IsPlaying())
	{
		RoadAudio->Stop();
	}
}

void AApexRaceCarActor::SetListenerSeat(ApexSpace::ESeat Seat)
{
	// An open cockpit has no bulkhead between the driver and the engine.
	if (Seat == ApexSpace::ESeat::Cabin && GetCockpitLayout().bOpenWheel)
	{
		Seat = ApexSpace::ESeat::OpenCockpit;
	}
	if (Seat == ListenerSeat)
	{
		return;
	}
	ListenerSeat = Seat;
	if (OwnEngineSound)
	{
		OwnEngineSound->SetSeat(Seat);
	}
	RefreshEnginePlayback();
}

void AApexRaceCarActor::RefreshEnginePlayback()
{
	if (!bHasTarget || !EngineAudio || !OwnEngineAudio)
	{
		return;
	}
	const bool bOwn = ListenerSeat != ApexSpace::ESeat::None;
	UAudioComponent* Wanted = bOwn ? OwnEngineAudio.Get() : EngineAudio.Get();
	UAudioComponent* Other = bOwn ? EngineAudio.Get() : OwnEngineAudio.Get();
	if (Other->IsPlaying())
	{
		Other->Stop();
	}
	if (!Wanted->IsPlaying())
	{
		Wanted->Play();
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
	bDrsOpen = Car.bDrsOpen;
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
		bHasTarget = true;
		RefreshEnginePlayback();
	}

	if (Motion.Num() < 2)
	{
		// Until the buffer can blend, the dials show the sample itself.
		SpeedMps = Car.SpeedMps;
		EngineRpm = Car.EngineRpm;
		Steering = Car.Steering;
	}
}

void AApexRaceCarActor::SetPlaybackPose(const FApexCarTelemetry& Car, float DeltaSeconds)
{
	CarIndex = Car.CarIndex;
	CurrentLap = Car.CurrentLap;
	CurrentLapTimeMs = Car.CurrentLapTimeMs;
	bPlaybackPose = true;

	const FVector Location = ApexRace::ServerToUnrealPosition(Car.Position);
	const FQuat Rotation = ApexRace::ServerToUnrealRotation(Car.YawRad, Car.PitchRad, Car.RollRad).Quaternion();
	const FVector PreviousLocation = GetActorLocation();
	const bool bFirst = !bHasTarget;
	SetActorLocationAndRotation(Location, Rotation);
	const bool bJumped = bFirst || FVector::Dist(PreviousLocation, Location) > TeleportDistanceCm;
	Root->ComponentVelocity = !bJumped && DeltaSeconds > 0.0f
		? (Location - PreviousLocation) / DeltaSeconds
		: Rotation.GetForwardVector() * Car.SpeedMps * ApexRace::MetresToCentimetres;

	SetPuppetState(Car.SpeedMps, Car.EngineRpm, Car.Gear, Car.Steering, Car.Throttle, Car.Brake);
	if (EngineSound && OwnEngineSound)
	{
		EngineSound->SetLive(EngineRpm, Throttle, Gear);
		OwnEngineSound->SetLive(EngineRpm, Throttle, Gear);
	}
	Wheels.Update(Steering,
		bJumped ? 0.0f : ApexWheels::RolledDistanceM(PreviousLocation, Location, Rotation, TeleportDistanceCm),
		FMath::DegreesToRadians(CVarWheelMaxDegPerFrame.GetValueOnGameThread()));

	if (bFirst)
	{
		SetActorHiddenInGame(false);
		bHasTarget = true;
		RefreshEnginePlayback();
	}
}

void AApexRaceCarActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bHasTarget || bPlaybackPose)
	{
		return;
	}

	ApexMotion::FPose Pose;
	if (!Motion.Sample(DeltaSeconds, MotionSettings(), Pose))
	{
		return;
	}
	const FVector PreviousLocation = GetActorLocation();
	SetActorLocationAndRotation(Pose.Location, Pose.Rotation);
	// Nothing else sets a velocity on a puppet; the audio's doppler and
	// anything asking the actor read this.
	Root->ComponentVelocity = Pose.Velocity;
	SpeedMps = Pose.SpeedMps;
	EngineRpm = Pose.EngineRpm;
	Steering = Pose.Steering;
	if (EngineSound && OwnEngineSound)
	{
		// The blended revs, every render frame: the telemetry's own 60Hz steps
		// are a zipper on a synthesised crank, and this is the same reading the
		// dials show. Throttle and gear are the newest sample's — a lift or a
		// shift should be heard at once, not blended into.
		EngineSound->SetLive(EngineRpm, Throttle, Gear);
		OwnEngineSound->SetLive(EngineRpm, Throttle, Gear);
	}
	// Rolled from how far the drawn car actually moved, not from the wire's
	// speed: that is the length of the whole velocity, so a car standing on
	// its springs or sliding sideways would turn its wheels, and it has no
	// sign for reversing.
	Wheels.Update(Steering,
		ApexWheels::RolledDistanceM(PreviousLocation, Pose.Location, Pose.Rotation, TeleportDistanceCm),
		FMath::DegreesToRadians(CVarWheelMaxDegPerFrame.GetValueOnGameThread()));
	// Swung, not snapped: the telemetry flips the flag in one frame, a real
	// actuator takes a fifth of a second.
	DrsFlap.Update(bDrsOpen, DeltaSeconds);

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
