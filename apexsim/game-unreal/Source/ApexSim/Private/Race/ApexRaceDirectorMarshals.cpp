// The race director's marshals and recovery vehicle at a stopped car
// (docs/game/cameras.md, "Marshals and recovery"): the scenes are
// ApexMarshals::FTracker's, drawn here with a small pool of kit props.
// The rest of the director is in ApexRaceDirector.cpp.

#include "Race/ApexRaceDirector.h"

#include "ApexSim.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Race/ApexRaceCarActor.h"
#include "Race/ApexRaceCoordinate.h"
#include "Track/ApexPropLibrary.h"
#include "Track/ApexTrackSceneBuilder.h"

namespace ApexMarshalDraw
{
	TAutoConsoleVariable<int32> CVarMarshals(
		TEXT("apexsim.marshals"),
		1,
		TEXT("Marshals waving yellow and a recovery tractor at a car stopped off the road or out of the race (0 off)"),
		ECVF_Default);

	/** The kit's people and vehicles; a primitive stands in when the kit is missing. */
	const TCHAR* const PropsRoot = TEXT("/Game/Props");
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const TCHAR* const CylinderPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	const TCHAR* const ShapeMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	const FLinearColor FlagYellow(1.0f, 0.78f, 0.0f);
	const FLinearColor Stick(0.08f, 0.08f, 0.09f);
	const FLinearColor Overalls(1.0f, 0.35f, 0.02f);
	const FLinearColor Beacon(1.0f, 0.45f, 0.0f);

	/** The flags swing this often, this far either way of upright. */
	constexpr float WaveHz = 1.4f;
	constexpr float WaveDeg = 38.0f;
	/** The two marshals stand side by side across the verge. */
	constexpr double FigureSpacingCm = 150.0;
	/**
	 * The raised hand that holds the waved flag's stick: beside the figure,
	 * a little over its shoulder. The stick rises StickCm from it and the
	 * cloth hangs off the stick's top half, out to the side (the hand's +X),
	 * so the two swing as one. A bare card at the hand read as a yellow
	 * square floating by the marshal.
	 */
	const FVector FlagPivot(30.0, 0.0, 150.0);
	constexpr double StickCm = 90.0;
	/** The cloth's centre from the hand, and its size (cm; the cube is 100 cm). */
	const FVector FlagCentre(30.0, 0.0, 68.0);
	const FVector FlagScale(0.6, 0.01, 0.4);
	/** The stick: the engine cylinder (100 cm tall, 100 across, centred) a 2.5 cm pole. */
	const FVector StickScale(0.025, 0.025, StickCm / 100.0);
	/** The tractor's roof beacon, flashing at 2 Hz. */
	constexpr float BeaconHz = 2.0f;
	const FVector BeaconAt(-20.0, 0.0, 300.0);

	UStaticMesh* LoadMesh(const FString& Kind, const FString& Asset)
	{
		return LoadObject<UStaticMesh>(nullptr, *ApexProps::MeshObjectPath(PropsRoot, Kind, Asset), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UMaterialInstanceDynamic* Paint(UObject* Outer, const FLinearColor& Colour)
	{
		UMaterialInterface* Shape = LoadObject<UMaterialInterface>(nullptr, ShapeMaterialPath);
		if (!Shape)
		{
			return nullptr;
		}
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Shape, Outer);
		Mid->SetVectorParameterValue(TEXT("Color"), Colour);
		return Mid;
	}

	FAutoConsoleCommandWithWorld LookCommand(
		TEXT("apexsim.marshals.Look"),
		TEXT("Park the shot camera on the newest marshal scene, up the road from the stopped car (apexsim.cam.Release hands it back)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
			{
				AApexRaceDirector* Director = AApexRaceDirector::Find(World);
				if (!Director || !Director->LookAtMarshals())
				{
					UE_LOG(LogApexSim, Warning, TEXT("apexsim.marshals.Look: no marshals out"));
				}
			}));

	/** The first ground below a point that is not a prop; the point's own height when there is none. */
	double GroundZ(UWorld* World, const FVector& At, const AActor* Ignore)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexMarshalGround), /*bTraceComplex*/ true, Ignore);
		TArray<FHitResult> Hits;
		World->LineTraceMultiByObjectType(Hits, At + FVector(0.0, 0.0, 500.0), At - FVector(0.0, 0.0, 2500.0),
			FCollisionObjectQueryParams(ECC_WorldStatic), Params);
		Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
		for (const FHitResult& Hit : Hits)
		{
			const AActor* Actor = Hit.GetActor();
			if (!Actor || !Actor->ActorHasTag(FApexTrackSceneBuilder::PropTag))
			{
				return Hit.ImpactPoint.Z;
			}
		}
		return At.Z;
	}
}

void AApexRaceDirector::SampleMarshalCars(const FApexTelemetryFrame& Frame)
{
	bMarshalRacing = Frame.SessionState == EApexSessionState::Racing;
	MarshalCars.Reset(Frame.Cars.Num());
	for (const FApexCarTelemetry& Car : Frame.Cars)
	{
		const AApexRaceCarActor* Actor = FindCar(Car.CarIndex);
		ApexMarshals::FCarSample Sample;
		Sample.CarIndex = Car.CarIndex;
		// Where the car is drawn, which is where the marshals should be.
		Sample.Location = Actor ? Actor->GetActorLocation() : ApexRace::ServerToUnrealPosition(Car.Position);
		Sample.YawDeg = Actor ? static_cast<float>(Actor->GetActorRotation().Yaw) : 0.0f;
		Sample.SpeedMps = Car.SpeedMps;
		Sample.bOnTrack = Car.bIsOnTrack;
		Sample.bInPits = Car.bInPitLane || Car.bInGarage;
		for (const float Damage : Car.DamagePct)
		{
			Sample.bOut |= Damage >= 99.5f;
		}
		MarshalCars.Add(Sample);
	}
}

void AApexRaceDirector::UpdateMarshals(float DeltaSeconds)
{
	using namespace ApexMarshalDraw;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const bool bOn = CVarMarshals.GetValueOnGameThread() != 0;
	const int32 Before = MarshalTracker.GetIncidents().Num();
	MarshalTracker.Tick(MarshalCars, bOn && bMarshalRacing, DeltaSeconds, Tv.GetPath().IsValid() ? &Tv.GetPath() : nullptr);
	const TArray<ApexMarshals::FIncident>& Scenes = MarshalTracker.GetIncidents();
	if (Scenes.Num() > Before)
	{
		const ApexMarshals::FIncident& Newest = Scenes.Last();
		UE_LOG(LogApexSim, Log, TEXT("Marshals out for car %d at (%.0f, %.0f, %.0f) cm"), Newest.CarIndex,
			Newest.CarLocation.X, Newest.CarLocation.Y, Newest.CarLocation.Z);
	}
	if (Scenes.IsEmpty() && MarshalSlots.IsEmpty())
	{
		return;
	}
	MarshalClock += DeltaSeconds;

	// The pool grows to the scenes there have been, never past MaxIncidents.
	while (MarshalSlots.Num() < Scenes.Num())
	{
		UStaticMesh* Figure = LoadMesh(TEXT("misc"), TEXT("marshal_figure"));
		UStaticMesh* Tractor = LoadMesh(TEXT("vehicle"), TEXT("tractor"));
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
		UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, CylinderPath);
		const int32 Index = MarshalSlots.Num();
		auto Make = [this, Index](const TCHAR* Part, UStaticMesh* Mesh, const FLinearColor* Colour)
		{
			UStaticMeshComponent* Component =
				NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("Marshal%d_%s"), Index, Part));
			Component->SetUsingAbsoluteLocation(true);
			Component->SetUsingAbsoluteRotation(true);
			Component->SetUsingAbsoluteScale(true);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(Mesh);
			if (Colour)
			{
				Component->SetMaterial(0, Paint(Component, *Colour));
			}
			Component->SetupAttachment(GetRootComponent());
			Component->RegisterComponent();
			Component->SetVisibility(false);
			return Component;
		};
		FMarshalSlot Slot;
		for (int32 Person = 0; Person < 2; ++Person)
		{
			// A marshal in orange overalls when the kit has none.
			Slot.Figures[Person] = Figure ? Make(*FString::Printf(TEXT("Figure%d"), Person), Figure, nullptr)
										  : Make(*FString::Printf(TEXT("Figure%d"), Person), Cube, &Overalls);
			if (!Figure)
			{
				Slot.Figures[Person]->SetWorldScale3D(FVector(0.5, 0.3, 1.8));
			}
			Slot.Flags[Person] = Make(*FString::Printf(TEXT("Flag%d"), Person), Cube, &FlagYellow);
			Slot.Flags[Person]->SetCastShadow(false);
			Slot.Flags[Person]->SetWorldScale3D(FlagScale);
			Slot.Sticks[Person] = Make(*FString::Printf(TEXT("Stick%d"), Person), Cylinder ? Cylinder : Cube, &Stick);
			Slot.Sticks[Person]->SetCastShadow(false);
			Slot.Sticks[Person]->SetWorldScale3D(StickScale);
		}
		Slot.Truck = Tractor ? Make(TEXT("Truck"), Tractor, nullptr) : Make(TEXT("Truck"), Cube, &Overalls);
		if (!Tractor)
		{
			Slot.Truck->SetWorldScale3D(FVector(4.5, 2.3, 2.6));
		}
		Slot.Beacon = Make(TEXT("Beacon"), Cube, &Beacon);
		Slot.Beacon->SetCastShadow(false);
		Slot.Beacon->SetWorldScale3D(FVector(0.25, 0.25, 0.18));
		MarshalSlots.Add(Slot);
		UE_LOG(LogApexSim, Log, TEXT("Marshal scene %d made (%s, %s)"), Index,
			Figure ? TEXT("kit marshals") : TEXT("stand-in marshals"), Tractor ? TEXT("kit tractor") : TEXT("stand-in tractor"));
	}

	for (int32 Index = 0; Index < MarshalSlots.Num(); ++Index)
	{
		FMarshalSlot& Slot = MarshalSlots[Index];
		const ApexMarshals::FIncident* Scene = Scenes.IsValidIndex(Index) ? &Scenes[Index] : nullptr;
		if (!Scene)
		{
			for (int32 Person = 0; Person < 2; ++Person)
			{
				Slot.Figures[Person]->SetVisibility(false);
				Slot.Flags[Person]->SetVisibility(false);
				Slot.Sticks[Person]->SetVisibility(false);
			}
			Slot.Truck->SetVisibility(false);
			Slot.Beacon->SetVisibility(false);
			continue;
		}

		// The marshals face the cars coming up the road; the card's face is
		// the kit's local +Y, so its yaw is a quarter turn short of theirs.
		const double FacingYaw = (-Scene->Along).Rotation().Yaw;
		const FRotator FigureRotation(0.0, FacingYaw - 90.0, 0.0);
		for (int32 Person = 0; Person < 2; ++Person)
		{
			FVector At = Scene->Post + Scene->Outward * (Person == 0 ? -0.4 : 0.6) * FigureSpacingCm;
			At.Z = GroundZ(World, At, this);
			Slot.Figures[Person]->SetWorldLocationAndRotation(At, FigureRotation);
			Slot.Figures[Person]->SetVisibility(true);

			// The flag swings in the plane the oncoming drivers see.
			const float Swing = WaveDeg * FMath::Sin(2.0f * PI * WaveHz * MarshalClock + Index * 1.3f + Person * 2.1f);
			const FQuat Hand = FigureRotation.Quaternion() * FRotator(Swing, 0.0f, 0.0f).Quaternion();
			const FVector Pivot = At + FigureRotation.RotateVector(FlagPivot);
			Slot.Flags[Person]->SetWorldLocationAndRotation(Pivot + Hand.RotateVector(FlagCentre), Hand);
			Slot.Flags[Person]->SetVisibility(Scene->IsWaving());
			Slot.Sticks[Person]->SetWorldLocationAndRotation(Pivot + Hand.RotateVector(FVector(0.0, 0.0, 0.5 * StickCm)), Hand);
			Slot.Sticks[Person]->SetVisibility(Scene->IsWaving());
		}

		const bool bTruck = Scene->HasTruck();
		Slot.Truck->SetVisibility(bTruck);
		if (bTruck)
		{
			FVector At = Scene->TruckLocation();
			At.Z = GroundZ(World, At, this);
			const FRotator TruckRotation(0.0, Scene->TruckYawDeg(), 0.0);
			Slot.Truck->SetWorldLocationAndRotation(At, TruckRotation);
			Slot.Beacon->SetWorldLocationAndRotation(At + TruckRotation.RotateVector(BeaconAt), TruckRotation);
		}
		Slot.Beacon->SetVisibility(bTruck && FMath::Frac(MarshalClock * BeaconHz) < 0.5f);
	}
}

void AApexRaceDirector::ForgetMarshals()
{
	MarshalTracker.Reset();
	MarshalCars.Reset();
	bMarshalRacing = false;
	for (FMarshalSlot& Slot : MarshalSlots)
	{
		for (int32 Person = 0; Person < 2; ++Person)
		{
			Slot.Figures[Person]->SetVisibility(false);
			Slot.Flags[Person]->SetVisibility(false);
			Slot.Sticks[Person]->SetVisibility(false);
		}
		Slot.Truck->SetVisibility(false);
		Slot.Beacon->SetVisibility(false);
	}
}

bool AApexRaceDirector::LookAtMarshals()
{
	const TArray<ApexMarshals::FIncident>& Scenes = MarshalTracker.GetIncidents();
	if (Scenes.IsEmpty())
	{
		return false;
	}
	// The newest, from the first of these with a clear line to the marshals
	// and to the car (a wall or a catch fence can run between them and any
	// one pose): up the road along the marshals' own line,
	// 25 m then 15 m back; over the track 25 m up the road; across the road
	// from the car, low; and high above the scene. With none clear, the one
	// whose two lines run furthest before they hit: a fence just short of the
	// post hides less than a stand roof over the camera. The post, the car
	// and the tractor's way in are in frame.
	const ApexMarshals::FIncident& Scene = Scenes.Last();
	const FVector Up(0.0, 0.0, 1.0);
	const FVector Subject = Scene.Post + Up * 150.0;
	const FVector Car = Scene.CarLocation + Up * 80.0;
	const FVector Candidates[] = {
		Scene.Post - Scene.Along * 2500.0 + Up * 400.0,
		Scene.Post - Scene.Along * 1500.0 + Up * 300.0,
		Scene.CarLocation - Scene.Along * 2500.0 - Scene.Outward * 300.0 + Up * 600.0,
		Scene.CarLocation - Scene.Along * 1000.0 - Scene.Outward * 1400.0 + Up * 250.0,
		Scene.CarLocation - Scene.Along * 1200.0 - Scene.Outward * 400.0 + Up * 2500.0,
	};
	FVector Eye = Candidates[2];
	if (UWorld* World = GetWorld())
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexMarshalLook), /*bTraceComplex*/ true, this);
		// The share of the line that is clear. A hit within 3 m of the target
		// is the target itself (the car's own body, the tractor beside it),
		// not something in the way.
		auto ClearShare = [&](const FVector& From, const FVector& To)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params)
				|| FVector::Dist(Hit.ImpactPoint, To) < 300.0)
			{
				return 1.0;
			}
			return FVector::Dist(From, Hit.ImpactPoint) / FMath::Max(FVector::Dist(From, To), 1.0);
		};
		double Best = -1.0;
		for (const FVector& Candidate : Candidates)
		{
			const double Share = ClearShare(Candidate, Subject) + ClearShare(Candidate, Car);
			if (Share >= 2.0)
			{
				Eye = Candidate;
				break;
			}
			if (Share > Best)
			{
				Best = Share;
				Eye = Candidate;
			}
		}
	}
	SetShotCameraPose(Eye, (FMath::Lerp(Car, Subject, 0.5) - Eye).Rotation());
	return true;
}
