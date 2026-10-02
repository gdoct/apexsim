#include "Race/ApexCarDriver.h"

#include "Cars/ApexCarContentSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

void FApexCarDriver::CreateComponent(UObject& Owner, USceneComponent* Body)
{
	Component = Owner.CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Driver"));
	Component->SetupAttachment(Body);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetMobility(EComponentMobility::Movable);
	// Hidden until a car says it has one.
	Component->SetVisibility(false);
}

void FApexCarDriver::SetSpec(const FApexDriverSpec& InSpec)
{
	Spec = InSpec;
	UStaticMesh* Loaded = Spec.IsUsable() ? ApexCarContent::LoadMesh(Spec.Mesh, Spec.RuntimeModel) : nullptr;
	bHasDriver = Loaded != nullptr;
	if (Component)
	{
		// Overrides are per slot index: the last car's livery must not land on this one.
		Component->EmptyOverrideMaterials();
		Component->SetStaticMesh(Loaded);
		// The body's frame: he sits where the GLB puts him.
		Component->SetRelativeTransform(FTransform::Identity);
	}
	Apply();
}

void FApexCarDriver::SetMeshVisible(bool bVisible)
{
	bMeshVisible = bVisible;
	Apply();
}

void FApexCarDriver::SetDriverVisible(bool bVisible)
{
	bDriverVisible = bVisible;
	Apply();
}

void FApexCarDriver::Apply()
{
	if (!Component)
	{
		return;
	}
	Component->SetVisibility(ApexDriver::IsDrawn(bHasDriver, bMeshVisible, bDriverVisible));
	Component->SetCastHiddenShadow(ApexDriver::CastsHiddenShadow(bHasDriver, bMeshVisible, bDriverVisible));
}
