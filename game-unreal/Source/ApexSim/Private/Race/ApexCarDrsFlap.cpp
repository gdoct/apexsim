#include "Race/ApexCarDrsFlap.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

namespace ApexDrs
{
	FTransform FlapTransform(const FApexDrsFlapSpec& Spec, float Open)
	{
		const float Angle = FMath::DegreesToRadians(Spec.OpenDeg) * FMath::Clamp(Open, 0.0f, 1.0f);
		// About X: +Y (the leading edge, ahead of a hinge at the trailing
		// edge) turns towards +Z for a positive angle.
		return FTransform(FQuat(FVector::XAxisVector, Angle),
			FVector(0.0f, Spec.HingeForwardM * 100.0f, Spec.HingeUpM * 100.0f));
	}

	float StepOpen(float Open, bool bOpen, float DeltaSeconds)
	{
		const float Step = FMath::Max(DeltaSeconds, 0.0f) / SwingSeconds;
		return FMath::Clamp(Open + (bOpen ? Step : -Step), 0.0f, 1.0f);
	}
}

void FApexCarDrsFlap::CreateComponent(UObject& Owner, USceneComponent* Body)
{
	Component = Owner.CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DrsFlap"));
	Component->SetupAttachment(Body);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetMobility(EComponentMobility::Movable);
	// Hidden until a car says it has one.
	Component->SetVisibility(false);
}

void FApexCarDrsFlap::SetSpec(const FApexDrsFlapSpec& InSpec)
{
	Spec = InSpec;
	UStaticMesh* Loaded = Spec.IsUsable() ? Spec.Mesh.LoadSynchronous() : nullptr;
	bHasFlap = Loaded != nullptr;
	Open = 0.0f;
	if (Component)
	{
		// Overrides are per slot index: the last car's livery must not land on this one.
		Component->EmptyOverrideMaterials();
		Component->SetStaticMesh(Loaded);
	}
	SetVisible(bVisible);
	Place();
}

void FApexCarDrsFlap::Update(bool bWantOpen, float DeltaSeconds)
{
	if (!bHasFlap)
	{
		return;
	}
	const float Next = ApexDrs::StepOpen(Open, bWantOpen, DeltaSeconds);
	if (Next != Open)
	{
		Open = Next;
		Place();
	}
}

void FApexCarDrsFlap::SetVisible(bool bInVisible)
{
	bVisible = bInVisible;
	if (Component)
	{
		Component->SetVisibility(bVisible && bHasFlap);
	}
}

void FApexCarDrsFlap::Place()
{
	if (bHasFlap && Component)
	{
		Component->SetRelativeTransform(ApexDrs::FlapTransform(Spec, Open));
	}
}
