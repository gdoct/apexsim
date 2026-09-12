#include "Race/ApexRacingLineActor.h"

#include "ApexSim.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Race/ApexRaceCoordinate.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** Distance between dots along the line, metres. */
	constexpr double DotSpacingM = 3.0;

	/**
	 * Dot size, centimetres. Longer than wide on purpose: seen from a driver's
	 * eye a metre off the road, a dot a few car lengths ahead is foreshortened
	 * to a quarter of its length, and a round one reads as a dash across the
	 * line rather than a dot on it.
	 */
	constexpr double DotLengthCm = 70.0;
	constexpr double DotWidthCm = 45.0;
	constexpr double DotThicknessCm = 1.0;

	/**
	 * Clearance between the road and the underside of a dot. The baked paint —
	 * start line, grid boxes, edge lines — floats 4 to 4.5 cm over the tarmac
	 * (`MARKING_LIFT_M` in the track editor), and a dot any lower is cut in half
	 * where the line crosses it.
	 */
	constexpr double DotLiftCm = 5.0;

	/** Beyond this the dots are culled: sub-pixel shimmer, and clutter over a crest. */
	constexpr int32 DrawDistanceCm = 40000;

	/**
	 * Vertical reach of the trace that drops a dot onto the road, around the
	 * height the line itself gives. Short enough upward that a bridge over the
	 * circuit (Suzuka's crossover) is not mistaken for the road beneath it.
	 */
	constexpr double TraceUpCm = 200.0;
	constexpr double TraceDownCm = 400.0;

	/**
	 * Paint colours, as albedo for the lit shape material. Bright against
	 * asphalt at a third of their value, and the three far enough apart in hue
	 * to tell at a glance.
	 */
	const FLinearColor ThrottleColor(0.03f, 0.60f, 0.06f);
	const FLinearColor PartialColor(0.90f, 0.50f, 0.00f);
	const FLinearColor BrakeColor(0.80f, 0.02f, 0.01f);

	/** The engine cylinder is 100 cm across and 100 cm tall, pivot at its centre. */
	constexpr double CylinderSizeCm = 100.0;

	/**
	 * Whether a trace hit is the road a dot belongs on: one of the track level's
	 * own baked meshes. Props are generated as `SM_Prop_*` meshes or instanced
	 * components (trees, walls), or authored kit actors tagged `ApexProp` (a
	 * bridge deck over the road, a garage), and a dot on top of a tyre wall
	 * is worse than one floating a few centimetres off the tarmac.
	 */
	bool IsRoadSurface(const FHitResult& Hit, const ULevel* Ground)
	{
		static const FName PropTag(TEXT("ApexProp"));
		const UPrimitiveComponent* Component = Hit.GetComponent();
		const AActor* Actor = Hit.GetActor();
		if (!Component || !Actor || Actor->GetLevel() != Ground || Component->IsA<UInstancedStaticMeshComponent>()
			|| Actor->ActorHasTag(PropTag))
		{
			return false;
		}
		if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
			return Mesh && !Mesh->GetName().StartsWith(TEXT("SM_Prop_"));
		}
		return false;
	}
}

AApexRacingLineActor::AApexRacingLineActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	// Engine primitives, referenced from the class default object so the
	// cooker carries them into a packaged build (see ApexCockpitRig).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderFinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	DotMesh = CylinderFinder.Object;
	ShapeMaterial = MaterialFinder.Object;

	ThrottleDots = MakeDots(TEXT("ThrottleDots"));
	PartialDots = MakeDots(TEXT("PartialDots"));
	BrakeDots = MakeDots(TEXT("BrakeDots"));
}

UInstancedStaticMeshComponent* AApexRacingLineActor::MakeDots(const TCHAR* Name)
{
	UInstancedStaticMeshComponent* Dots = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
	Dots->SetupAttachment(Root);
	Dots->SetStaticMesh(DotMesh);
	Dots->SetMobility(EComponentMobility::Movable);
	Dots->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Dots->SetGenerateOverlapEvents(false);
	Dots->SetCanEverAffectNavigation(false);
	// Paint on the road: it casts nothing, and a car's shadow falls across it.
	Dots->SetCastShadow(false);
	Dots->bReceivesDecals = false;
	Dots->SetCullDistances(0, DrawDistanceCm);
	return Dots;
}

void AApexRacingLineActor::BeginPlay()
{
	Super::BeginPlay();

	// Instances are made here rather than in the constructor, which is too
	// early for dynamic materials to be safe.
	Paint(ThrottleDots, ThrottleColor);
	Paint(PartialDots, PartialColor);
	Paint(BrakeDots, BrakeColor);
	ApplyVisibility();
}

void AApexRacingLineActor::Paint(UInstancedStaticMeshComponent* Dots, const FLinearColor& Color)
{
	if (!Dots || !ShapeMaterial)
	{
		return;
	}
	if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(ShapeMaterial, this))
	{
		Mid->SetVectorParameterValue(TEXT("Color"), Color);
		Dots->SetMaterial(0, Mid);
	}
}

void AApexRacingLineActor::SetLine(const FApexRacingLineData& InLine)
{
	Line = InLine;
	bOnGround = false;
	Rebuild(nullptr);
}

void AApexRacingLineActor::SnapToGround(const ULevel* TrackLevel)
{
	if (!TrackLevel || !HasLine())
	{
		return;
	}
	Rebuild(TrackLevel);
	bOnGround = true;
}

void AApexRacingLineActor::SetMode(EApexRacingLine InMode)
{
	Mode = InMode;
	ApplyVisibility();
}

void AApexRacingLineActor::ApplyVisibility()
{
	const bool bAny = Mode != EApexRacingLine::Off;
	const bool bFull = Mode == EApexRacingLine::Full;
	ThrottleDots->SetVisibility(bFull);
	PartialDots->SetVisibility(bFull);
	BrakeDots->SetVisibility(bAny);
}

void AApexRacingLineActor::Rebuild(const ULevel* Ground)
{
	ThrottleDots->ClearInstances();
	PartialDots->ClearInstances();
	BrakeDots->ClearInstances();

	UWorld* World = GetWorld();
	const int32 PointCount = Line.Points.Num();
	if (!World || !Line.IsValid())
	{
		return;
	}

	// Lay dots at an even spacing of their own rather than one per server
	// point, so the look does not depend on how finely the server sampled.
	TArray<double> SegmentLength;
	SegmentLength.SetNumUninitialized(PointCount);
	double Total = 0.0;
	for (int32 i = 0; i < PointCount; ++i)
	{
		SegmentLength[i] = FVector::Dist(Line.Points[i], Line.Points[(i + 1) % PointCount]);
		Total += SegmentLength[i];
	}
	const int32 DotCount = FMath::Max(3, FMath::RoundToInt(Total / DotSpacingM));
	const double Step = Total / DotCount;

	TArray<FTransform> Throttle, Partial, Brake;
	const FVector Scale(DotLengthCm / CylinderSizeCm, DotWidthCm / CylinderSizeCm, DotThicknessCm / CylinderSizeCm);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexRacingLine), /*bTraceComplex*/ true, this);
	const FCollisionObjectQueryParams GroundObjects(ECC_WorldStatic);
	TArray<FHitResult> Hits;
	int32 Grounded = 0;

	int32 Segment = 0;
	double SegmentStart = 0.0;
	for (int32 Dot = 0; Dot < DotCount; ++Dot)
	{
		const double Along = Dot * Step;
		while (Segment < PointCount - 1 && Along > SegmentStart + SegmentLength[Segment])
		{
			SegmentStart += SegmentLength[Segment];
			++Segment;
		}
		const int32 Next = (Segment + 1) % PointCount;
		const double T = FMath::Clamp((Along - SegmentStart) / FMath::Max(SegmentLength[Segment], 1e-3), 0.0, 1.0);

		const FVector From = ApexRace::ServerToUnrealPosition(Line.Points[Segment]);
		const FVector To = ApexRace::ServerToUnrealPosition(Line.Points[Next]);
		FVector Location = FMath::Lerp(From, To, T);
		FVector Normal = FVector::UpVector;

		if (Ground)
		{
			Hits.Reset();
			World->LineTraceMultiByObjectType(Hits, Location + FVector(0.0, 0.0, TraceUpCm),
				Location - FVector(0.0, 0.0, TraceDownCm), GroundObjects, Params);
			Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
			for (const FHitResult& Hit : Hits)
			{
				if (IsRoadSurface(Hit, Ground))
				{
					Location = Hit.ImpactPoint;
					Normal = Hit.ImpactNormal;
					++Grounded;
					break;
				}
			}
		}

		// Flat on the surface, long axis along the line.
		const FRotator Rotation = FRotationMatrix::MakeFromZX(Normal, To - From).Rotator();
		const FVector Centre = Location + Normal * (DotThicknessCm * 0.5 + DotLiftCm);
		const FTransform Transform(Rotation, Centre, Scale);

		// The phase of whichever server point the dot is nearer.
		switch (Line.Phases[T < 0.5 ? Segment : Next])
		{
		case EApexLinePhase::Throttle: Throttle.Add(Transform); break;
		case EApexLinePhase::Brake:    Brake.Add(Transform);    break;
		default:                       Partial.Add(Transform);  break;
		}
	}

	ThrottleDots->AddInstances(Throttle, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	PartialDots->AddInstances(Partial, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	BrakeDots->AddInstances(Brake, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);

	if (Ground)
	{
		UE_LOG(LogApexSim, Log, TEXT("Racing line: %d dot(s) (%d throttle, %d partial, %d brake), %d on the road"),
			DotCount, Throttle.Num(), Partial.Num(), Brake.Num(), Grounded);
		if (Grounded < DotCount / 2)
		{
			UE_LOG(LogApexSim, Warning,
				TEXT("Racing line: only %d of %d dots found the road; the rest sit at the track file's heights. "
					 "Does the track level have collision?"),
				Grounded, DotCount);
		}
	}
}
