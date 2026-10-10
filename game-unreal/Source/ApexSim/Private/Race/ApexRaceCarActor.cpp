#include "Race/ApexRaceCarActor.h"

#include "ApexSim.h"

#include "Audio/ApexEngineSoundWave.h"
#include "Audio/ApexRoadSoundWave.h"
#include "Cars/ApexCarContentSubsystem.h"
#include "Cars/ApexCarToml.h"
#include "Components/AudioComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Engine.h"
#include "Race/ApexCarEffectsActor.h"
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

	/** The material slot every car GLB gives its brake lights (docs/content/cars.md). */
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
	/** A headlight flash (full beam) against the dipped beam. */
	constexpr float HeadlightFullBeamScale = 2.5f;

	/**
	 * Damage to draw on every car instead of the server's, percent: "front,
	 * rear, left, right, engine" (e.g. "60,40,30,20,80"); empty uses the
	 * telemetry. For looking at the dents, parts and smoke without a crash.
	 */
	TAutoConsoleVariable<FString> CVarDamagePreview(
		TEXT("apexsim.car.DamagePreview"),
		TEXT(""),
		TEXT("Damage drawn on every car instead of the server's: \"front,rear,left,right,engine\" percent; empty for the telemetry"),
		ECVF_Default);

	/** Smoke, steam, sparks and thrown parts; dents are drawn either way. */
	TAutoConsoleVariable<int32> CVarDamageEffects(
		TEXT("apexsim.car.DamageEffects"),
		1,
		TEXT("1: damaged cars smoke, steam, spark and throw off parts; 0: dents only"),
		ECVF_Default);

	/** "60,40,30,20,80" -> five percentages; false unless it is five numbers. */
	bool ParseDamagePreview(const FString& Text, float Out[ApexDamage::NumZones])
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() != ApexDamage::NumZones)
		{
			return false;
		}
		for (int32 i = 0; i < ApexDamage::NumZones; ++i)
		{
			Out[i] = FCString::Atof(*Parts[i].TrimStartAndEnd());
		}
		return true;
	}

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
	Driver.CreateComponent(*this, CarMesh);

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

void AApexRaceCarActor::SetCarMesh(UStaticMesh* MeshToShow)
{
	UStaticMesh* Loaded = MeshToShow;
	// Overrides are per slot index, and the new body's slots need not line up
	// with the old one's.
	CarMesh->EmptyOverrideMaterials();
	// The wheels may outlive the body (the same wheel spec): a skin's rims go with the skin.
	Wheels.ForEachComponent([](UStaticMeshComponent& Wheel) { ApexLivery::Apply(&Wheel, nullptr); });
	bLiveryApplied = false;
	CarMesh->SetStaticMesh(Loaded);
	// A different body is a different seat.
	InvalidateCockpitLayout();
	// ... and a different frame for the dents; the parts belong to the old body.
	SetDamageParts({}, {});

	// Normalised authored colour, so the cvars alone set how bright the lights are.
	auto LightSlot = [this, Loaded](FName Slot, FLinearColor& OutColor) -> UMaterialInstanceDynamic*
	{
		OutColor = FLinearColor::Red;
		const int32 Index = Loaded ? CarMesh->GetMaterialIndex(Slot) : INDEX_NONE;
		// This car's own instance: a runtime body's slot is shared by every car wearing it.
		UMaterialInstanceDynamic* Instance = Index != INDEX_NONE ? ApexCarContent::OwnMaterialInstance(*CarMesh, Index) : nullptr;
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
		// The driver's helmet is in the livery's paint and accent.
		if (UStaticMeshComponent* Figure = GetDriverComponent())
		{
			ApexLivery::Apply(Figure, Livery);
		}
		// The parts were cut from the painted body.
		ForEachDamagePartComponent([Livery](UStaticMeshComponent& Part) { ApexLivery::Apply(&Part, Livery); });
		// A skin may retexture the rims; a colour livery finds no slot of its
		// own on a wheel and leaves it alone.
		Wheels.ForEachComponent([Livery](UStaticMeshComponent& Wheel) { ApexLivery::Apply(&Wheel, Livery); });
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

void AApexRaceCarActor::SetDriver(const FApexDriverSpec& Spec)
{
	if (Spec == Driver.GetSpec() && Driver.HasDriver() == Spec.IsUsable())
	{
		return;
	}
	Driver.SetSpec(Spec);
}

void AApexRaceCarActor::SetDriverVisible(bool bVisible)
{
	Driver.SetDriverVisible(bVisible);
}

void AApexRaceCarActor::SetCompounds(const TArray<FApexCompoundSpec>& Specs)
{
	Compounds = Specs.IsEmpty() ? ApexCarToml::DefaultCompounds() : Specs;
	UpdateTyreLook();
}

void AApexRaceCarActor::UpdateTyreLook()
{
	// An unknown compound (an older server, a car not yet on its tyres) is drawn as a slick.
	const EApexCompoundKind Kind = Compounds.IsValidIndex(TelemetryCompound) ? Compounds[TelemetryCompound].Kind : EApexCompoundKind::Slick;
	Wheels.SetTyreLook(Kind);
}

void AApexRaceCarActor::TakeTyreTelemetry(const FApexCarTelemetry& Car)
{
	for (int32 Wheel = 0; Wheel < ApexWheels::NumWheels; ++Wheel)
	{
		TyreWear[Wheel] = Car.TyreWearPct[Wheel] >= 0.0f ? Car.TyreWearPct[Wheel] / 100.0f : -1.0f;
		TyrePressureKpa[Wheel] = Car.TyrePressureKpa[Wheel];
		TyreFlatSpot[Wheel] = Car.FlatSpot[Wheel];
	}
}

void AApexRaceCarActor::UpdateTyreSurface(float DeltaSeconds)
{
	// One figure for the car: the road state is read under its middle.
	TyreWetness = ApexWheels::WetnessStep(TyreWetness, ApexWheels::WetnessForWater(RoadWaterPct), DeltaSeconds);
	for (int32 Wheel = 0; Wheel < ApexWheels::NumWheels; ++Wheel)
	{
		FApexTyreSurface Surface;
		Surface.Wear = FMath::Max(TyreWear[Wheel], 0.0f);
		Surface.Wetness = TyreWetness;
		Surface.FlatSpot = TyreFlatSpot[Wheel];
		Surface.PressureKpa = TyrePressureKpa[Wheel];
		Wheels.SetTyreSurface(static_cast<ApexWheels::EWheel>(Wheel), Surface);
	}
}

void AApexRaceCarActor::UpdateTyreSmoke(float DeltaSeconds)
{
	// The server decides what smokes (`slide_flags`: spinning past 1.6x the
	// peak slip ratio, locked, or past 2.4x the peak slip angle, on tarmac,
	// above 5 m/s); the client only draws it. Puffs owed per wheel like the
	// engine smoke, so a slow frame does not skip a puff nor a fast one double it.
	AApexCarEffectsActor* Fx = nullptr;
	for (int32 Wheel = 0; Wheel < ApexWheels::NumWheels; ++Wheel)
	{
		if (!bTyreSliding[Wheel] || DeltaSeconds <= 0.0f)
		{
			TyreSmokeOwed[Wheel] = 0.0f;
			continue;
		}
		if (!Fx)
		{
			Fx = GetEffects();
			if (!Fx || !Fx->CanDraw())
			{
				return;
			}
		}
		// A locked wheel grinds one patch and smokes about twice as much as a spinning or scrubbing one.
		const float PuffsPerSecond = bTyreLocked[Wheel] ? 9.0f : 5.0f;
		TyreSmokeOwed[Wheel] += PuffsPerSecond * DeltaSeconds;
		FVector Patch;
		if (!Wheels.ContactPatch(static_cast<ApexWheels::EWheel>(Wheel), Patch))
		{
			TyreSmokeOwed[Wheel] = 0.0f;
			continue;
		}
		const FVector CarVelocity = Root->ComponentVelocity;
		FRandomStream& R = DamageRandom;
		for (; TyreSmokeOwed[Wheel] >= 1.0f; TyreSmokeOwed[Wheel] -= 1.0f)
		{
			ApexDamage::FPuff Puff;
			// Born just behind and inside the patch, so it rolls out from under the tyre.
			Puff.Location = Patch + FVector(R.FRandRange(-8.0f, 8.0f), R.FRandRange(-8.0f, 8.0f), R.FRandRange(2.0f, 10.0f));
			// Left on the road more than carried: rubber smoke hangs where it was made, drifting back
			// in the car's wake and rising slowly.
			Puff.Velocity = CarVelocity * 0.15 + FVector(0.0, 0.0, 45.0) + R.GetUnitVector() * 30.0;
			Puff.Life = R.FRandRange(1.2f, 2.0f);
			Puff.StartSize = 14.0f;
			Puff.EndSize = R.FRandRange(90.0f, 150.0f);
			Puff.Shade = FMath::Clamp(0.85f + R.FRandRange(-0.04f, 0.04f), 0.0f, 1.0f);
			Puff.Opacity = bTyreLocked[Wheel] ? 0.32f : 0.25f;
			Puff.Drag = 2.0f;
			Puff.Gravity = -0.03f;
			Fx->Emit(Puff);
		}
	}
}

void AApexRaceCarActor::SetWheels(const FApexWheelSpec& Spec)
{
	if (Spec == Wheels.GetSpec() && Wheels.HasWheels() == Spec.IsUsable())
	{
		return;
	}
	Wheels.SetSpec(Spec);
	// The layout box includes the wheels.
	InvalidateCockpitLayout();
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

void AApexRaceCarActor::SetHeadlights(bool bOn, bool bFullBeam)
{
	bFullBeam = bOn && bFullBeam;
	if (bOn == bHeadlightsOn && bFullBeam == bHeadlightsFullBeam && (!bOn || HeadlightLeft))
	{
		return;
	}
	const bool bSwitched = bOn != bHeadlightsOn;
	bHeadlightsOn = bOn;
	bHeadlightsFullBeam = bFullBeam;
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
	// A flash is full beam: brighter, and reaching the car ahead's mirrors.
	const float Lumens = CVarHeadlightLumens.GetValueOnGameThread() * (bFullBeam ? HeadlightFullBeamScale : 1.0f);
	const float Reach = bFullBeam ? 18000.0f : 9000.0f;
	for (USpotLightComponent* Lamp : { HeadlightLeft.Get(), HeadlightRight.Get() })
	{
		if (Lamp)
		{
			Lamp->SetVisibility(bOn);
			Lamp->SetIntensity(Lumens);
			Lamp->SetAttenuationRadius(Reach);
		}
	}
	if (!bSwitched)
	{
		return;
	}
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
	Driver.SetMeshVisible(bVisible);
	bBodyShown = bVisible;
	// The flap and the parts: shown with the body unless they have come off.
	ApplyPartVisibility();
}

void AApexRaceCarActor::SetCockpitSpec(const FString& InCarClass, const FApexCockpitOverrides& InOverrides)
{
	CarClass = InCarClass;
	CockpitOverrides = InOverrides;
	InvalidateCockpitLayout();
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
	FMemory::Memcpy(TelemetryDamagePct, Car.DamagePct, sizeof(TelemetryDamagePct));
	WaterTempC = Car.WaterTempC;
	bColliding = Car.bIsColliding;
	for (int32 Wheel = 0; Wheel < ApexWheels::NumWheels; ++Wheel)
	{
		bTyreSliding[Wheel] = Car.bTyreSliding[Wheel];
		bTyreLocked[Wheel] = Car.bTyreLocked[Wheel];
	}
	TelemetryCompound = Car.Compound;
	TakeTyreTelemetry(Car);

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
	// What the clip's rows hold of the tyres (an older recording: nothing,
	// so they draw new, dry of the road's water only and round).
	TakeTyreTelemetry(Car);
	UpdateTyreSurface(DeltaSeconds);
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
	UpdateDamage(DeltaSeconds);
	UpdateTyreSmoke(DeltaSeconds);
	UpdateTyreLook();
	UpdateTyreSurface(DeltaSeconds);

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

void AApexRaceCarActor::SetDamageParts(const TArray<FApexDamagePartSpec>& Specs, const TArray<UStaticMesh*>& Meshes)
{
	const int32 Count = Specs.Num() == Meshes.Num() ? Specs.Num() : 0;
	while (DamagePartMeshes.Num() < Count)
	{
		UStaticMeshComponent* Part =
			NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("DamagePart%d"), DamagePartMeshes.Num()));
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetMobility(EComponentMobility::Movable);
		// In the body's own frame, like the wheels: no transform of its own.
		Part->SetupAttachment(CarMesh);
		Part->RegisterComponent();
		DamagePartMeshes.Add(Part);
	}
	for (int32 i = 0; i < DamagePartMeshes.Num(); ++i)
	{
		if (UStaticMeshComponent* Part = DamagePartMeshes[i])
		{
			// Overrides are per slot index: the last car's livery must not land on this one.
			Part->EmptyOverrideMaterials();
			Part->SetStaticMesh(i < Count ? Meshes[i] : nullptr);
		}
	}
	DamagePartSpecs = Count > 0 ? Specs : TArray<FApexDamagePartSpec>();
	DamagePartOn.Init(true, Count);

	// The flap goes with the wing it is hinged on.
	DrsFlapPart = INDEX_NONE;
	if (DrsFlap.HasFlap())
	{
		const FApexDrsFlapSpec& Flap = DrsFlap.GetSpec();
		const FVector Hinge(0.0f, Flap.HingeForwardM * 100.0f, Flap.HingeUpM * 100.0f);
		for (int32 i = 0; i < Count && DrsFlapPart == INDEX_NONE; ++i)
		{
			if (Meshes[i] && DamagePartSpecs[i].Box().ExpandBy(5.0).IsInsideOrOn(Hinge))
			{
				DrsFlapPart = i;
			}
		}
	}
	// A car dressed anew starts from what the telemetry says, without
	// throwing off what is already gone.
	bDamageSeen = false;
	for (float& Share : DrawnDents.Zone)
	{
		Share = -1.0f;
	}
	ApplyDamageFrame();
	ApplyPartVisibility();
}

void AApexRaceCarActor::ForEachDamagePartComponent(TFunctionRef<void(UStaticMeshComponent&)> Fn) const
{
	for (UStaticMeshComponent* Part : DamagePartMeshes)
	{
		if (Part && Part->GetStaticMesh())
		{
			Fn(*Part);
		}
	}
}

void AApexRaceCarActor::ApplyPartVisibility()
{
	for (int32 i = 0; i < DamagePartMeshes.Num(); ++i)
	{
		if (UStaticMeshComponent* Part = DamagePartMeshes[i])
		{
			const bool bOn = DamagePartOn.IsValidIndex(i) && DamagePartOn[i];
			Part->SetVisibility(bBodyShown && bOn && Part->GetStaticMesh() != nullptr);
		}
	}
	const bool bFlapOff = DrsFlapPart != INDEX_NONE && DamagePartOn.IsValidIndex(DrsFlapPart) && !DamagePartOn[DrsFlapPart];
	DrsFlap.SetVisible(bBodyShown && !bFlapOff);
}

void AApexRaceCarActor::ApplyDamageFrame()
{
	const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	const FBoxSphereBounds Bounds = Mesh ? Mesh->GetBounds() : FBoxSphereBounds(FVector::ZeroVector, FVector(100.0), 100.0);
	const FVector Extent = Bounds.BoxExtent.ComponentMax(FVector(1.0));
	// Two cars of a model should not crumple alike.
	const float Seed = FMath::Frac(static_cast<float>(GetUniqueID()) * 0.6180339f);
	auto Write = [&](UStaticMeshComponent& Component) {
		Component.SetCustomPrimitiveDataVector4(ApexDamage::CpdCentre, FVector4(Bounds.Origin, 0.0));
		Component.SetCustomPrimitiveDataVector4(ApexDamage::CpdExtent, FVector4(Extent, 0.0));
		Component.SetCustomPrimitiveDataFloat(ApexDamage::CpdSeed, Seed);
	};
	Write(*CarMesh);
	ForEachDamagePartComponent(Write);
}

void AApexRaceCarActor::ApplyDents(const ApexDamage::FShares& Shares)
{
	auto Write = [&Shares](UStaticMeshComponent& Component) {
		Component.SetCustomPrimitiveDataFloat(ApexDamage::CpdFront, ApexDamage::Visual(Shares.Zone[ApexDamage::Front]));
		Component.SetCustomPrimitiveDataFloat(ApexDamage::CpdRear, ApexDamage::Visual(Shares.Zone[ApexDamage::Rear]));
		Component.SetCustomPrimitiveDataFloat(ApexDamage::CpdLeft, ApexDamage::Visual(Shares.Zone[ApexDamage::Left]));
		Component.SetCustomPrimitiveDataFloat(ApexDamage::CpdRight, ApexDamage::Visual(Shares.Zone[ApexDamage::Right]));
	};
	Write(*CarMesh);
	ForEachDamagePartComponent(Write);
	DrawnDents = Shares;
}

AApexCarEffectsActor* AApexRaceCarActor::GetEffects()
{
	if (!Effects.IsValid())
	{
		Effects = AApexCarEffectsActor::Get(GetWorld());
	}
	return Effects.Get();
}

FVector AApexRaceCarActor::ZonePoint(int32 Zone, FVector& OutNormal)
{
	const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	const FBoxSphereBounds Bounds = Mesh ? Mesh->GetBounds() : FBoxSphereBounds(FVector(0.0, 0.0, 60.0), FVector(90.0, 230.0, 60.0), 250.0);
	const FVector O = Bounds.Origin;
	const FVector E = Bounds.BoxExtent;
	FRandomStream& R = DamageRandom;
	const double Height = O.Z - E.Z + 2.0 * E.Z * R.FRandRange(0.12f, 0.5f);
	// The body mesh's frame: nose +Y, left +X, floor at Z = 0.
	FVector Local;
	FVector Normal;
	switch (Zone)
	{
	case ApexDamage::Front:
		Local = FVector(O.X + E.X * R.FRandRange(-0.6f, 0.6f), O.Y + E.Y * 0.97, Height);
		Normal = FVector(0.0, 1.0, 0.25);
		break;
	case ApexDamage::Rear:
		Local = FVector(O.X + E.X * R.FRandRange(-0.6f, 0.6f), O.Y - E.Y * 0.97, Height);
		Normal = FVector(0.0, -1.0, 0.25);
		break;
	case ApexDamage::Left:
		Local = FVector(O.X + E.X * 0.95, O.Y + E.Y * R.FRandRange(-0.6f, 0.6f), Height);
		Normal = FVector(1.0, 0.0, 0.25);
		break;
	case ApexDamage::Right:
		Local = FVector(O.X - E.X * 0.95, O.Y + E.Y * R.FRandRange(-0.6f, 0.6f), Height);
		Normal = FVector(-1.0, 0.0, 0.25);
		break;
	default:
		// The floor: a plank or a skid block on the ground.
		Local = FVector(O.X + E.X * R.FRandRange(-0.7f, 0.7f), O.Y + E.Y * R.FRandRange(-0.7f, 0.7f), O.Z - E.Z + 2.0);
		Normal = FVector(R.FRandRange(-0.5f, 0.5f), R.FRandRange(-0.5f, 0.5f), 0.2);
		break;
	}
	const FTransform& Frame = CarMesh->GetComponentTransform();
	OutNormal = Frame.TransformVectorNoScale(Normal).GetSafeNormal();
	return Frame.TransformPosition(Local);
}

void AApexRaceCarActor::EmitSparks(int32 Zone, int32 Count)
{
	AApexCarEffectsActor* Fx = GetEffects();
	if (!Fx || !Fx->CanDraw())
	{
		return;
	}
	const FVector CarVelocity = Root->ComponentVelocity;
	FRandomStream& R = DamageRandom;
	for (int32 i = 0; i < Count; ++i)
	{
		FVector Normal;
		ApexDamage::FPuff Spark;
		Spark.bSpark = true;
		Spark.Location = ZonePoint(Zone, Normal);
		const FVector Spray = (Normal + FVector(0.0, 0.0, 0.5) + R.GetUnitVector() * 0.8).GetSafeNormal();
		Spark.Velocity = CarVelocity * 0.85 + Spray * R.FRandRange(300.0f, 950.0f);
		Spark.Life = R.FRandRange(0.3f, 0.7f);
		// Thicker than a real one: at a chase camera's distance a true spark is under a pixel.
		Spark.StartSize = 2.5f;
		Spark.Glow = R.FRandRange(1500.0f, 4000.0f);
		Spark.Drag = 0.8f;
		Spark.Gravity = 1.0f;
		Fx->Emit(Spark);
	}
	if (Zone == INDEX_NONE)
	{
		return;
	}
	// A hit throws up a little dust and paint with its sparks.
	for (int32 i = 0; i < 3; ++i)
	{
		FVector Normal;
		ApexDamage::FPuff Dust;
		Dust.Location = ZonePoint(Zone, Normal);
		Dust.Velocity = CarVelocity * 0.5 + Normal * R.FRandRange(80.0f, 220.0f);
		Dust.Life = R.FRandRange(1.0f, 1.8f);
		Dust.StartSize = 15.0f;
		Dust.EndSize = R.FRandRange(70.0f, 120.0f);
		Dust.Shade = 0.55f;
		Dust.Opacity = 0.22f;
		Dust.Drag = 2.5f;
		Dust.Gravity = -0.02f;
		Fx->Emit(Dust);
	}
}

void AApexRaceCarActor::ThrowPart(int32 Index)
{
	UStaticMeshComponent* Part = DamagePartMeshes.IsValidIndex(Index) ? DamagePartMeshes[Index].Get() : nullptr;
	UWorld* World = GetWorld();
	AApexCarEffectsActor* Fx = GetEffects();
	if (!Part || !Part->GetStaticMesh() || !World || !Fx)
	{
		return;
	}
	const int32 Zone = ApexDamage::ZoneIndex(DamagePartSpecs[Index].Zone);
	FVector Normal;
	ZonePoint(Zone, Normal);
	FRandomStream& R = DamageRandom;
	auto Throw = [&](const UStaticMeshComponent& Source) {
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AApexCarDebrisActor* Debris =
			World->SpawnActor<AApexCarDebrisActor>(AApexCarDebrisActor::StaticClass(), Source.GetComponentTransform(), Params);
		if (!Debris)
		{
			return;
		}
		// Left behind by the car, knocked off the way the hit came from, and up.
		const FVector Velocity = Root->ComponentVelocity * R.FRandRange(0.75f, 0.92f) + Normal * R.FRandRange(250.0f, 600.0f)
			+ FVector(0.0, 0.0, R.FRandRange(250.0f, 550.0f));
		const FVector Spin = R.GetUnitVector() * R.FRandRange(3.0f, 9.0f);
		Debris->Launch(Source, Velocity, Spin, static_cast<float>(GetActorLocation().Z));
		Fx->AddDebris(Debris);
	};
	Throw(*Part);
	if (Index == DrsFlapPart)
	{
		if (const UStaticMeshComponent* Flap = DrsFlap.GetComponent(); Flap && Flap->GetStaticMesh())
		{
			Throw(*Flap);
		}
	}
	EmitSparks(Zone, 30);
	UE_LOG(LogApexSim, Log, TEXT("Car %d (%s) lost its %s"), CarIndex, *DisplayName, *DamagePartSpecs[Index].Name);
}

void AApexRaceCarActor::UpdateDamage(float DeltaSeconds)
{
	ApexDamage::FShares Shares = ApexDamage::FromPercent(TelemetryDamagePct);
	float Preview[ApexDamage::NumZones];
	if (ParseDamagePreview(CVarDamagePreview.GetValueOnGameThread(), Preview))
	{
		Shares = ApexDamage::FromPercent(Preview);
	}
	const ApexDamage::FShares Before = DamageShares;
	DamageShares = Shares;
	SinceLastHit += DeltaSeconds;

	// The dents: written only when a zone has moved, which is a hit or a repair.
	for (int32 Zone = 0; Zone < ApexDamage::Engine; ++Zone)
	{
		if (FMath::Abs(Shares.Zone[Zone] - DrawnDents.Zone[Zone]) > 0.0005f)
		{
			ApplyDents(Shares);
			break;
		}
	}

	const bool bEffects = bDamageSeen && bHasTarget && !IsHidden() && CVarDamageEffects.GetValueOnGameThread() != 0;

	// Parts off past their zone's threshold, back on after a repair.
	bool bPartsChanged = false;
	for (int32 i = 0; i < DamagePartSpecs.Num(); ++i)
	{
		const bool bOn = ApexDamage::IsAttached(DamagePartSpecs[i], Shares);
		if (bOn != DamagePartOn[i])
		{
			DamagePartOn[i] = bOn;
			bPartsChanged = true;
			if (!bOn && bEffects)
			{
				ThrowPart(i);
			}
		}
	}
	if (bPartsChanged)
	{
		ApplyPartVisibility();
	}

	// A zone that jumped took a hit: sparks off its face.
	if (bDamageSeen)
	{
		for (int32 Zone = 0; Zone < ApexDamage::Engine; ++Zone)
		{
			const float Grown = (Shares.Zone[Zone] - Before.Zone[Zone]) * 100.0f;
			if (Grown >= ApexDamage::HitPercent)
			{
				LastHitZone = Zone;
				SinceLastHit = 0.0f;
				if (bEffects)
				{
					EmitSparks(Zone, 10 + FMath::Min(FMath::RoundToInt(Grown * 4.0f), 60));
				}
			}
		}
	}
	bDamageSeen = true;

	AApexCarEffectsActor* Fx = bEffects ? GetEffects() : nullptr;
	if (!Fx || !Fx->CanDraw() || DeltaSeconds <= 0.0f)
	{
		SmokeOwed = SteamOwed = ScrapeOwed = 0.0f;
		return;
	}
	const UStaticMesh* Mesh = CarMesh->GetStaticMesh();
	if (!Mesh)
	{
		return;
	}
	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const FVector O = Bounds.Origin;
	const FVector E = Bounds.BoxExtent;
	const FTransform& Frame = CarMesh->GetComponentTransform();
	const FVector CarVelocity = Root->ComponentVelocity;
	FRandomStream& R = DamageRandom;

	// Oil smoke out of the back of the car: the engine bay and the exhaust.
	SmokeOwed += ApexDamage::EngineSmokeRate(Shares) * DeltaSeconds;
	const float Shade = ApexDamage::EngineSmokeShade(Shares);
	const bool bOut = Shares.Zone[ApexDamage::Engine] >= 1.0f;
	for (; SmokeOwed >= 1.0f; SmokeOwed -= 1.0f)
	{
		ApexDamage::FPuff Puff;
		Puff.Location = Frame.TransformPosition(
			FVector(O.X + E.X * R.FRandRange(-0.15f, 0.15f), O.Y - E.Y * 0.88, O.Z - E.Z + 2.0 * E.Z * R.FRandRange(0.4f, 0.65f)));
		Puff.Velocity = CarVelocity * 0.25 + FVector(0.0, 0.0, 70.0) + R.GetUnitVector() * 40.0;
		Puff.Life = 2.8f * R.FRandRange(0.8f, 1.2f);
		Puff.StartSize = 20.0f;
		Puff.EndSize = R.FRandRange(130.0f, 190.0f);
		Puff.Shade = FMath::Clamp(Shade + R.FRandRange(-0.05f, 0.05f), 0.0f, 1.0f);
		Puff.Opacity = bOut ? 0.7f : 0.5f;
		Puff.Drag = 1.6f;
		Puff.Gravity = -0.04f;
		Fx->Emit(Puff);
	}

	// Steam out of a holed radiator in a damaged nose.
	SteamOwed += ApexDamage::SteamRate(Shares, WaterTempC) * DeltaSeconds;
	for (; SteamOwed >= 1.0f; SteamOwed -= 1.0f)
	{
		ApexDamage::FPuff Puff;
		Puff.Location = Frame.TransformPosition(
			FVector(O.X + E.X * R.FRandRange(-0.3f, 0.3f), O.Y + E.Y * 0.7, O.Z - E.Z + 2.0 * E.Z * R.FRandRange(0.5f, 0.7f)));
		Puff.Velocity = CarVelocity * 0.3 + FVector(0.0, 0.0, 120.0) + R.GetUnitVector() * 50.0;
		Puff.Life = R.FRandRange(0.8f, 1.2f);
		Puff.StartSize = 10.0f;
		Puff.EndSize = R.FRandRange(55.0f, 85.0f);
		Puff.Shade = 0.93f;
		Puff.Opacity = 0.35f;
		Puff.Drag = 2.2f;
		Puff.Gravity = -0.08f;
		Fx->Emit(Puff);
	}

	// Scraping along a wall or another car: sparks off the side that last
	// took a hit, else off the floor.
	ScrapeOwed += bColliding ? ApexDamage::ScrapeSparkRate(SpeedMps) * DeltaSeconds : 0.0f;
	const int32 ScrapeZone = SinceLastHit < 3.0f ? LastHitZone : INDEX_NONE;
	const int32 Scrape = FMath::FloorToInt(ScrapeOwed);
	if (Scrape > 0)
	{
		ScrapeOwed -= Scrape;
		EmitSparks(ScrapeZone, Scrape);
	}
}
