#include "Race/ApexChaseBoom.h"

#include "Race/ApexChaseView.h"

void UApexChaseBoom::SetBody(const FTransform& InBodyToWorld, const FBox& InBodyBox)
{
	BodyToWorld = InBodyToWorld;
	BodyBox = InBodyBox;
}

void UApexChaseBoom::ClearBody()
{
	BodyBox = FBox(ForceInit);
}

void UApexChaseBoom::UpdateDesiredArmLocation(bool bDoTrace, bool bDoLocationLag, bool bDoRotationLag, float DeltaTime)
{
	Super::UpdateDesiredArmLocation(bDoTrace, bDoLocationLag, bDoRotationLag, DeltaTime);
	if (!BodyBox.IsValid)
	{
		return;
	}

	const FVector World = GetComponentTransform().TransformPosition(RelativeSocketLocation);
	const FVector Local = BodyToWorld.InverseTransformPosition(World);
	const FVector Clear = ApexChase::KeepOutOfBody(Local, BodyBox);
	if (Clear == Local)
	{
		return;
	}
	RelativeSocketLocation = GetComponentTransform().InverseTransformPosition(BodyToWorld.TransformPosition(Clear));
	UpdateChildTransforms();
}
