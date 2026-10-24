#include "Race/ApexSchoolMarkerActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "UObject/ConstructorHelpers.h"

namespace ApexSchoolMarks
{
	/** Across the road, metres: wider than most circuits' tarmac, so the mark reads from either edge. */
	constexpr double StripWidthM = 16.0;
	constexpr double StripThicknessCm = 1.0;
	/** Over the baked paint, which floats 4 to 4.5 cm over the tarmac (the racing line's dots sit the same). */
	constexpr double StripLiftCm = 5.0;
	constexpr double TraceUpCm = 200.0;
	constexpr double TraceDownCm = 400.0;
	/** The engine cube is 100 cm a side, pivot at its centre. */
	constexpr double CubeSizeCm = 100.0;

	const FLinearColor BoardColor(0.95f, 0.75f, 0.02f);
	const FLinearColor LineColor(0.90f, 0.90f, 0.90f);
	const FLinearColor BoxColor(0.03f, 0.60f, 0.06f);

	/** One of the track's own baked surfaces (as the racing line's dots land on). */
	bool IsRoad(const FHitResult& Hit)
	{
		const UPrimitiveComponent* Component = Hit.GetComponent();
		const AActor* Actor = Hit.GetActor();
		return Component && Actor && !Component->IsA<UInstancedStaticMeshComponent>()
			&& !Actor->ActorHasTag(FApexTrackSceneBuilder::PropTag)
			&& Actor->ActorHasTag(FApexTrackSceneBuilder::TrackMeshTag);
	}
}

AApexSchoolMarkerActor::AApexSchoolMarkerActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(
		TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	StripMesh = CubeFinder.Object;
	ShapeMaterial = MaterialFinder.Object;

	BoardStrips = MakeStrips(TEXT("BoardStrips"));
	LineStrips = MakeStrips(TEXT("LineStrips"));
	BoxStrips = MakeStrips(TEXT("BoxStrips"));
}

UInstancedStaticMeshComponent* AApexSchoolMarkerActor::MakeStrips(const TCHAR* Name)
{
	UInstancedStaticMeshComponent* Strips = CreateDefaultSubobject<UInstancedStaticMeshComponent>(Name);
	Strips->SetupAttachment(Root);
	Strips->SetStaticMesh(StripMesh);
	Strips->SetMobility(EComponentMobility::Movable);
	Strips->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Strips->SetGenerateOverlapEvents(false);
	Strips->SetCanEverAffectNavigation(false);
	Strips->SetCastShadow(false);
	Strips->bReceivesDecals = false;
	return Strips;
}

void AApexSchoolMarkerActor::BeginPlay()
{
	Super::BeginPlay();
	Paint(BoardStrips, ApexSchoolMarks::BoardColor);
	Paint(LineStrips, ApexSchoolMarks::LineColor);
	Paint(BoxStrips, ApexSchoolMarks::BoxColor);
}

void AApexSchoolMarkerActor::Paint(UInstancedStaticMeshComponent* Strips, const FLinearColor& Color)
{
	if (!Strips || !ShapeMaterial)
	{
		return;
	}
	if (UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(ShapeMaterial, this))
	{
		Mid->SetVectorParameterValue(TEXT("Color"), Color);
		Strips->SetMaterial(0, Mid);
	}
}

void AApexSchoolMarkerActor::AddStrip(UInstancedStaticMeshComponent* Strips, const TArray<FVector>& CenterlineCm, float StationM, float LengthM)
{
	namespace M = ApexSchoolMarks;
	FVector At;
	FVector Forward;
	if (!Strips || !ApexSchool::PoseAtStation(CenterlineCm, StationM, At, Forward) || Forward.IsNearlyZero())
	{
		return;
	}
	// Onto the road under the centerline point.
	if (const UWorld* World = GetWorld())
	{
		TArray<FHitResult> Hits;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(ApexSchoolMarks), true);
		Params.AddIgnoredActor(this);
		if (World->LineTraceMultiByChannel(Hits, At + FVector(0.0, 0.0, M::TraceUpCm), At - FVector(0.0, 0.0, M::TraceDownCm), ECC_Visibility, Params))
		{
			for (const FHitResult& Hit : Hits)
			{
				if (M::IsRoad(Hit))
				{
					At.Z = Hit.ImpactPoint.Z;
					break;
				}
			}
		}
	}
	const FRotator Rotation = Forward.Rotation();
	const FVector Scale(LengthM * 100.0 / M::CubeSizeCm, M::StripWidthM * 100.0 / M::CubeSizeCm, M::StripThicknessCm / M::CubeSizeCm);
	Strips->AddInstance(FTransform(Rotation, At + FVector(0.0, 0.0, M::StripLiftCm + M::StripThicknessCm * 0.5), Scale), true);
}

void AApexSchoolMarkerActor::SetMarks(const ApexSchool::FTest& Test, const TArray<FVector>& CenterlineCm)
{
	BoardStrips->ClearInstances();
	LineStrips->ClearInstances();
	BoxStrips->ClearInstances();
	switch (Test.Kind)
	{
	case EApexSchoolKind::Brake:
		AddStrip(BoardStrips, CenterlineCm, Test.FromM, 1.5f);
		AddStrip(LineStrips, CenterlineCm, Test.ToM, 0.5f);
		// The gold box: its two edges.
		AddStrip(BoxStrips, CenterlineCm, Test.ToM - Test.Limits[2], 0.3f);
		AddStrip(BoxStrips, CenterlineCm, Test.ToM + Test.Limits[2], 0.3f);
		break;
	case EApexSchoolKind::CornerExit:
	case EApexSchoolKind::CornerTime:
		AddStrip(LineStrips, CenterlineCm, Test.FromM, 0.5f);
		AddStrip(LineStrips, CenterlineCm, Test.ToM, 0.5f);
		break;
	case EApexSchoolKind::Lap:
	default:
		// The start line is on the road already.
		break;
	}
}
