#include "Race/ApexCloudLayer.h"

#include "ApexSim.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Track/ApexTrackSceneBuilder.h"
#include "UObject/ConstructorHelpers.h"

AApexCloudLayerActor::AApexCloudLayerActor()
{
	PrimaryActorTick.bCanEverTick = true;
	// After the cameras have moved, so the dome is centred on this frame's view.
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	Dome = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Dome"));
	SetRootComponent(Dome);
	Dome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Dome->SetCastShadow(false);
	Dome->bAffectDistanceFieldLighting = false;
	Dome->bAffectDynamicIndirectLighting = false;
	Dome->SetMobility(EComponentMobility::Movable);
	// The sphere is 100 cm across; the dome is its radius scaled.
	Dome->SetRelativeScale3D(FVector(RadiusCm / 50.0f));
	Dome->bTreatAsBackgroundForOcclusion = true;
	Dome->SetTranslucentSortPriority(-100);
	Dome->SetVisibility(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereFinder(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereFinder.Succeeded())
	{
		Dome->SetStaticMesh(SphereFinder.Object);
	}
}

void AApexCloudLayerActor::SetSky(float Cover, const FLinearColor& Color, float Brightness)
{
	CoverNow = FMath::Clamp(Cover, 0.0f, 1.0f);
	if (!Material)
	{
		// The baked parent, when the project has it.
		const FString Package = ApexTrackMaterials::PackageName(ApexTrackMaterials::CloudName);
		UMaterialInterface* Parent = FPackageName::DoesPackageExist(Package)
			? LoadObject<UMaterialInterface>(nullptr, *ApexTrackMaterials::ObjectPath(ApexTrackMaterials::CloudName))
			: nullptr;
		if (!Parent)
		{
			if (!bDrawn)
			{
				UE_LOG(LogApexSim, Log, TEXT("No %s parent: the sky draws no cloud layer (bake it with -run=ApexMaterialBake)"),
					ApexTrackMaterials::CloudName);
			}
			bDrawn = false;
			Dome->SetVisibility(false);
			SetActorTickEnabled(false);
			return;
		}
		Material = UMaterialInstanceDynamic::Create(Parent, this);
		Dome->SetMaterial(0, Material);
		Material->SetScalarParameterValue(TEXT("CloudHeight"), CloudHeightCm);
	}
	Material->SetScalarParameterValue(TEXT("CloudCover"), CoverNow);
	Material->SetVectorParameterValue(TEXT("CloudColor"), Color);
	Material->SetScalarParameterValue(TEXT("CloudBrightness"), FMath::Max(Brightness, 0.0f));
	bDrawn = CoverNow > 0.01f;
	Dome->SetVisibility(bDrawn);
	SetActorTickEnabled(bDrawn);
}

void AApexCloudLayerActor::SetWind(const FVector& WindCmPerS)
{
	if (Material)
	{
		// A cloud a kilometre and a half up drifts with the wind up there,
		// which the material reads as a world offset growing with time.
		Material->SetVectorParameterValue(TEXT("CloudWind"), FLinearColor(WindCmPerS.X, WindCmPerS.Y, 0.0f, 0.0f));
	}
}

void AApexCloudLayerActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bDrawn)
	{
		return;
	}
	if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		SetActorLocation(Camera->GetCameraLocation());
	}
}
